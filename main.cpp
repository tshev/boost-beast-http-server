#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <unordered_map>
#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <list>
#include <memory>
#include <string>

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

template<typename Application>
class http_worker
{
public:
    typedef http::request<http::string_body> request_type;
    typedef http::response<http::string_body> response_type;

    http_worker(http_worker const&) = delete;
    http_worker& operator=(http_worker const&) = delete;
    http_worker(http_worker&&) = default;

    http_worker(tcp::acceptor& acceptor, Application& app) :
        acceptor_(&acceptor),
        application_(&app)
    {
        start();
    }

    void start()
    {
        accept();
        check_deadline();
    }

private:
    //using request_body_t = http::basic_dynamic_body<beast::flat_static_buffer<1024 * 1024>>;
    using request_body_t = http::string_body;

    tcp::acceptor* acceptor_;
    Application* application_;
    tcp::socket socket_{acceptor_->get_executor()};
    beast::flat_static_buffer<8192> buffer_;
    boost::optional<http::request_parser<request_body_t>> parser_;
    net::basic_waitable_timer<std::chrono::steady_clock> request_deadline_{acceptor_->get_executor(), (std::chrono::steady_clock::time_point::max)()};
    boost::optional<http::response<http::string_body>> string_response_;
    boost::optional<http::response_serializer<http::string_body>> string_serializer_;

    void accept() {
        beast::error_code ec;
        socket_.close(ec);
        buffer_.consume(buffer_.size());

        acceptor_->async_accept(socket_, [this](beast::error_code ec) {
            if (ec) {
                accept();
            } else {
                request_deadline_.expires_after(std::chrono::seconds(60));
                read_request();
            }
        });
    }

    void read_request() {
        parser_.emplace(std::piecewise_construct, std::make_tuple());

        http::async_read(socket_, buffer_, *parser_, [this](beast::error_code ec, std::size_t) {
            if (ec) {
                accept();
            } else {
                process_request(parser_->get());
            }
        });
    }

    void process_request(http::request<request_body_t> const& req) {
        string_response_.emplace(std::piecewise_construct, std::make_tuple());
        application_->operator()(req, string_response_.get());
        string_response_->prepare_payload();
        string_serializer_.emplace(*string_response_);

        http::async_write(socket_, *string_serializer_, [this](beast::error_code ec, std::size_t) {
            socket_.shutdown(tcp::socket::shutdown_send, ec);
            string_serializer_.reset();
            string_response_.reset();
            accept();
        });
    }

    void check_deadline() {
        // The deadline may have moved, so check it has really passed.
        if (request_deadline_.expiry() <= std::chrono::steady_clock::now()) {
            // Close socket to cancel any outstanding operation.
            beast::error_code ec;
            socket_.close();

            // Sleep indefinitely until we're given a new deadline.
            request_deadline_.expires_at(std::chrono::steady_clock::time_point::max());
        }

        request_deadline_.async_wait([this](beast::error_code) {
            check_deadline();
        });
    }
};



template<typename T>
class http_server {
public:
    typedef http::request<http::string_body> request_type;
    typedef http::response<http::string_body> response_type;

    struct concurrency {
        size_t value;
    };

    struct workers_count {
        typedef size_t value_type;
        size_t value;
    };

    struct port {
        uint16_t value;
    };

    struct address {
        std::string value;
    };

    struct max_request_bytes {
        size_t value;
    };

    struct config {
        http_server::address address;
        http_server::port port;
        http_server::workers_count workers_count;;
        http_server::concurrency concurrency;
        http_server::max_request_bytes max_request_bytes;
    };

    http_server(const http_server::config& config, T router) :
        config_(config),
        ioc_(config.concurrency.value),
        acceptor_{ioc_, {net::ip::make_address(config.address.value), config.port.value}},
        router_(router) {

        for (size_t i = 0; i < config.workers_count.value; ++i) {
            workers_.emplace_back(acceptor_, router_);
        }
    }

    void poll() {
        ioc_.poll();
    }
    
private:
    http_server::config config_;
    net::io_context ioc_;
    tcp::acceptor acceptor_;
    T router_;
    std::list<http_worker<T>> workers_;
};


class http_router {
public:
    typedef http::request<http::string_body> request_type;
    typedef http::response<http::string_body> response_type;
    typedef void return_type;

private:
    std::unordered_map<std::string, std::function<void(request_type const&, response_type&)>> routes_;
public:
    http_router() = default; 
    http_router(const http_router&) = default;
    http_router(http_router&&) = default;

    return_type operator()(request_type const& request, response_type& response) {
        response.keep_alive(true);
        const std::string target(request.target());
        auto handle = routes_.find(target);
        if (handle != std::end(routes_)) {
            return handle->second(request, response);
        }
        response.result(http::status::not_implemented);
    }

    template<typename H>
    void emplace(const std::string& path, H handle) {
        routes_.emplace(path, handle);
    }
};

int main(int argc, char* argv[]) {
    try {
        typedef http_server<http_router> server_type;
        server_type::config server_config{
            server_type::address{"0.0.0.0"},
            server_type::port{4000},
            server_type::workers_count{1024},
            server_type::concurrency{1},
            server_type::max_request_bytes{8192}
        };
        http_router router{};
        router.emplace("/", [](server_type::request_type const& request, server_type::response_type& response) {
            response.body() = R"({"message": "Hello world!"})" ;
            response.result(http::status::ok);
            response.set(http::field::content_type, "application/json");
        });
        server_type server{server_config, router};
        for (;;) server.poll();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}

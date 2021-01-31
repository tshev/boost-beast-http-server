# boost-beast-http-server

## Dependencies
- boost beast
- boost asio

## Compile
```bash
g++ main.cpp  -pthread -O3 -o server
./server # Starts a server, which listens port 4000
```

```bash
curl -w "\n" http://localhost:4000 
```

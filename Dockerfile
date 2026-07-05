cat > Dockerfile << 'EOF'
FROM gcc:latest

WORKDIR /app
COPY . .

RUN g++ -std=c++17 -DASIO_STANDALONE main.cpp -o server -lpthread

EXPOSE 10000

CMD ["./server"]
EOF
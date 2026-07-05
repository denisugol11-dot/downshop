FROM gcc:latest

WORKDIR /app

RUN apt-get update && apt-get install -y \
    libasio-dev \
    libcurl4-openssl-dev \
    nlohmann-json3-dev

COPY . .

RUN g++ -std=c++17 -DASIO_STANDALONE main.cpp -o server -lpthread -lcurl

EXPOSE 10000

CMD ["./server"]
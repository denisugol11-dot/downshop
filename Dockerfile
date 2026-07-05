FROM gcc:latest

WORKDIR /app

# Устанавливаем Asio перед компиляцией
RUN apt-get update && apt-get install -y libasio-dev

COPY . .

RUN g++ -std=c++17 -DASIO_STANDALONE main.cpp -o server -lpthread

EXPOSE 10000

CMD ["./server"]
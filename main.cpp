#include "crow_all.h"

int main() {
    crow::SimpleApp app;

    // Главная страница
    CROW_ROUTE(app, "/")([]() {
        return "<h1>Добро пожаловать в мой магазин!</h1>"
               "<p><a href='/items'>Перейти к товарам</a></p>";
    });

    // Страница с товарами и картинками
    CROW_ROUTE(app, "/items")([]() {
        return R"(
            <html>
            <head>
                <title>Товары</title>
                <style>
                    body { font-family: Arial, sans-serif; background: #f4f4f4; padding: 20px; }
                    .product { 
                        background: white; 
                        border-radius: 10px; 
                        padding: 15px; 
                        margin-bottom: 15px; 
                        max-width: 300px;
                        box-shadow: 0 2px 5px rgba(0,0,0,0.1);
                    }
                    .product img { width: 100%; border-radius: 8px; }
                    .price { color: green; font-weight: bold; font-size: 18px; }
                </style>
            </head>
            <body>
                <h1>Наши товары</h1>

                <div class="product">
                    <img src="/static/tshirt.jpg" alt="Футболка">
                    <h3>Футболка</h3>
                    <p class="price">$15.99</p>
                </div>

                <div class="product">
                    <img src="/static/mug.jpg" alt="Кружка">
                    <h3>Кружка</h3>
                    <p class="price">$8.50</p>
                </div>

            </body>
            </html>
        )";
    });

    // Отдаём файлы из папки static (картинки, css и т.д.)
    CROW_ROUTE(app, "/static/<string>")([](std::string filename) {
        crow::response res;
        res.set_static_file_info("static/" + filename);
        return res;
    });

    app.port(18080).multithreaded().run();
}
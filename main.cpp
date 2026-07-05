#include "crow_all.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <string>
#include <map>
#include <vector>

using json = nlohmann::json;

// ==== Каталог товаров (хранится на сервере — цену клиенту не доверяем!) ====
struct Product {
    std::string name;
    int price_cents; // цена в центах (1599 = $15.99)
    std::vector<std::string> colors;
};

std::map<std::string, Product> products = {
    {"tshirt", {"Футболка", 1599, {"Белый", "Чёрный", "Синий"}}},
    {"mug",    {"Кружка",   850,  {"Белый", "Красный"}}}
};

// ==== Функция для сбора ответа от curl в строку ====
size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* out) {
    out->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// ==== URL-кодирование (нужно для отправки данных в Stripe) ====
std::string urlEncode(const std::string& value) {
    CURL* curl = curl_easy_init();
    char* output = curl_easy_escape(curl, value.c_str(), value.length());
    std::string result(output);
    curl_free(output);
    curl_easy_cleanup(curl);
    return result;
}

// ==== Создание сессии оплаты Stripe ====
std::string createCheckoutSession(const std::vector<std::pair<std::string, std::string>>& cartItems) {
    // cartItems = [(product_id, color), ...]

    std::string secretKey = std::getenv("STRIPE_SECRET_KEY") ? std::getenv("STRIPE_SECRET_KEY") : "";
    if (secretKey.empty()) {
        return "";
    }

    std::string domain = std::getenv("SITE_URL") ? std::getenv("SITE_URL") : "http://localhost:18080";

    // Собираем тело запроса в формате Stripe (form-urlencoded с индексами)
    std::string postFields;
    int index = 0;
    for (const auto& item : cartItems) {
        std::string productId = item.first;
        std::string color = item.second;

        if (products.find(productId) == products.end()) continue;
        Product p = products[productId];

        std::string prefix = "line_items[" + std::to_string(index) + "]";

        postFields += prefix + "[price_data][currency]=usd&";
        postFields += prefix + "[price_data][product_data][name]=" + urlEncode(p.name + " (" + color + ")") + "&";
        postFields += prefix + "[price_data][unit_amount]=" + std::to_string(p.price_cents) + "&";
        postFields += prefix + "[quantity]=1&";

        index++;
    }

    postFields += "mode=payment&";
    postFields += "success_url=" + urlEncode(domain + "/success") + "&";
    postFields += "cancel_url=" + urlEncode(domain + "/cancel");

    CURL* curl = curl_easy_init();
    std::string response;

    if (curl) {
        struct curl_slist* headers = NULL;
        std::string authHeader = "Authorization: Bearer " + secretKey;
        headers = curl_slist_append(headers, authHeader.c_str());

        curl_easy_setopt(curl, CURLOPT_URL, "https://api.stripe.com/v1/checkout/sessions");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postFields.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            return "";
        }
    }

    try {
        json j = json::parse(response);
        if (j.contains("url")) {
            return j["url"].get<std::string>();
        }
    } catch (...) {
        return "";
    }

    return "";
}

int main() {
    crow::SimpleApp app;

    // ==== Главная страница с каталогом ====
    CROW_ROUTE(app, "/")([]() {
        crow::response res;
        res.set_header("Content-Type", "text/html; charset=utf-8");

        std::string html = R"(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>Магазин</title>
<style>
    body { font-family: Arial, sans-serif; background: #f4f4f4; padding: 20px; margin: 0; }
    h1 { text-align: center; }
    .products { display: flex; flex-wrap: wrap; gap: 20px; justify-content: center; }
    .product { background: white; border-radius: 10px; padding: 15px; width: 250px; box-shadow: 0 2px 5px rgba(0,0,0,0.1); }
    .product img { width: 100%; border-radius: 8px; }
    .price { color: green; font-weight: bold; font-size: 18px; }
    select, button { padding: 8px; margin-top: 8px; width: 100%; border-radius: 6px; border: 1px solid #ccc; }
    button { background: #2563eb; color: white; border: none; cursor: pointer; font-weight: bold; }
    button:hover { background: #1d4ed8; }
    #cart-box { max-width: 400px; margin: 30px auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 5px rgba(0,0,0,0.1); }
    .cart-item { display: flex; justify-content: space-between; padding: 8px 0; border-bottom: 1px solid #eee; }
    .remove-btn { color: red; cursor: pointer; background: none; border: none; width: auto; padding: 0 8px; }
    #checkout-btn { background: #16a34a; margin-top: 15px; }
    #checkout-btn:hover { background: #15803d; }
</style>
</head>
<body>

<h1>Наш магазин</h1>

<div class="products">

    <div class="product">
        <img src="/static/tshirt.jpg" alt="Футболка">
        <h3>Футболка - $15.99</h3>
        <select id="color-tshirt">
            <option value="Белый">Белый</option>
            <option value="Чёрный">Чёрный</option>
            <option value="Синий">Синий</option>
        </select>
        <button onclick="addToCart('tshirt', 'Футболка', 15.99)">Добавить в корзину</button>
    </div>

    <div class="product">
        <img src="/static/mug.jpg" alt="Кружка">
        <h3>Кружка - $8.50</h3>
        <select id="color-mug">
            <option value="Белый">Белый</option>
            <option value="Красный">Красный</option>
        </select>
        <button onclick="addToCart('mug', 'Кружка', 8.50)">Добавить в корзину</button>
    </div>

</div>

<div id="cart-box">
    <h2>Корзина</h2>
    <div id="cart-items"></div>
    <p><b>Итого: $<span id="cart-total">0.00</span></b></p>
    <button id="checkout-btn" onclick="checkout()">Оплатить картой</button>
</div>

<script>
    let cart = JSON.parse(localStorage.getItem('cart') || '[]');

    function addToCart(id, name, price) {
        const colorSelect = document.getElementById('color-' + id);
        const color = colorSelect.value;
        cart.push({ id, name, color, price });
        saveCart();
    }

    function removeFromCart(index) {
        cart.splice(index, 1);
        saveCart();
    }

    function saveCart() {
        localStorage.setItem('cart', JSON.stringify(cart));
        renderCart();
    }

    function renderCart() {
        const container = document.getElementById('cart-items');
        container.innerHTML = '';
        let total = 0;
        cart.forEach((item, index) => {
            total += item.price;
            container.innerHTML += `
                <div class="cart-item">
                    <span>${item.name} (${item.color}) - $${item.price.toFixed(2)}</span>
                    <button class="remove-btn" onclick="removeFromCart(${index})">✕</button>
                </div>
            `;
        });
        document.getElementById('cart-total').innerText = total.toFixed(2);
    }

    async function checkout() {
        if (cart.length === 0) {
            alert('Корзина пуста!');
            return;
        }

        const items = cart.map(item => ({ id: item.id, color: item.color }));

        const response = await fetch('/create-checkout-session', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ items })
        });

        const data = await response.json();

        if (data.url) {
            localStorage.removeItem('cart');
            window.location.href = data.url;
        } else {
            alert('Ошибка при создании оплаты. Попробуй ещё раз.');
        }
    }

    renderCart();
</script>

</body>
</html>
        )";

        res.write(html);
        return res;
    });

    // ==== Endpoint для создания оплаты ====
    CROW_ROUTE(app, "/create-checkout-session").methods("POST"_method)([](const crow::request& req) {
        crow::response res;
        res.set_header("Content-Type", "application/json");

        try {
            json body = json::parse(req.body);
            std::vector<std::pair<std::string, std::string>> cartItems;

            for (auto& item : body["items"]) {
                std::string id = item["id"];
                std::string color = item["color"];
                cartItems.push_back({id, color});
            }

            std::string checkoutUrl = createCheckoutSession(cartItems);

            if (checkoutUrl.empty()) {
                res.code = 500;
                res.write(R"({"error": "failed to create session"})");
                return res;
            }

            json responseJson = { {"url", checkoutUrl} };
            res.write(responseJson.dump());
        } catch (...) {
            res.code = 400;
            res.write(R"({"error": "invalid request"})");
        }

        return res;
    });

    // ==== Страницы успеха и отмены ====
    CROW_ROUTE(app, "/success")([]() {
        crow::response res;
        res.set_header("Content-Type", "text/html; charset=utf-8");
        res.write("<h1>Спасибо за покупку!</h1><p>Оплата прошла успешно.</p><a href='/'>Вернуться в магазин</a>");
        return res;
    });

    CROW_ROUTE(app, "/cancel")([]() {
        crow::response res;
        res.set_header("Content-Type", "text/html; charset=utf-8");
        res.write("<h1>Оплата отменена</h1><a href='/'>Вернуться в магазин</a>");
        return res;
    });

    // ==== Статика (картинки) ====
    CROW_ROUTE(app, "/static/<string>")([](std::string filename) {
        crow::response res;
        res.set_static_file_info("static/" + filename);
        return res;
    });

    const char* port_env = std::getenv("PORT");
    int port = port_env ? std::stoi(port_env) : 18080;

    app.port(port).multithreaded().run();
}
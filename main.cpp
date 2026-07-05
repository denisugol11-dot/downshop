#include "crow_all.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <string>
#include <map>
#include <vector>
#include <tuple>

using json = nlohmann::json;

// ==== Каталог товаров (хранится на сервере — цену клиенту не доверяем!) ====
struct Product {
    std::string name;
    int price_cents; // цена в центах (3000 = $30.00)
    std::vector<std::string> colors;
    bool has_sizes;
};

std::map<std::string, Product> products = {
    {"tshirt",  {"Футболка Palm Angels graffiti",         3000, {"Бело-серый", "Чёрно-синий", "Бело-красный"}, true}},
    {"tshirt3",     {"Футболка Lanvin",           4000,  {"Белый", "Черный"}, false}},
    {"tshirt1",  {"Футболка Lanvin&Gallery Dept",    30000, {"Белый", "Чёрный"}, true}},
    {"tshirt2",  {"Футболка Marcelo Burlon",      3000, {"Чёрный", "Серый", "Белый"}, true}},
    {"shorts1", {"Шорты Palm Angels",     3000, {"Чёрный", "Серый", "Белый"}, true}},
    {"shorts2", {"Шорты Sprayground",      3000, {"Чёрный", "Серый"}, true}},
    {"pants1",  {"Штаны Спортивные gallery dept",    3500, {"Чёрный", "Серый"}, true}},
    {"pants2",  {"Штаны Purple brand flared",      4000, {"Чёрный", "Серый"}, true}},
    {"shoes1",  {"Кроссовки Jordan5",    9000, {"Белый", "Чёрный", "Красный", "Черно-синий"}, true}},
    {"shoes2",  {"Кроссовки Off white be right back", 10000, {"Чёрный", "Красный", "Белый", "Зеленый", "Бело-синий"}, true}},
    {"shoes3",  {"Кроссовки Nike Acronym",  12000, {"Белый", "Чёрный", "Бело-оранжевый"}, true}}
};

// ==== Соответствие цвета -> ключ для имени файла картинки ====
std::map<std::string, std::string> colorKeys = {
    {"Белый", "white"},
    {"Чёрный", "black"},
    {"Серый", "gray"},
    {"Красный", "red"},
    {"Синий", "blue"},
    {"Зелёный", "green"},
    {"Бело-серый", "white-gray"},
    {"Чёрно-синий", "black-blue"},
    {"Бело-красный", "white-red"},
    {"Бело-синий", "white-blue"},
    {"Бело-оранжевый", "white-orange"}
};

// ==== Стоимость доставки (в центах) ====
std::map<std::string, int> deliveryOptions = {
    {"Самовывоз", 0},
    {"Курьер по Клайпеде", 500},
    {"Почта Европы", 1500}
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
// cartItems = [(product_id, color, size), ...]
std::string createCheckoutSession(const std::vector<std::tuple<std::string, std::string, std::string>>& cartItems,
                                   const std::string& deliveryMethod) {

    std::string secretKey = std::getenv("STRIPE_SECRET_KEY") ? std::getenv("STRIPE_SECRET_KEY") : "";
    if (secretKey.empty()) {
        return "";
    }

    std::string domain = std::getenv("SITE_URL") ? std::getenv("SITE_URL") : "http://localhost:18080";

    std::string postFields;
    int index = 0;
    for (const auto& item : cartItems) {
        std::string productId = std::get<0>(item);
        std::string color = std::get<1>(item);
        std::string size = std::get<2>(item);

        if (products.find(productId) == products.end()) continue;
        Product p = products[productId];

        std::string label = p.name + " (" + color;
        if (!size.empty()) label += ", размер " + size;
        label += ")";

        std::string prefix = "line_items[" + std::to_string(index) + "]";

        postFields += prefix + "[price_data][currency]=usd&";
        postFields += prefix + "[price_data][product_data][name]=" + urlEncode(label) + "&";
        postFields += prefix + "[price_data][unit_amount]=" + std::to_string(p.price_cents) + "&";
        postFields += prefix + "[quantity]=1&";

        index++;
    }

    // Добавляем доставку отдельной строкой
    if (deliveryOptions.find(deliveryMethod) != deliveryOptions.end()) {
        int deliveryCost = deliveryOptions[deliveryMethod];
        std::string prefix = "line_items[" + std::to_string(index) + "]";

        postFields += prefix + "[price_data][currency]=usd&";
        postFields += prefix + "[price_data][product_data][name]=" + urlEncode("Доставка: " + deliveryMethod) + "&";
        postFields += prefix + "[price_data][unit_amount]=" + std::to_string(deliveryCost) + "&";
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

        std::string html = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>DOWNSHOP</title>
<style>
    body { font-family: 'Segoe UI', Arial, sans-serif; background: #f4f4f4; padding: 20px; margin: 0; }
    h1.logo { text-align:center; font-size: 48px; font-weight: 800; letter-spacing: 3px; background: linear-gradient(90deg, #2563eb, #16a34a); -webkit-background-clip: text; background-clip: text; color: transparent; margin-bottom: 10px; }
    .contact-block { text-align:center; margin-bottom: 25px; }
    .contact-block a { display:inline-flex; align-items:center; gap:8px; background:#229ED9; color:white; padding:10px 20px; border-radius:30px; text-decoration:none; font-weight:bold; font-size:15px; }
    .contact-block p { max-width:500px; margin:15px auto 0; font-size:14px; color:#555; line-height:1.5; }
    .products { display: flex; flex-wrap: wrap; gap: 20px; justify-content: center; }
    .product { background: white; border-radius: 10px; padding: 15px; width: 250px; box-shadow: 0 2px 5px rgba(0,0,0,0.1); }
    .product img { width: 100%; height: 200px; object-fit: cover; border-radius: 8px; }
    .price { color: green; font-weight: bold; font-size: 18px; }
    select, button { padding: 8px; margin-top: 8px; width: 100%; border-radius: 6px; border: 1px solid #ccc; }
    button { background: #2563eb; color: white; border: none; cursor: pointer; font-weight: bold; }
    button:hover { background: #1d4ed8; }
    #cart-box { max-width: 420px; margin: 30px auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 5px rgba(0,0,0,0.1); }
    .cart-item { display: flex; justify-content: space-between; padding: 8px 0; border-bottom: 1px solid #eee; }
    .remove-btn { color: red; cursor: pointer; background: none; border: none; width: auto; padding: 0 8px; }
    #checkout-btn { background: #16a34a; margin-top: 15px; }
    #checkout-btn:hover { background: #15803d; }
    label { font-size: 13px; color: #555; margin-top: 6px; display: block; }
</style>
</head>
<body>

<h1 class="logo">DOWNSHOP</h1>
<div class="contact-block">
    <a href="https://t.me/bahurzd1" target="_blank">
        <svg width="20" height="20" viewBox="0 0 24 24" fill="white"><path d="M12 0C5.373 0 0 5.373 0 12s5.373 12 12 12 12-5.373 12-12S18.627 0 12 0zm5.562 8.248-1.97 9.29c-.145.658-.537.818-1.084.508l-3-2.212-1.448 1.394c-.16.16-.295.295-.605.295l.213-3.053 5.56-5.023c.242-.213-.054-.334-.373-.121l-6.871 4.326-2.962-.924c-.643-.204-.657-.643.136-.953l11.57-4.461c.538-.196 1.006.128.834.334z"/></svg>
        Написать в Telegram
    </a>
    <p>Не нашли нужную вещь или цвет? Свяжитесь с нами — мы сможем сделать специальный заказ для вас.</p>
</div>

<div class="products" id="products-container"></div>

<div id="cart-box">
    <h2>Корзина</h2>
    <div id="cart-items"></div>

    <label>Способ доставки:</label>
    <select id="delivery-method" onchange="renderCart()">
        <option value="Самовывоз">Самовывоз — бесплатно</option>
        <option value="Курьер по Клайпеде">Курьер по Клайпеде — $5.00</option>
        <option value="Почта Европы">Почта Европы — $15.00</option>
    </select>

    <p><b>Итого: $<span id="cart-total">0.00</span></b></p>
    <button id="checkout-btn" onclick="checkout()">Оплатить картой</button>
</div>

<script>
    // Каждый цвет имеет свой ключ (для имени файла картинки: {id}-{key}.jpg) и отображаемое имя
    const products = [
        { id: "tshirt",  name: "Футболка Palm Angels graffiti", price: 30.00, sizes: ["S","M","L","XL"],
          colors: [ {name:"Бело-серый", key:"white-gray"}, {name:"Чёрно-синий", key:"black-blue"}, {name:"Бело-красный", key:"white-red"} ] },
        { id: "tshirt3", name: "Футболка Lanvin",  price: 40.00, sizes: null,
          colors: [ {name:"Белый", key:"white"}, {name:"Чёрный", key:"black"} ] },
        { id: "tshirt1", name: "Футболка Lanvin&Gallery Dept", price: 300.00, sizes: ["S","M","L","XL"],
          colors: [ {name:"Белый", key:"white"}, {name:"Чёрный", key:"black"} ] },
        { id: "tshirt2", name: "Футболка Marcelo Burlon",    price: 30.00, sizes: ["S","M","L","XL"],
          colors: [ {name:"Чёрный", key:"black"}, {name:"Серый", key:"gray"}, {name:"Белый", key:"white"} ] },
        { id: "shorts1", name: "Шорты Palm Angels",  price: 30.00, sizes: ["S","M","L","XL"],
          colors: [ {name:"Чёрный", key:"black"}, {name:"Серый", key:"gray"}, {name:"Белый", key:"white"} ] },
        { id: "shorts2", name: "Шорты Sprayground",       price: 30.00, sizes: ["S","M","L","XL"],
          colors: [ {name:"Чёрный", key:"black"}, {name:"Серый", key:"gray"} ] },
        { id: "pants1",  name: "Штаны Спортивные gallery dept",  price: 35.00, sizes: ["S","M","L","XL"],
          colors: [ {name:"Чёрный", key:"black"}, {name:"Серый", key:"gray"} ] },
        { id: "pants2",  name: "Штаны Purple brand flared",        price: 40.00, sizes: ["S","M","L","XL"],
          colors: [ {name:"Чёрный", key:"black"}, {name:"Серый", key:"gray"} ] },
        { id: "shoes1",  name: "Кроссовки Jordan5", price: 90.00, sizes: ["38","39","40","41","42","43","44"],
          colors: [ {name:"Белый", key:"white"}, {name:"Чёрный", key:"black"}, {name:"Красный", key:"red"}, {name:"Чёрно-синий", key:"black-blue"} ] },
        { id: "shoes2",  name: "Кроссовки Off white be right back",    price: 100.00, sizes: ["38","39","40","41","42","43","44"],
          colors: [ {name:"Чёрный", key:"black"}, {name:"Красный", key:"red"}, {name:"Белый", key:"white"}, {name:"Зелёный", key:"green"}, {name:"Бело-синий", key:"white-blue"} ] },
        { id: "shoes3",  name: "Кроссовки Nike Acronym",  price: 120.00, sizes: ["38","39","40","41","42","43","44"],
          colors: [ {name:"Белый", key:"white"}, {name:"Чёрный", key:"black"}, {name:"Бело-оранжевый", key:"white-orange"} ] }
    ];

    const deliveryPrices = {
        "Самовывоз": 0,
        "Курьер по Клайпеде": 5.00,
        "Почта Европы": 15.00
    };

    let cart = JSON.parse(localStorage.getItem('cart') || '[]');

    function productImage(id, colorKey) {
        return `/static/${id}-${colorKey}.jpg`;
    }

    function renderProducts() {
        const container = document.getElementById('products-container');
        container.innerHTML = '';

        products.forEach(p => {
            const colorOptions = p.colors.map(c => `<option value="${c.key}">${c.name}</option>`).join('');
            const sizeBlock = p.sizes
                ? `<label>Размер:</label>
                   <select id="size-${p.id}">
                      ${p.sizes.map(s => `<option value="${s}">${s}</option>`).join('')}
                   </select>`
                : '';

            container.innerHTML += `
                <div class="product">
                    <img id="img-${p.id}" src="${productImage(p.id, p.colors[0].key)}" alt="${p.name}"
                         onerror="this.onerror=null; this.src='/static/${p.id}.jpg'">
                    <h3>${p.name} - $${p.price.toFixed(2)}</h3>
                    <label>Цвет:</label>
                    <select id="color-${p.id}" onchange="changeImage('${p.id}')">${colorOptions}</select>
                    ${sizeBlock}
                    <button onclick="addToCart('${p.id}')">Добавить в корзину</button>
                </div>
            `;
        });
    }

    function changeImage(id) {
        const colorKey = document.getElementById('color-' + id).value;
        const img = document.getElementById('img-' + id);
        img.onerror = function() { this.onerror = null; this.src = `/static/${id}.jpg`; };
        img.src = productImage(id, colorKey);
    }

    function addToCart(id) {
        const product = products.find(p => p.id === id);
        const colorKey = document.getElementById('color-' + id).value;
        const colorObj = product.colors.find(c => c.key === colorKey);
        const sizeSelect = document.getElementById('size-' + id);
        const size = sizeSelect ? sizeSelect.value : '';

        cart.push({ id, name: product.name, color: colorObj.name, size, price: product.price });
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
            const sizeText = item.size ? `, размер ${item.size}` : '';
            container.innerHTML += `
                <div class="cart-item">
                    <span>${item.name} (${item.color}${sizeText}) - $${item.price.toFixed(2)}</span>
                    <button class="remove-btn" onclick="removeFromCart(${index})">✕</button>
                </div>
            `;
        });

        const deliveryMethod = document.getElementById('delivery-method').value;
        const deliveryCost = deliveryPrices[deliveryMethod] || 0;
        total += deliveryCost;

        document.getElementById('cart-total').innerText = total.toFixed(2);
    }

    async function checkout() {
        if (cart.length === 0) {
            alert('Корзина пуста!');
            return;
        }

        const items = cart.map(item => ({ id: item.id, color: item.color, size: item.size }));
        const deliveryMethod = document.getElementById('delivery-method').value;

        const response = await fetch('/create-checkout-session', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ items, delivery: deliveryMethod })
        });

        const data = await response.json();

        if (data.url) {
            localStorage.removeItem('cart');
            window.location.href = data.url;
        } else {
            alert('Ошибка при создании оплаты. Попробуй ещё раз.');
        }
    }

    renderProducts();
    renderCart();
</script>

</body>
</html>
        )HTML";

        res.write(html);
        return res;
    });

    // ==== Endpoint для создания оплаты ====
    CROW_ROUTE(app, "/create-checkout-session").methods("POST"_method)([](const crow::request& req) {
        crow::response res;
        res.set_header("Content-Type", "application/json");

        try {
            json body = json::parse(req.body);
            std::vector<std::tuple<std::string, std::string, std::string>> cartItems;

            for (auto& item : body["items"]) {
                std::string id = item["id"];
                std::string color = item["color"];
                std::string size = item.contains("size") ? item["size"].get<std::string>() : "";
                cartItems.push_back({id, color, size});
            }

            std::string deliveryMethod = body.contains("delivery") ? body["delivery"].get<std::string>() : "Самовывоз";

            std::string checkoutUrl = createCheckoutSession(cartItems, deliveryMethod);

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
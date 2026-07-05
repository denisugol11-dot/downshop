#include "crow_all.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <string>
#include <map>
#include <vector>
#include <tuple>
#include <mutex>

using json = nlohmann::json;

// ==== Каталог товаров ====
struct Product {
    std::string name;
    int price_cents;
    std::vector<std::string> colors;
    bool has_sizes;
};

std::map<std::string, Product> products = {
    {"tshirt",  {"Футболка Palm Angels graffiti",         3000, {"Бело-серый", "Чёрно-синий", "Бело-красный"}, true}},
    {"tshirt3",     {"Футболка Lanvin",           4000,  {"Белый", "Черный"}, false}},
    {"tshirt1",  {"Футболка Lanvin&Gallery Dept",    3000, {"Белый", "Чёрный"}, true}},
    {"tshirt2",  {"Футболка Marcelo Burlon",      3000, {"Чёрный", "Серый", "Белый"}, true}},
    {"shorts1", {"Шорты Palm Angels",     3000, {"Чёрный", "Серый", "Белый"}, true}},
    {"shorts2", {"Шорты Sprayground",      3000, {"Чёрный", "Серый"}, true}},
    {"pants1",  {"Штаны Спортивные gallery dept",    3500, {"Чёрный", "Серый"}, true}},
    {"pants2",  {"Штаны Purple brand flared",      4000, {"Чёрный", "Серый"}, true}},
    {"shoes1",  {"Кроссовки Jordan5",    9000, {"Белый", "Чёрный", "Красный", "Черно-синий"}, true}},
    {"shoes2",  {"Кроссовки Off white be right back", 10000, {"Чёрный", "Красный", "Белый", "Зеленый", "Бело-синий"}, true}},
    {"shoes3",  {"Кроссовки Nike Acronym",  12000, {"Белый", "Чёрный", "Бело-оранжевый"}, true}}
};

// ==== Стоимость доставки (в центах) ====
std::map<std::string, int> deliveryOptions = {
    {"Самовывоз", 0},
    {"DPD", 600},
    {"Omniva", 450}
};

// ==== Кэш терминалов Omniva (загружается один раз при старте сервера) ====
struct OmnivaLocation {
    std::string id;
    std::string name;
    std::string country;
    std::string address;
};
std::vector<OmnivaLocation> omnivaCache;
std::mutex omnivaCacheMutex;

size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* out) {
    out->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string urlEncode(const std::string& value) {
    CURL* curl = curl_easy_init();
    char* output = curl_easy_escape(curl, value.c_str(), value.length());
    std::string result(output);
    curl_free(output);
    curl_easy_cleanup(curl);
    return result;
}

// ==== Загрузка публичного списка терминалов Omniva ====
void loadOmnivaLocations() {
    CURL* curl = curl_easy_init();
    std::string response;

    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "https://www.omniva.ee/locations.json");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }

    try {
        json j = json::parse(response);
        std::vector<OmnivaLocation> temp;

        for (auto& item : j) {
            std::string country = item.value("A0_NAME", "");
            std::string type = item.value("TYPE", "0");

            // Берём только автоматы (TYPE 0) в Литве, Латвии и Эстонии
            if (type != "0") continue;
            if (country != "LT" && country != "LV" && country != "EE") continue;

            OmnivaLocation loc;
            loc.id = item.value("ZIP", "");
            loc.name = item.value("NAME", "");
            loc.country = country;

            std::string a2 = item.value("A2_NAME", "");
            std::string a3 = item.value("A3_NAME", "");
            std::string a5 = item.value("A5_NAME", "");
            std::string a7 = item.value("A7_NAME", "");

            std::string address = a2;
            if (!a3.empty()) address += (address.empty() ? "" : ", ") + a3;
            if (!a5.empty()) address += (address.empty() ? "" : ", ") + a5;
            if (!a7.empty()) address += " " + a7;

            loc.address = address;
            temp.push_back(loc);
        }

        std::lock_guard<std::mutex> lock(omnivaCacheMutex);
        omnivaCache = temp;
    } catch (...) {
        // Если загрузка не удалась, кэш останется пустым — endpoint вернёт пустой список
    }
}

// ==== Создание сессии оплаты Stripe ====
std::string createCheckoutSession(const std::vector<std::tuple<std::string, std::string, std::string>>& cartItems,
                                   const std::string& deliveryMethod,
                                   const std::string& address,
                                   const std::string& email,
                                   const std::string& telegram) {

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
    postFields += "cancel_url=" + urlEncode(domain + "/cancel") + "&";

    if (!email.empty()) {
        postFields += "customer_email=" + urlEncode(email) + "&";
    }

    std::string description;
    if (!address.empty()) description += "Доставка: " + address + ". ";
    if (!telegram.empty()) description += "Telegram: " + telegram + ". ";

    if (!description.empty()) {
        postFields += "payment_intent_data[description]=" + urlEncode(description) + "&";
    }

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

    // Загружаем список терминалов Omniva один раз при старте сервера
    loadOmnivaLocations();

    // ==== Endpoint со списком терминалов Omniva (для живого поиска на сайте) ====
    CROW_ROUTE(app, "/omniva-locations")([](const crow::request& req) {
        crow::response res;
        res.set_header("Content-Type", "application/json; charset=utf-8");

        std::string countryFilter = req.url_params.get("country") ? req.url_params.get("country") : "LT";

        json arr = json::array();
        {
            std::lock_guard<std::mutex> lock(omnivaCacheMutex);
            for (const auto& loc : omnivaCache) {
                if (loc.country != countryFilter) continue;
                arr.push_back({
                    {"id", loc.id},
                    {"name", loc.name},
                    {"address", loc.address}
                });
            }
        }

        res.write(arr.dump());
        return res;
    });

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
    select, input, button { padding: 8px; margin-top: 8px; width: 100%; border-radius: 6px; border: 1px solid #ccc; box-sizing: border-box; font-family: inherit; }
    button { background: #2563eb; color: white; border: none; cursor: pointer; font-weight: bold; }
    button:hover { background: #1d4ed8; }
    #cart-box { max-width: 420px; margin: 30px auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 5px rgba(0,0,0,0.1); position: relative; }
    .cart-item { display: flex; justify-content: space-between; padding: 8px 0; border-bottom: 1px solid #eee; }
    .remove-btn { color: red; cursor: pointer; background: none; border: none; width: auto; padding: 0 8px; }
    #checkout-btn { background: #16a34a; margin-top: 15px; }
    #checkout-btn:hover { background: #15803d; }
    label { font-size: 13px; color: #555; margin-top: 10px; display: block; }
    #dpd-fields, #omniva-fields { display: none; }
    .search-results { border: 1px solid #ddd; border-radius: 6px; max-height: 220px; overflow-y: auto; margin-top: 4px; background: white; position: relative; z-index: 10; }
    .search-result-item { padding: 8px 10px; cursor: pointer; font-size: 14px; border-bottom: 1px solid #f0f0f0; }
    .search-result-item:hover { background: #f0f7ff; }
    .selected-point { background: #eefdf3; border: 1px solid #16a34a; border-radius: 6px; padding: 8px 10px; margin-top: 8px; font-size: 14px; }
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
    <select id="delivery-method" onchange="onDeliveryChange()">
        <option value="Самовывоз">Самовывоз — бесплатно</option>
        <option value="DPD">DPD-средняя 15 если будет больше надо будет доплатить — $12.00-30.00</option>
        <option value="Omniva">Omniva — $8.00</option>
    </select>

    <!-- DPD: простая форма адреса (пока без карты пунктов) -->
    <div id="dpd-fields">
        <label>Страна:</label>
        <input type="text" id="addr-country" placeholder="Например: Литва">
        <label>Город:</label>
        <input type="text" id="addr-city" placeholder="Например: Клайпеда">
        <label>Улица, дом, квартира (или адрес пункта DPD):</label>
        <input type="text" id="addr-street" placeholder="Например: ул. Тайкос, д. 10">
        <label>Почтовый индекс:</label>
        <input type="text" id="addr-zip" placeholder="Например: 92100">
    </div>

    <!-- Omniva: живой поиск реальных терминалов -->
    <div id="omniva-fields">
        <label>Найди ближайший терминал Omniva:</label>
        <input type="text" id="omniva-search" placeholder="Введи город или улицу..." oninput="searchOmniva()" autocomplete="off">
        <div id="omniva-results" class="search-results" style="display:none;"></div>
        <div id="omniva-selected" style="display:none;"></div>
    </div>

    <h3 style="font-size: 15px; margin: 20px 0 5px; color: #333; border-top: 1px solid #eee; padding-top: 15px;">Контакты для связи</h3>
    <label>Электронная почта:</label>
    <input type="email" id="contact-email" placeholder="example@mail.com">
    <label>Telegram (юзернейм):</label>
    <input type="text" id="contact-telegram" placeholder="@username">

    <p><b>Итого: $<span id="cart-total">0.00</span></b></p>
    <button id="checkout-btn" onclick="checkout()">Оплатить картой</button>
</div>

<script>
    const products = [
        { id: "tshirt",  name: "Футболка Palm Angels graffiti", price: 30.00, sizes: ["S","M","L","XL"],
          colors: [ {name:"Бело-серый", key:"white-gray"}, {name:"Чёрно-синий", key:"black-blue"}, {name:"Бело-красный", key:"white-red"} ] },
        { id: "tshirt3", name: "Футболка Lanvin",  price: 30.00, sizes: ["S","M","L","XL"],
          colors: [ {name:"Белый", key:"white"}, {name:"Чёрный", key:"black"} ] },
        { id: "tshirt1", name: "Футболка Lanvin&Gallery Dept", price: 30.00, sizes: ["S","M","L","XL"],
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
        "DPD": 15.00,
        "Omniva": 8.00
    };

    let cart = JSON.parse(localStorage.getItem('cart') || '[]');
    let omnivaData = null;
    let selectedOmnivaPoint = null;

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

    async function onDeliveryChange() {
        const method = document.getElementById('delivery-method').value;
        document.getElementById('dpd-fields').style.display = (method === 'DPD') ? 'block' : 'none';
        document.getElementById('omniva-fields').style.display = (method === 'Omniva') ? 'block' : 'none';

        if (method === 'Omniva' && omnivaData === null) {
            try {
                const response = await fetch('/omniva-locations?country=LT');
                omnivaData = await response.json();
            } catch (e) {
                omnivaData = [];
            }
        }

        renderCart();
    }

    function searchOmniva() {
        const query = document.getElementById('omniva-search').value.trim().toLowerCase();
        const resultsBox = document.getElementById('omniva-results');

        if (!omnivaData || query.length < 2) {
            resultsBox.style.display = 'none';
            return;
        }

        const matches = omnivaData.filter(loc =>
            loc.name.toLowerCase().includes(query) || loc.address.toLowerCase().includes(query)
        ).slice(0, 15);

        if (matches.length === 0) {
            resultsBox.innerHTML = '<div class="search-result-item">Ничего не найдено</div>';
            resultsBox.style.display = 'block';
            return;
        }

        resultsBox.innerHTML = matches.map(loc =>
            `<div class="search-result-item" onclick='selectOmnivaPoint(${JSON.stringify(loc).replace(/'/g, "&apos;")})'>
                <b>${loc.name}</b><br><span style="color:#777">${loc.address}</span>
            </div>`
        ).join('');
        resultsBox.style.display = 'block';
    }

    function selectOmnivaPoint(loc) {
        selectedOmnivaPoint = loc;
        document.getElementById('omniva-search').value = '';
        document.getElementById('omniva-results').style.display = 'none';
        const selectedBox = document.getElementById('omniva-selected');
        selectedBox.innerHTML = `<div class="selected-point">📦 Выбран пункт: <b>${loc.name}</b><br>${loc.address} <button style="width:auto;padding:2px 8px;margin-left:8px;background:#eee;color:#333;" onclick="clearOmnivaPoint()">Изменить</button></div>`;
        selectedBox.style.display = 'block';
    }

    function clearOmnivaPoint() {
        selectedOmnivaPoint = null;
        document.getElementById('omniva-selected').style.display = 'none';
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

    function isValidEmail(email) {
        return /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email);
    }

    async function checkout() {
        if (cart.length === 0) {
            alert('Корзина пуста!');
            return;
        }

        const deliveryMethod = document.getElementById('delivery-method').value;
        let address = '';

        if (deliveryMethod === 'DPD') {
            const country = document.getElementById('addr-country').value.trim();
            const city = document.getElementById('addr-city').value.trim();
            const street = document.getElementById('addr-street').value.trim();
            const zip = document.getElementById('addr-zip').value.trim();

            if (!country || !city || !street || !zip) {
                alert('Пожалуйста, заполни все поля адреса доставки DPD.');
                return;
            }
            address = `${country}, ${city}, ${street}, индекс ${zip} (DPD)`;
        } else if (deliveryMethod === 'Omniva') {
            if (!selectedOmnivaPoint) {
                alert('Пожалуйста, выбери терминал Omniva из списка.');
                return;
            }
            address = `Omniva терминал: ${selectedOmnivaPoint.name}, ${selectedOmnivaPoint.address}`;
        }

        const email = document.getElementById('contact-email').value.trim();
        const telegram = document.getElementById('contact-telegram').value.trim();

        if (!email || !isValidEmail(email)) {
            alert('Пожалуйста, укажи корректный email для связи.');
            return;
        }
        if (!telegram) {
            alert('Пожалуйста, укажи свой Telegram для связи.');
            return;
        }

        const items = cart.map(item => ({ id: item.id, color: item.color, size: item.size }));

        const response = await fetch('/create-checkout-session', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ items, delivery: deliveryMethod, address, email, telegram })
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
            std::string address = body.contains("address") ? body["address"].get<std::string>() : "";
            std::string email = body.contains("email") ? body["email"].get<std::string>() : "";
            std::string telegram = body.contains("telegram") ? body["telegram"].get<std::string>() : "";

            std::string checkoutUrl = createCheckoutSession(cartItems, deliveryMethod, address, email, telegram);

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

    CROW_ROUTE(app, "/static/<string>")([](std::string filename) {
        crow::response res;
        res.set_static_file_info("static/" + filename);
        return res;
    });

    const char* port_env = std::getenv("PORT");
    int port = port_env ? std::stoi(port_env) : 18080;

    app.port(port).multithreaded().run();
}

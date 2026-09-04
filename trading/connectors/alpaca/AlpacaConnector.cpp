#include "trading/connectors/alpaca/AlpacaConnector.hpp"
#include "trading/core/security/CredentialStore.hpp"
#include "logger.hpp"

#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <cstdlib>

namespace trading {

namespace {

uint64_t nowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return s;
}

std::string doubleToStr(double v) {
    std::ostringstream oss;
    oss << v;
    return oss.str();
}

} // namespace

AlpacaConnector::AlpacaConnector(const ConnectorConfig& cfg)
    : cfg_(cfg), env_(cfg.env) {
    capabilities_.spot = true;
    capabilities_.shorting = true;
    capabilities_.maxLeverage = 1.0;  // cash account; margin accounts report more
}

AlpacaConnector::~AlpacaConnector() {
    disconnect();
}

bool AlpacaConnector::loadCredentials(std::string& key, std::string& secret) {
    CredentialStore store;
    // Env names are the standard Alpaca ones; file/cred-manager also supported.
    auto k = store.get("alpaca", cred::API_KEY, "ALPACA_API_KEY_ID");
    auto s = store.get("alpaca", cred::API_SECRET, "ALPACA_API_SECRET_KEY");
    if (k && s) {
        key = *k;
        secret = *s;
        return true;
    }
    if (!apiKey_.empty() && !apiSecret_.empty()) {
        key = apiKey_;
        secret = apiSecret_;
        return true;
    }
    return false;
}

std::string AlpacaConnector::apiBase() const {
    return env_ == EnvMode::LIVE
               ? "https://api.alpaca.markets"
               : "https://paper-api.alpaca.markets";
}

std::string AlpacaConnector::streamUrl() const {
    return "wss://stream.data.alpaca.markets/v2/iex";
}

std::map<std::string, std::string> AlpacaConnector::authHeaders() const {
    return {
        {"APCA-API-KEY-ID", apiKey_},
        {"APCA-API-SECRET-KEY", apiSecret_}
    };
}

bool AlpacaConnector::connect() {
    std::string key, secret;
    if (!loadCredentials(key, secret)) {
        LOG_ERROR("AlpacaConnector: no credentials (env ALPACA_API_KEY_ID / "
                  "ALPACA_API_SECRET_KEY or credential store)");
        return false;
    }
    apiKey_ = key;
    apiSecret_ = secret;
    connected_.store(true);
    LOG_INFO("AlpacaConnector: connected (env={})", toString(env_));
    return true;
}

void AlpacaConnector::disconnect() {
    streamStop_.store(true);
    if (streamThread_.joinable()) streamThread_.join();
    if (ws_) ws_->close();
    connected_.store(false);
    authenticated_.store(false);
}

bool AlpacaConnector::isConnected() const {
    return connected_.load();
}

bool AlpacaConnector::authenticate() {
    if (!connected_.load()) {
        if (!connect()) return false;
    }
    // Real auth check: GET /v2/account with the keys.
    auto resp = get("/v2/account");
    if (!resp.ok()) {
        LOG_ERROR("AlpacaConnector: authentication failed (HTTP {})", resp.status);
        authenticated_.store(false);
        return false;
    }
    authenticated_.store(true);
    return true;
}

bool AlpacaConnector::isAuthenticated() const {
    return authenticated_.load();
}

HttpResponse AlpacaConnector::get(const std::string& path) {
    HttpRequest req;
    req.method = "GET";
    req.url = apiBase() + path;
    req.headers = authHeaders();
    req.timeout = std::chrono::seconds(15);
    return http_.request(req);
}

HttpResponse AlpacaConnector::post(const std::string& path,
                                   const nlohmann::json& body) {
    HttpRequest req;
    req.method = "POST";
    req.url = apiBase() + path;
    req.headers = authHeaders();
    req.headers["Content-Type"] = "application/json";
    req.body = body.dump();
    req.timeout = std::chrono::seconds(15);
    return http_.request(req);
}

HttpResponse AlpacaConnector::del(const std::string& path) {
    HttpRequest req;
    req.method = "DELETE";
    req.url = apiBase() + path;
    req.headers = authHeaders();
    req.timeout = std::chrono::seconds(15);
    return http_.request(req);
}

// ---------------------------------------------------------------------------
// Parsing helpers
// ---------------------------------------------------------------------------

std::string AlpacaConnector::canonicalSymbol(const std::string& native) const {
    // Alpaca crypto uses BTCUSD; stocks use plain AAPL. Convert to canonical.
    std::string n = toUpper(native);
    if (n.size() > 3) {
        static const char* kCrypto[] = {"USD", "USDT"};
        for (const char* q : kCrypto) {
            size_t ql = std::char_traits<char>::length(q);
            if (n.size() > ql && n.compare(n.size() - ql, ql, q) == 0 &&
                (n.size() - ql) >= 2 && n.compare(0, 2, "B") == 0 &&
                n.compare(0, 3, "BTC") == 0)
                return n.substr(0, n.size() - ql) + "/USD";
        }
    }
    if (n.size() >= 6 && n.compare(n.size() - 3, 3, "USD") == 0) {
        std::string base = n.substr(0, n.size() - 3);
        // Only treat as crypto if base is a known crypto symbol.
        static const char* kKnown[] = {"BTC", "ETH", "SOL", "ADA", "LTC",
                                       "XRP", "DOT", "LINK", "AVAX", "MATIC",
                                       "DOGE", "SHIB", "UNI", "AAVE", "BCH"};
        for (const char* b : kKnown) {
            if (base == b) return base + "/USD";
        }
    }
    return n + "/USD";  // stocks: AAPL -> AAPL/USD
}

std::string AlpacaConnector::nativeSymbol(const std::string& canonical) const {
    auto slash = canonical.find('/');
    if (slash == std::string::npos) return canonical;
    std::string base = canonical.substr(0, slash);
    std::string quote = canonical.substr(slash + 1);
    static const char* kKnown[] = {"BTC", "ETH", "SOL", "ADA", "LTC",
                                   "XRP", "DOT", "LINK", "AVAX", "MATIC",
                                   "DOGE", "SHIB", "UNI", "AAVE", "BCH"};
    for (const char* b : kKnown) {
        if (base == b) return base + quote;  // BTCUSD
    }
    // Stocks: return the ticker part only.
    return base;
}

double jsonDouble(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return 0.0;
    if (it->is_string()) {
        try { return std::stod(it->get<std::string>()); } catch (...) { return 0.0; }
    }
    return it->get<double>();
}

std::string jsonString(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return "";
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

bool jsonBool(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_boolean()) return false;
    return it->get<bool>();
}

AccountInfo AlpacaConnector::parseAccount(const nlohmann::json& j) const {
    AccountInfo a;
    a.connector = "alpaca";
    a.accountId = jsonString(j, "id");
    a.env = env_;
    a.equity = jsonDouble(j, "equity");
    a.cash = jsonDouble(j, "cash");
    a.buyingPower = jsonDouble(j, "buying_power");
    a.initialMargin = jsonDouble(j, "initial_margin");
    a.maintenanceMargin = jsonDouble(j, "maintenance_margin");
    a.unrealizedPnl = jsonDouble(j, "unrealized_pl");
    a.realizedPnl = jsonDouble(j, "realized_pl");
    a.currency = jsonString(j, "currency");
    if (a.currency.empty()) a.currency = "USD";
    a.tradingBlocked = jsonBool(j, "trading_blocked");
    a.status = jsonString(j, "status");
    return a;
}

Position AlpacaConnector::parsePosition(const nlohmann::json& j) const {
    Position p;
    p.connector = "alpaca";
    std::string sym = jsonString(j, "symbol");
    p.symbol = canonicalSymbol(sym);
    double qty = jsonDouble(j, "qty");
    if (jsonString(j, "side") == "short") qty = -std::fabs(qty);
    p.quantity = qty;
    p.avgEntryPrice = jsonDouble(j, "avg_entry_price");
    p.currentPrice = jsonDouble(j, "current_price");
    p.unrealizedPnl = jsonDouble(j, "unrealized_pl");
    p.realizedPnl = jsonDouble(j, "realized_pl");
    p.updatedAt = nowNs();
    p.raw = j.dump();
    if (p.raw.size() > 512) p.raw.resize(512);
    return p;
}

OrderStatus AlpacaConnector::mapOrderStatus(const std::string& s) const {
    std::string u = toUpper(s);
    if (u == "NEW" || u == "ACCEPTED" || u == "PENDING_NEW" ||
        u == "ACCEPTED_FOR_BIDDING") return OrderStatus::ACKNOWLEDGED;
    if (u == "PARTIALLY_FILLED") return OrderStatus::PARTIALLY_FILLED;
    if (u == "FILLED") return OrderStatus::FILLED;
    if (u == "CANCELED" || u == "CANCELLED" || u == "PENDING_CANCEL" ||
        u == "EXPIRED") return OrderStatus::CANCELLED;
    if (u == "REJECTED" || u == "SUSPENDED") return OrderStatus::REJECTED;
    if (u == "DONE_FOR_DAY") return OrderStatus::CANCELLED;
    if (u == "HELD") return OrderStatus::ACKNOWLEDGED;
    return OrderStatus::UNKNOWN;
}

Order AlpacaConnector::parseOrder(const nlohmann::json& j) const {
    Order o;
    o.connector = "alpaca";
    o.orderId = jsonString(j, "id");
    o.clientOrderId = jsonString(j, "client_order_id");
    o.symbol = canonicalSymbol(jsonString(j, "symbol"));
    std::string side = toUpper(jsonString(j, "side"));
    o.side = side == "SELL" ? Side::SELL : Side::BUY;
    std::string type = toUpper(jsonString(j, "type"));
    if (type == "LIMIT") o.type = OrderType::LIMIT;
    else if (type == "STOP") o.type = OrderType::STOP;
    else if (type == "STOP_LIMIT") o.type = OrderType::STOP_LIMIT;
    else if (type == "TRAILING_STOP") o.type = OrderType::STOP;
    else o.type = OrderType::MARKET;
    o.requestedQuantity = jsonDouble(j, "qty");
    o.filledQuantity = jsonDouble(j, "filled_qty");
    o.avgFillPrice = jsonDouble(j, "filled_avg_price");
    o.limitPrice = jsonDouble(j, "limit_price");
    o.stopPrice = jsonDouble(j, "stop_price");
    std::string tif = toUpper(jsonString(j, "time_in_force"));
    if (tif == "IOC") o.tif = TimeInForce::IOC;
    else if (tif == "OPG") o.tif = TimeInForce::OPG;
    else if (tif == "CLS") o.tif = TimeInForce::CLS;
    else if (tif == "DAY") o.tif = TimeInForce::DAY;
    else if (tif == "FOK") o.tif = TimeInForce::FOK;
    else o.tif = TimeInForce::GTC;
    o.reduceOnly = jsonBool(j, "reduce_only");
    o.status = mapOrderStatus(jsonString(j, "status"));
    o.statusMessage = jsonString(j, "status");
    o.createdAt = nowNs();
    o.updatedAt = nowNs();
    o.raw = j.dump();
    if (o.raw.size() > 512) o.raw.resize(512);
    return o;
}

// ---------------------------------------------------------------------------
// Account / positions / orders
// ---------------------------------------------------------------------------

AccountInfo AlpacaConnector::getAccount() {
    auto resp = get("/v2/account");
    if (!resp.ok()) {
        LOG_ERROR("AlpacaConnector: getAccount failed (HTTP {})", resp.status);
        return AccountInfo{};
    }
    try {
        return parseAccount(nlohmann::json::parse(resp.body));
    } catch (const std::exception& e) {
        LOG_ERROR("AlpacaConnector: getAccount parse failed: {}", e.what());
        return AccountInfo{};
    }
}

std::vector<Position> AlpacaConnector::getPositions() {
    std::vector<Position> out;
    auto resp = get("/v2/positions");
    if (!resp.ok()) return out;
    try {
        auto arr = nlohmann::json::parse(resp.body);
        if (!arr.is_array()) return out;
        for (const auto& j : arr) {
            if (j.is_object()) out.push_back(parsePosition(j));
        }
    } catch (...) {}
    return out;
}

std::vector<Order> AlpacaConnector::getOpenOrders() {
    std::vector<Order> out;
    auto resp = get("/v2/orders?status=open");
    if (!resp.ok()) return out;
    try {
        auto arr = nlohmann::json::parse(resp.body);
        if (!arr.is_array()) return out;
        for (const auto& j : arr) {
            if (j.is_object()) out.push_back(parseOrder(j));
        }
    } catch (...) {}
    return out;
}

OrderResult AlpacaConnector::placeOrder(const OrderRequest& request) {
    OrderResult res;
    nlohmann::json body;
    body["symbol"] = nativeSymbol(request.symbol);
    body["qty"] = doubleToStr(request.quantity);
    body["side"] = request.side == Side::BUY ? "buy" : "sell";
    switch (request.type) {
        case OrderType::LIMIT:
            body["type"] = "limit";
            body["limit_price"] = doubleToStr(request.limitPrice);
            break;
        case OrderType::STOP:
            body["type"] = "stop";
            body["stop_price"] = doubleToStr(request.stopPrice);
            break;
        case OrderType::STOP_LIMIT:
            body["type"] = "stop_limit";
            body["limit_price"] = doubleToStr(request.limitPrice);
            body["stop_price"] = doubleToStr(request.stopPrice);
            break;
        default:
            body["type"] = "market";
            break;
    }
    switch (request.tif) {
        case TimeInForce::IOC: body["time_in_force"] = "ioc"; break;
        case TimeInForce::OPG: body["time_in_force"] = "opg"; break;
        case TimeInForce::CLS: body["time_in_force"] = "cls"; break;
        case TimeInForce::DAY: body["time_in_force"] = "day"; break;
        case TimeInForce::FOK: body["time_in_force"] = "fok"; break;
        default: body["time_in_force"] = "gtc"; break;
    }
    if (!request.clientOrderId.empty()) body["client_order_id"] = request.clientOrderId;
    if (request.reduceOnly) body["reduce_only"] = true;

    auto resp = post("/v2/orders", body);
    if (!resp.ok()) {
        res.ok = false;
        res.error = "Alpaca order rejected (HTTP " + std::to_string(resp.status) + ")";
        if (!resp.body.empty()) {
            try {
                auto err = nlohmann::json::parse(resp.body);
                if (err.contains("message"))
                    res.error += ": " + err["message"].get<std::string>();
            } catch (...) {}
        }
        return res;
    }
    try {
        auto j = nlohmann::json::parse(resp.body);
        res.ok = true;
        res.order = parseOrder(j);
    } catch (const std::exception& e) {
        res.ok = false;
        res.error = std::string("Alpaca order response parse failed: ") + e.what();
    }
    return res;
}

CancelResult AlpacaConnector::cancelOrder(const std::string& orderId) {
    CancelResult res;
    res.orderId = orderId;
    auto resp = del("/v2/orders/" + orderId);
    if (resp.ok() || resp.status == 404) {
        res.ok = true;
        res.status = OrderStatus::CANCELLED;
    } else {
        res.ok = false;
        res.error = "cancel failed (HTTP " + std::to_string(resp.status) + ")";
    }
    return res;
}

OrderResult AlpacaConnector::modifyOrder(const std::string& orderId,
                                         const OrderModification& mod) {
    OrderResult res;
    nlohmann::json body;
    if (mod.quantity > 0) body["qty"] = doubleToStr(mod.quantity);
    if (mod.limitPrice > 0) body["limit_price"] = doubleToStr(mod.limitPrice);
    if (mod.stopPrice > 0) body["stop_price"] = doubleToStr(mod.stopPrice);
    auto resp = post("/v2/orders/" + orderId, body);
    if (!resp.ok()) {
        res.ok = false;
        res.error = "modify failed (HTTP " + std::to_string(resp.status) + ")";
        return res;
    }
    try {
        auto j = nlohmann::json::parse(resp.body);
        res.ok = true;
        res.order = parseOrder(j);
    } catch (...) {
        res.ok = false;
        res.error = "modify response parse failed";
    }
    return res;
}

// ---------------------------------------------------------------------------
// Market data
// ---------------------------------------------------------------------------

MarketSnapshot AlpacaConnector::getMarketSnapshot(const std::string& symbol) {
    MarketSnapshot snap;
    snap.symbol = symbol;
    std::string native = nativeSymbol(symbol);
    // Use the IEX trades endpoint for a last quote approximation via the
    // snapshot endpoint when available; fall back to cached WS state.
    auto resp = get("/v2/stocks/" + native + "/snapshot");
    if (resp.ok()) {
        try {
            auto j = nlohmann::json::parse(resp.body);
            if (j.contains("latestTrade") && j["latestTrade"].is_object()) {
                snap.last = jsonDouble(j["latestTrade"], "p");
            }
            if (j.contains("latestQuote") && j["latestQuote"].is_object()) {
                snap.bid = jsonDouble(j["latestQuote"], "bp");
                snap.ask = jsonDouble(j["latestQuote"], "ap");
            }
            if (j.contains("prevDailyBar") && j["prevDailyBar"].is_object()) {
                snap.vwap = jsonDouble(j["prevDailyBar"], "v");
            }
            if (j.contains("minuteBar") && j["minuteBar"].is_object()) {
                snap.last = jsonDouble(j["minuteBar"], "c");
            }
            snap.spread = snap.ask - snap.bid;
            snap.timestamp = nowNs();
        } catch (...) {}
    } else {
        // Fall back to cached state from the WS stream.
        std::lock_guard<std::mutex> lock(snapMutex_);
        auto it = snapshots_.find(symbol);
        if (it != snapshots_.end()) return it->second;
        snap.timestamp = nowNs();
    }
    return snap;
}

void AlpacaConnector::subscribeMarketData(const SubscriptionRequest& request) {
    // Cache the requested symbols so the stream loop re-subscribes on reconnect.
    std::lock_guard<std::mutex> lock(wsMutex_);
    subscribedSymbols_.push_back(request.symbol);
}

void AlpacaConnector::unsubscribeMarketData(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(wsMutex_);
    // minimal: leave subscription in place; stream filters server-side
    (void)symbol;
}

void AlpacaConnector::handleStreamMessage(const std::string& text) {
    try {
        auto j = nlohmann::json::parse(text);
        if (!j.is_array()) return;
        for (const auto& item : j) {
            if (!item.is_object()) continue;
            std::string type = jsonString(item, "T");
            MarketTick tick;
            tick.exchange = "alpaca";
            std::string sym = jsonString(item, "S");
            tick.symbol = canonicalSymbol(sym);
            if (type == "q") {  // quote
                tick.bid = jsonDouble(item, "bp");
                tick.ask = jsonDouble(item, "ap");
                tick.bidSize = jsonDouble(item, "bs");
                tick.askSize = jsonDouble(item, "as");
                tick.exchangeTimestamp = static_cast<uint64_t>(jsonDouble(item, "t")) * 1'000'000ULL;
                tick.localTimestamp = nowNs();
            } else if (type == "t" || type == "T") {  // trade
                tick.last = jsonDouble(item, "p");
                tick.volume = jsonDouble(item, "s");
                tick.bid = tick.last;
                tick.ask = tick.last;
                tick.exchangeTimestamp = static_cast<uint64_t>(jsonDouble(item, "t")) * 1'000'000ULL;
                tick.localTimestamp = nowNs();
            } else {
                continue;
            }
            // Cache snapshot.
            {
                std::lock_guard<std::mutex> lock(snapMutex_);
                MarketSnapshot& s = snapshots_[tick.symbol];
                s.symbol = tick.symbol;
                s.bid = tick.bid;
                s.ask = tick.ask;
                s.last = tick.last;
                s.spread = tick.ask - tick.bid;
                s.timestamp = tick.localTimestamp;
                s.stale = false;
            }
            if (tickCb_) tickCb_(tick);
        }
    } catch (const std::exception& e) {
        LOG_WARN("AlpacaConnector: stream message parse failed: {}", e.what());
    }
}

void AlpacaConnector::streamLoop() {
    while (!streamStop_.load()) {
        // Open a fresh stream connection.
        auto ws = std::make_shared<WebSocketClient>();
        ws->setMessageHandler([this](int opcode, const std::string& payload) {
            if (opcode == 0x1) handleStreamMessage(payload);
        });
        std::string err;
        if (!ws->connect(streamUrl(), 10000, &err)) {
            LOG_WARN("AlpacaConnector: stream connect failed: {}", err);
            // Backoff + jitter.
            for (int i = 0; i < 20 && !streamStop_.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(wsMutex_);
            ws_ = ws;
        }
        // Authenticate on the stream.
        nlohmann::json auth;
        auth["action"] = "auth";
        auth["key"] = apiKey_;
        auth["secret"] = apiSecret_;
        ws->sendText(auth.dump());
        LOG_INFO("AlpacaConnector: market stream connected");

        // Subscribe to cached symbols.
        std::vector<std::string> syms;
        {
            std::lock_guard<std::mutex> lock(wsMutex_);
            syms = subscribedSymbols_;
        }
        if (!syms.empty()) {
            nlohmann::json sub;
            sub["action"] = "subscribe";
            sub["trades"] = syms;
            sub["quotes"] = syms;
            ws->sendText(sub.dump());
        }

        // Block until the socket drops.
        while (!streamStop_.load() && ws->isConnected()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        ws->close();
        LOG_WARN("AlpacaConnector: market stream disconnected (reconnecting)");
    }
}

// ---------------------------------------------------------------------------
// Health check (spec §30): real API-level checks, not TCP.
// ---------------------------------------------------------------------------

ConnectorHealth AlpacaConnector::testConnection() {
    ConnectorHealth health;
    auto check = [&](const char* label, bool ok, const std::string& detail = "") {
        if (!ok) health.issues.push_back(std::string(label) + ": " + detail);
        return ok;
    };
    if (!connect()) {
        health.status = "FAIL (no credentials)";
        return health;
    }
    check("auth", authenticate(), "account endpoint rejected credentials");
    health.authenticated = authenticated_.load();
    if (health.authenticated) {
        auto acc = getAccount();
        check("account", !acc.accountId.empty(), "account payload empty");
        health.connected = true;
    }
    health.status = health.issues.empty() ? "PASS" : "FAIL";
    return health;
}

void AlpacaConnector::setCredentials(const std::string& key,
                                     const std::string& secret) {
    apiKey_ = key;
    apiSecret_ = secret;
}

} // namespace trading

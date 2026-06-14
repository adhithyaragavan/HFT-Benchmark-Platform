#include <httplib.h>
#include <nlohmann/json.hpp>
#include "orderbook/orderbook.hpp"
#include <iostream>
#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <atomic>
#include <chrono>
#include <mutex>
#include <memory>
#include <algorithm>

using json = nlohmann::json;
using namespace orderbook;

// Global orderbooks map and shared mutex
std::unordered_map<std::string, std::shared_ptr<OrderBook>> orderbooks;
std::shared_mutex books_mu;

// Struct to track resting orders in memory for execution report leaves/cum qty mapping
struct OrderTracker {
    std::string client_order_id;
    Side side;
    std::string symbol;
    int64_t orig_qty;
    int64_t remaining_qty;
};
std::unordered_map<std::string, OrderTracker> order_index;
std::shared_mutex index_mu;

std::atomic<int64_t> order_counter{0};

std::string next_order_id(const std::string& symbol) {
    return "ORD-" + symbol + "-" + std::to_string(++order_counter);
}

std::shared_ptr<OrderBook> get_or_create_book(const std::string& symbol) {
    {
        std::shared_lock<std::shared_mutex> lock(books_mu);
        auto it = orderbooks.find(symbol);
        if (it != orderbooks.end()) {
            return it->second;
        }
    }
    {
        std::unique_lock<std::shared_mutex> lock(books_mu);
        auto it = orderbooks.find(symbol);
        if (it != orderbooks.end()) {
            return it->second;
        }
        auto book = std::make_shared<OrderBook>(symbol);
        orderbooks[symbol] = book;
        return book;
    }
}

int64_t current_time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

int main() {
    // Port configurations
    const char* port_env = std::getenv("PORT");
    int port = port_env ? std::stoi(port_env) : 8080;

    std::vector<std::string> initial_symbols = {"AAPL", "GOOG", "MSFT", "AMZN", "TSLA"};
    for (const auto& sym : initial_symbols) {
        get_or_create_book(sym);
    }

    httplib::Server svr;

    // --- Health Check ---
    svr.Get("/api/v1/health", [](const httplib::Request&, httplib::Response& res) {
        json j = {
            {"status", "healthy"},
            {"service", "cpp-exchange"},
            {"version", "1.0.0"}
        };
        res.set_content(j.dump(), "application/json");
    });

    // --- Order Book Snapshot ---
    svr.Get("/api/v1/orderbook/:symbol", [](const httplib::Request& req, httplib::Response& res) {
        std::string symbol = req.path_params.at("symbol");
        std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::toupper);

        auto book = get_or_create_book(symbol);
        auto snap = book->get_snapshot();

        json j;
        to_json(j, snap);
        res.set_content(j.dump(), "application/json");
    });

    // --- Submit Order ---
    svr.Post("/api/v1/orders", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            std::string symbol = body.value("symbol", "AAPL");
            std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::toupper);

            std::string side_str = body.value("side", "");
            Side side = (side_str == "buy" || side_str == "Buy") ? Side::Buy : Side::Sell;

            std::string type_str = body.value("type", "");
            OrderType type = (type_str == "market" || type_str == "Market") ? OrderType::Market : OrderType::Limit;

            int64_t qty = body.value("quantity", 0LL);
            int64_t price = body.value("price", 0LL);
            std::string client_order_id = body.value("client_order_id", "");

            if (qty <= 0) {
                res.status = 400;
                res.set_content(R"({"success":false,"error":"quantity must be positive"})", "application/json");
                return;
            }
            if (type == OrderType::Limit && price <= 0) {
                res.status = 400;
                res.set_content(R"({"success":false,"error":"price must be positive for limit orders"})", "application/json");
                return;
            }

            auto book = get_or_create_book(symbol);
            std::string order_id = next_order_id(symbol);

            Order ord;
            ord.id = order_id;
            ord.side = side;
            ord.type = type;
            ord.price = price;
            ord.quantity = qty;
            ord.timestamp = std::chrono::system_clock::now();

            // Match order and get fills
            std::vector<Fill> fills = book->add_order(ord);

            int64_t filled_qty = 0;
            int64_t total_val = 0;
            for (const auto& fill : fills) {
                filled_qty += fill.quantity;
                total_val += fill.quantity * fill.price;
            }

            int64_t filled_price = filled_qty > 0 ? (total_val / filled_qty) : 0;
            int64_t leaves_qty = qty - filled_qty;

            // Update maker orders in index
            for (const auto& fill : fills) {
                std::unique_lock<std::shared_mutex> idx_lock(index_mu);
                auto it = order_index.find(fill.maker_order_id);
                if (it != order_index.end()) {
                    it->second.remaining_qty -= fill.quantity;
                    if (it->second.remaining_qty <= 0) {
                        order_index.erase(it);
                    }
                }
            }

            std::string status = "new";
            if (leaves_qty == 0) {
                status = "filled";
            } else if (filled_qty > 0) {
                status = "partial";
            } else if (type == OrderType::Market) {
                status = "rejected";
            }

            // If it's a limit order and has leaves remaining, track it
            if (type == OrderType::Limit && leaves_qty > 0) {
                std::unique_lock<std::shared_mutex> idx_lock(index_mu);
                order_index[order_id] = OrderTracker{
                    client_order_id,
                    side,
                    symbol,
                    qty,
                    leaves_qty
                };
            }

            json report = {
                {"order_id", order_id},
                {"client_order_id", client_order_id},
                {"symbol", symbol},
                {"status", status},
                {"side", to_string(side)},
                {"filled_qty", filled_qty},
                {"filled_price", filled_price},
                {"leaves_qty", leaves_qty},
                {"cum_qty", filled_qty},
                {"timestamp_ns", current_time_ns()}
            };

            res.status = 200;
            res.set_content(report.dump(), "application/json");

        } catch (const std::exception& e) {
            res.status = 400;
            json err = {{"success", false}, {"error", std::string("bad request: ") + e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // --- Cancel Order ---
    svr.Delete("/api/v1/orders/:order_id", [](const httplib::Request& req, httplib::Response& res) {
        std::string order_id = req.path_params.at("order_id");

        // Lookup symbol and client_order_id first from tracker
        std::string symbol;
        std::string client_order_id;
        Side side = Side::Buy;
        int64_t orig_qty = 0;
        int64_t remaining_qty = 0;
        bool found_tracker = false;

        {
            std::shared_lock<std::shared_mutex> idx_lock(index_mu);
            auto it = order_index.find(order_id);
            if (it != order_index.end()) {
                symbol = it->second.symbol;
                client_order_id = it->second.client_order_id;
                side = it->second.side;
                orig_qty = it->second.orig_qty;
                remaining_qty = it->second.remaining_qty;
                found_tracker = true;
            }
        }

        if (!found_tracker) {
            // If not found in our local tracker, it might have already been filled or doesn't exist
            res.status = 404;
            json err = {{"success", false}, {"error", "order not found: " + order_id}};
            res.set_content(err.dump(), "application/json");
            return;
        }

        auto book = get_or_create_book(symbol);
        bool cancelled = book->cancel_order(order_id);

        if (cancelled) {
            {
                std::unique_lock<std::shared_mutex> idx_lock(index_mu);
                order_index.erase(order_id);
            }

            json report = {
                {"order_id", order_id},
                {"client_order_id", client_order_id},
                {"symbol", symbol},
                {"status", "cancelled"},
                {"side", to_string(side)},
                {"filled_qty", 0},
                {"filled_price", 0},
                {"leaves_qty", 0},
                {"cum_qty", orig_qty - remaining_qty},
                {"timestamp_ns", current_time_ns()}
            };

            res.status = 200;
            res.set_content(report.dump(), "application/json");
        } else {
            res.status = 404;
            json err = {{"success", false}, {"error", "order could not be cancelled: " + order_id}};
            res.set_content(err.dump(), "application/json");
        }
    });

    std::cout << "🏦 C++ Mock Exchange starting on port " << port << " (symbols: AAPL, GOOG, MSFT, AMZN, TSLA)\n";
    svr.listen("0.0.0.0", port);

    return 0;
}

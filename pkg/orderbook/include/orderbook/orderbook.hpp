#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <map>
#include <list>
#include <unordered_map>
#include <shared_mutex>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <optional>

namespace orderbook {

constexpr int64_t PriceMul = 10000;

enum class Side {
    Buy,
    Sell
};

std::string to_string(Side side);

enum class OrderType {
    Limit,
    Market
};

std::string to_string(OrderType type);

using TimePoint = std::chrono::system_clock::time_point;

struct Order {
    std::string id;
    std::string account_id;
    Side side;
    OrderType type;
    int64_t price;
    int64_t quantity;
    TimePoint timestamp;
};

struct Fill {
    std::string maker_order_id;
    std::string taker_order_id;
    int64_t price;
    int64_t quantity;
    TimePoint timestamp;
};

struct PriceLevel {
    int64_t price;
    int64_t quantity;
    int32_t count;
};

void to_json(nlohmann::json& j, const PriceLevel& pl);

struct OrderBookSnapshot {
    std::string symbol;
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
    TimePoint timestamp;
};

void to_json(nlohmann::json& j, const OrderBookSnapshot& obs);

class OrderBook {
public:
    explicit OrderBook(std::string symbol);

    // Returns fills
    std::vector<Fill> add_order(Order ord);
    
    // Returns true if order was found and removed
    bool cancel_order(const std::string& order_id);

    OrderBookSnapshot get_snapshot() const;
    
    // pair<price, bool>
    std::pair<int64_t, bool> best_bid() const;
    std::pair<int64_t, bool> best_ask() const;
    std::pair<int64_t, bool> spread() const;
    
    size_t order_count() const;
    std::string symbol() const;

private:
    std::string symbol_;
    mutable std::shared_mutex mu_;

    struct LevelData {
        std::list<Order> orders;
        int64_t total_quantity = 0;
    };

    using BidsMap = std::map<int64_t, LevelData, std::greater<int64_t>>;
    using AsksMap = std::map<int64_t, LevelData, std::less<int64_t>>;

    BidsMap bids_;
    AsksMap asks_;

    struct OrderLocation {
        Side side;
        int64_t price;
        std::list<Order>::iterator it;
    };

    std::unordered_map<std::string, OrderLocation> orders_;

    std::vector<Fill> match(Order& taker);
};

} // namespace orderbook

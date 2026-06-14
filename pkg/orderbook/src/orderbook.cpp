#include "orderbook/orderbook.hpp"
#include <random>
#include <sstream>
#include <algorithm>

namespace orderbook {

std::string to_string(Side side) {
    switch (side) {
        case Side::Buy: return "buy";
        case Side::Sell: return "sell";
        default: return "unknown";
    }
}

std::string to_string(OrderType type) {
    switch (type) {
        case OrderType::Limit: return "limit";
        case OrderType::Market: return "market";
        default: return "unknown";
    }
}

void to_json(nlohmann::json& j, const PriceLevel& pl) {
    j = nlohmann::json{
        {"price", pl.price},
        {"quantity", pl.quantity},
        {"count", pl.count}
    };
}

void to_json(nlohmann::json& j, const OrderBookSnapshot& obs) {
    j = nlohmann::json{
        {"symbol", obs.symbol},
        {"bids", obs.bids},
        {"asks", obs.asks},
        {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(obs.timestamp.time_since_epoch()).count()}
    };
}

static std::string generate_uuid() {
    static thread_local std::random_device rd;
    static thread_local std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    std::uniform_int_distribution<> dis2(8, 11);

    std::stringstream ss;
    ss << std::hex;
    for (int i = 0; i < 8; i++) ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 4; i++) ss << dis(gen);
    ss << "-4";
    for (int i = 0; i < 3; i++) ss << dis(gen);
    ss << "-";
    ss << dis2(gen);
    for (int i = 0; i < 3; i++) ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 12; i++) ss << dis(gen);
    return ss.str();
}

OrderBook::OrderBook(std::string symbol) : symbol_(std::move(symbol)) {}

std::vector<Fill> OrderBook::add_order(Order ord) {
    std::unique_lock<std::shared_mutex> lock(mu_);

    if (ord.quantity <= 0) {
        throw std::invalid_argument("order quantity must be positive");
    }
    if (ord.type == OrderType::Limit && ord.price <= 0) {
        throw std::invalid_argument("limit order price must be positive");
    }
    if (ord.id.empty()) {
        ord.id = generate_uuid();
    }
    if (ord.timestamp.time_since_epoch().count() == 0) {
        ord.timestamp = std::chrono::system_clock::now();
    }

    if (orders_.find(ord.id) != orders_.end()) {
        throw std::invalid_argument("duplicate order ID: " + ord.id);
    }

    auto fills = match(ord);

    if (ord.quantity > 0 && ord.type == OrderType::Limit) {
        if (ord.side == Side::Buy) {
            auto& level = bids_[ord.price];
            level.orders.push_back(ord);
            level.total_quantity += ord.quantity;
            orders_[ord.id] = {Side::Buy, ord.price, std::prev(level.orders.end())};
        } else {
            auto& level = asks_[ord.price];
            level.orders.push_back(ord);
            level.total_quantity += ord.quantity;
            orders_[ord.id] = {Side::Sell, ord.price, std::prev(level.orders.end())};
        }
    }

    return fills;
}

std::vector<Fill> OrderBook::match(Order& taker) {
    std::vector<Fill> fills;

    if (taker.side == Side::Buy) {
        auto it = asks_.begin();
        while (taker.quantity > 0 && it != asks_.end()) {
            if (taker.type == OrderType::Limit && it->first > taker.price) {
                break; // best ask is too expensive
            }

            auto& level = it->second;
            auto order_it = level.orders.begin();

            while (taker.quantity > 0 && order_it != level.orders.end()) {
                auto& maker = *order_it;
                
                if (!taker.account_id.empty() && taker.account_id == maker.account_id) {
                    ++order_it;
                    continue; // Skip matching same account (STP)
                }

                int64_t fill_qty = std::min(taker.quantity, maker.quantity);
                
                auto fill_time = std::chrono::system_clock::now();

                fills.push_back(Fill{
                    maker.id,
                    taker.id,
                    maker.price,
                    fill_qty,
                    fill_time
                });

                taker.quantity -= fill_qty;
                maker.quantity -= fill_qty;
                level.total_quantity -= fill_qty;

                if (maker.quantity == 0) {
                    orders_.erase(maker.id);
                    order_it = level.orders.erase(order_it);
                } else {
                    ++order_it;
                }
            }

            if (level.orders.empty()) {
                it = asks_.erase(it);
            } else {
                ++it;
            }
        }
    } else {
        auto it = bids_.begin();
        while (taker.quantity > 0 && it != bids_.end()) {
            if (taker.type == OrderType::Limit && it->first < taker.price) {
                break; // best bid is too cheap
            }

            auto& level = it->second;
            auto order_it = level.orders.begin();

            while (taker.quantity > 0 && order_it != level.orders.end()) {
                auto& maker = *order_it;

                if (!taker.account_id.empty() && taker.account_id == maker.account_id) {
                    ++order_it;
                    continue; // Skip matching same account (STP)
                }

                int64_t fill_qty = std::min(taker.quantity, maker.quantity);
                
                auto fill_time = std::chrono::system_clock::now();

                fills.push_back(Fill{
                    maker.id,
                    taker.id,
                    maker.price,
                    fill_qty,
                    fill_time
                });

                taker.quantity -= fill_qty;
                maker.quantity -= fill_qty;
                level.total_quantity -= fill_qty;

                if (maker.quantity == 0) {
                    orders_.erase(maker.id);
                    order_it = level.orders.erase(order_it);
                } else {
                    ++order_it;
                }
            }

            if (level.orders.empty()) {
                it = bids_.erase(it);
            } else {
                ++it;
            }
        }
    }

    return fills;
}

bool OrderBook::cancel_order(const std::string& order_id) {
    std::unique_lock<std::shared_mutex> lock(mu_);

    auto it = orders_.find(order_id);
    if (it == orders_.end()) {
        return false;
    }

    auto location = it->second;
    orders_.erase(it);

    if (location.side == Side::Buy) {
        auto level_it = bids_.find(location.price);
        if (level_it != bids_.end()) {
            level_it->second.total_quantity -= location.it->quantity;
            level_it->second.orders.erase(location.it);
            if (level_it->second.orders.empty()) {
                bids_.erase(level_it);
            }
        }
    } else {
        auto level_it = asks_.find(location.price);
        if (level_it != asks_.end()) {
            level_it->second.total_quantity -= location.it->quantity;
            level_it->second.orders.erase(location.it);
            if (level_it->second.orders.empty()) {
                asks_.erase(level_it);
            }
        }
    }

    return true;
}

OrderBookSnapshot OrderBook::get_snapshot() const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    
    OrderBookSnapshot snapshot;
    snapshot.symbol = symbol_;
    snapshot.timestamp = std::chrono::system_clock::now();

    for (const auto& [price, level] : bids_) {
        if (!level.orders.empty()) {
            snapshot.bids.push_back(PriceLevel{
                price,
                level.total_quantity,
                static_cast<int32_t>(level.orders.size())
            });
        }
    }

    for (const auto& [price, level] : asks_) {
        if (!level.orders.empty()) {
            snapshot.asks.push_back(PriceLevel{
                price,
                level.total_quantity,
                static_cast<int32_t>(level.orders.size())
            });
        }
    }

    return snapshot;
}

std::pair<int64_t, bool> OrderBook::best_bid() const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    if (bids_.empty()) {
        return {0, false};
    }
    return {bids_.begin()->first, true};
}

std::pair<int64_t, bool> OrderBook::best_ask() const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    if (asks_.empty()) {
        return {0, false};
    }
    return {asks_.begin()->first, true};
}

std::pair<int64_t, bool> OrderBook::spread() const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    if (bids_.empty() || asks_.empty()) {
        return {0, false};
    }
    int64_t spr = asks_.begin()->first - bids_.begin()->first;
    if (spr < 0) {
        return {0, true}; // Fix negative spread calculation
    }
    return {spr, true};
}

size_t OrderBook::order_count() const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    return orders_.size();
}

std::string OrderBook::symbol() const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    return symbol_;
}

} // namespace orderbook

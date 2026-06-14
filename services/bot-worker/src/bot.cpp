#include "bot.h"
#include <chrono>
#include <iostream>

Bot::Bot(const BotConfig& cfg, std::shared_ptr<Sender> sender, std::shared_ptr<Reporter> reporter)
    : cfg_(cfg), sender_(sender), reporter_(reporter), base_price_(1500000), running_(false) {
    std::random_device rd;
    rng_.seed(rd() ^ std::chrono::high_resolution_clock::now().time_since_epoch().count());

    // Assign persona based on bot_id hash
    size_t h = std::hash<std::string>{}(cfg_.bot_id);
    int persona = h % 3;
    if (persona == 0) { // Market Maker
        cfg_.limit_ratio = 0.8;
        cfg_.market_ratio = 0.1;
        cfg_.cancel_ratio = 0.1;
    } else if (persona == 1) { // Momentum Trader
        cfg_.limit_ratio = 0.0;
        cfg_.market_ratio = 1.0;
        cfg_.cancel_ratio = 0.0;
    } else { // HFT Arb
        cfg_.limit_ratio = 0.4;
        cfg_.market_ratio = 0.0;
        cfg_.cancel_ratio = 0.6;
    }
}

Bot::~Bot() {
    stop();
}

std::string Bot::generate_uuid() {
    thread_local std::mt19937 gen(std::random_device{}());
    thread_local std::uniform_int_distribution<> dis(0, 15);
    thread_local std::uniform_int_distribution<> dis2(8, 11);

    const char* v = "0123456789abcdef";
    std::string res(36, '-');
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            res[i] = '-';
        } else if (i == 14) {
            res[i] = '4';
        } else if (i == 19) {
            res[i] = v[dis2(gen)];
        } else {
            res[i] = v[dis(gen)];
        }
    }
    return res;
}

void Bot::run() {
    running_ = true;
    thread_ = std::thread([this]() {
        try {
            double ops = cfg_.orders_per_sec;
            if (ops <= 0.0) ops = 10.0;
            double interval_us = 1000000.0 / ops;

            auto start_time = std::chrono::steady_clock::now();
            auto deadline = start_time + std::chrono::milliseconds(cfg_.duration_ms);

            while (running_) {
                auto loop_start = std::chrono::steady_clock::now();

                if (cfg_.duration_ms > 0 && loop_start >= deadline) {
                    break;
                }

                send_order();

                auto loop_end = std::chrono::steady_clock::now();
                auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(loop_end - loop_start).count();
                if (elapsed_us < interval_us) {
                    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(interval_us - elapsed_us)));
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Bot thread terminated with exception: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "Bot thread terminated with unknown exception." << std::endl;
        }
        running_ = false;
    });
}

void Bot::stop() {
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Bot::send_order() {
    std::uniform_real_distribution<double> dist_ratio(0.0, 1.0);
    double r = dist_ratio(rng_);

    std::string symbol = "BTC-USD";
    if (!cfg_.symbols.empty()) {
        std::uniform_int_distribution<size_t> dist_sym(0, cfg_.symbols.size() - 1);
        symbol = cfg_.symbols[dist_sym(rng_)];
    }

    if (r < cfg_.cancel_ratio) {
        std::string order_id;
        {
            std::lock_guard<std::mutex> lock(live_orders_mu_);
            if (!live_orders_.empty()) {
                std::uniform_int_distribution<size_t> dist_live(0, live_orders_.size() - 1);
                size_t idx = dist_live(rng_);
                order_id = live_orders_[idx];
                live_orders_.erase(live_orders_.begin() + idx);
            }
        }

        if (!order_id.empty()) {
            auto sender = sender_;
            auto reporter = reporter_;
            auto cfg = cfg_;
            std::thread([sender, reporter, cfg, order_id, symbol]() {
                auto sent_at = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                std::string err_msg;
                auto resp = sender->cancel_order(order_id, err_msg);
                auto acked_at = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

                TelemetryEvent te;
                te.benchmark_run_id = cfg.run_id;
                te.bot_id = cfg.bot_id;
                te.order_id = order_id;
                te.order_type = "cancel";
                te.symbol = symbol;
                te.side = "none";
                if (resp) {
                    te.filled_qty = resp->filled_qty;
                    te.filled_price = resp->filled_price;
                    te.leaves_qty = resp->leaves_qty;
                }
                te.sent_at_ns = sent_at;
                te.acked_at_ns = acked_at;
                te.latency_ns = acked_at - sent_at;
                te.success = (resp != nullptr);
                te.error_code = err_msg;

                reporter->record(te);
            }).detach();
            return;
        }
    }

    std::string order_type;
    if (r < cfg_.cancel_ratio + cfg_.market_ratio) {
        order_type = "market";
    } else {
        order_type = "limit";
    }

    std::string side = dist_ratio(rng_) < 0.5 ? "buy" : "sell";
    int64_t price = 0;

    if (order_type == "limit") {
        std::uniform_int_distribution<int64_t> dist_drift(-75000, 75000);
        int64_t drift = dist_drift(rng_);
        base_price_ += drift / 100;
        if (base_price_ < 1000000) base_price_ = 1000000;
        if (base_price_ > 2000000) base_price_ = 2000000;

        std::uniform_int_distribution<int64_t> dist_spread(-50000, 50000);
        int64_t spread = dist_spread(rng_);
        price = base_price_ + spread;
        if (price <= 0) price = 10000;
    }

    std::uniform_int_distribution<int64_t> dist_qty(1, 1000);
    int64_t quantity = dist_qty(rng_);

    std::string client_order_id = generate_uuid();

    OrderRequest req;
    req.client_order_id = client_order_id;
    req.symbol = symbol;
    req.side = side;
    req.type = order_type;
    req.price = price;
    req.quantity = quantity;
    req.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    auto sender = sender_;
    auto reporter = reporter_;
    auto cfg = cfg_;
    std::thread([sender, reporter, cfg, req, symbol, order_type, side, price, quantity, client_order_id, this]() {
        auto sent_at = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        std::string err_msg;
        auto resp = sender->send_order(req, err_msg);
        auto acked_at = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

        if (resp && (resp->status == "new" || resp->status == "partial")) {
            std::lock_guard<std::mutex> lock(live_orders_mu_);
            if (live_orders_.size() < 100) {
                live_orders_.push_back(resp->order_id);
            }
        }

        TelemetryEvent te;
        te.benchmark_run_id = cfg.run_id;
        te.bot_id = cfg.bot_id;
        te.order_id = client_order_id;
        te.order_type = order_type + "_" + side;
        te.symbol = symbol;
        te.side = side;
        te.price = price;
        te.quantity = quantity;
        if (resp) {
            te.filled_qty = resp->filled_qty;
            te.filled_price = resp->filled_price;
            te.leaves_qty = resp->leaves_qty;
        }
        te.sent_at_ns = sent_at;
        te.acked_at_ns = acked_at;
        te.latency_ns = acked_at - sent_at;
        te.success = (resp != nullptr);
        te.error_code = err_msg;

        reporter->record(te);
    }).detach();
}

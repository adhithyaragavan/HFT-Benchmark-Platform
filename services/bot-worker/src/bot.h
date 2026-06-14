#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <random>
#include <memory>

#include "sender.h"
#include "reporter.h"

struct BotConfig {
    std::string bot_id;
    std::string run_id;
    std::string endpoint_url;
    int orders_per_sec = 0;
    double limit_ratio = 0.0;
    double market_ratio = 0.0;
    double cancel_ratio = 0.0;
    std::vector<std::string> symbols;
    int duration_ms = 0;
};

class Bot {
public:
    Bot(const BotConfig& cfg, std::shared_ptr<Sender> sender, std::shared_ptr<Reporter> reporter);
    ~Bot();

    void run();
    void stop();

private:
    void send_order();
    std::string generate_uuid();

    BotConfig cfg_;
    std::shared_ptr<Sender> sender_;
    std::shared_ptr<Reporter> reporter_;
    
    std::mt19937_64 rng_;
    std::vector<std::string> live_orders_;
    std::mutex live_orders_mu_;
    int64_t base_price_;

    std::atomic<bool> running_;
    std::thread thread_;
};

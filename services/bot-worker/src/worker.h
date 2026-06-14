#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <nlohmann/json.hpp>
#include <cppkafka/cppkafka.h>

#include "config.h"
#include "reporter.h"
#include "bot.h"

struct BotAssignment {
    std::string run_id;
    std::string endpoint_url;
    int bot_count = 0;
    std::string phase;
    int duration_ms = 0;
    int orders_per_sec = 0;
    double limit_ratio = 0.0;
    double market_ratio = 0.0;
    double cancel_ratio = 0.0;
    std::vector<std::string> symbols;
};

void from_json(const nlohmann::json& j, BotAssignment& a);

class BotFleet {
public:
    BotFleet(const std::string& run_id);
    ~BotFleet();

    void add_bot(std::unique_ptr<Bot> bot);
    void stop();

private:
    std::string run_id_;
    std::vector<std::unique_ptr<Bot>> bots_;
};

class Worker {
public:
    Worker(const Config& cfg, std::shared_ptr<Reporter> reporter);
    ~Worker();

    void run();
    void stop();
    void wait();

private:
    void handle_assignment(const BotAssignment& a);
    void stop_fleet(const std::string& run_id);

    Config cfg_;
    std::shared_ptr<Reporter> reporter_;

    std::unique_ptr<cppkafka::Consumer> consumer_;
    std::atomic<bool> running_;
    std::thread consumer_thread_;

    std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<BotFleet>> fleets_;

    std::mutex timer_mu_;
    std::condition_variable timer_cv_;
    std::vector<std::thread> timer_threads_;
};

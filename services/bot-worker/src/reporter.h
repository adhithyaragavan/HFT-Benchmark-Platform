#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <thread>
#include <atomic>
#include <nlohmann/json.hpp>
#include <cppkafka/cppkafka.h>
#include "config.h"

struct TelemetryEvent {
    std::string benchmark_run_id;
    std::string bot_id;
    std::string order_id;
    std::string order_type;
    std::string symbol;
    std::string side;
    int64_t price = 0;
    int64_t quantity = 0;
    int64_t filled_qty = 0;
    int64_t filled_price = 0;
    int64_t leaves_qty = 0;
    int64_t sent_at_ns = 0;
    int64_t acked_at_ns = 0;
    int64_t latency_ns = 0;
    bool success = false;
    std::string error_code;
};

void to_json(nlohmann::json& j, const TelemetryEvent& e);

class Reporter {
public:
    Reporter(const Config& cfg);
    ~Reporter();

    void record(const TelemetryEvent& event);
    void run();
    void stop();

private:
    void flush();

    std::unique_ptr<cppkafka::Producer> producer_;
    std::string topic_;

    std::mutex mu_;
    std::vector<TelemetryEvent> buffer_;
    size_t max_batch_;
    std::atomic<bool> running_;
    std::thread flusher_thread_;
};

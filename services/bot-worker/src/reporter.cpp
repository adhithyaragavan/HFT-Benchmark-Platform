#include "reporter.h"
#include <iostream>
#include <chrono>

using json = nlohmann::json;

void to_json(json& j, const TelemetryEvent& e) {
    j = json{
        {"benchmark_run_id", e.benchmark_run_id},
        {"bot_id", e.bot_id},
        {"client_order_id", e.order_id},
        {"order_type", e.order_type},
        {"symbol", e.symbol},
        {"sent_at_ns", e.sent_at_ns},
        {"acked_at_ns", e.acked_at_ns},
        {"latency_ns", e.latency_ns},
        {"success", e.success},
        {"side", e.side},
        {"price", e.price},
        {"quantity", e.quantity},
        {"filled_qty", e.filled_qty},
        {"filled_price", e.filled_price},
        {"leaves_qty", e.leaves_qty}
    };
    if (!e.error_code.empty()) {
        j["error_code"] = e.error_code;
    }
}

Reporter::Reporter(const Config& cfg) : topic_(cfg.telemetry_topic), max_batch_(500), running_(false) {
    cppkafka::Configuration config = {
        { "metadata.broker.list", cfg.redpanda_brokers.empty() ? "localhost:19092" : cfg.redpanda_brokers[0] }, // Simplify for comma separated later if needed
        { "batch.size", 1024 * 1024 },
        { "linger.ms", 50 }
    };
    
    // Convert all brokers to comma separated string
    std::string broker_list;
    for (size_t i = 0; i < cfg.redpanda_brokers.size(); ++i) {
        broker_list += cfg.redpanda_brokers[i];
        if (i < cfg.redpanda_brokers.size() - 1) broker_list += ",";
    }
    config.set("metadata.broker.list", broker_list);

    producer_ = std::make_unique<cppkafka::Producer>(config);
    buffer_.reserve(512);
}

Reporter::~Reporter() {
    stop();
}

void Reporter::record(const TelemetryEvent& event) {
    bool should_flush = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        buffer_.push_back(event);
        should_flush = (buffer_.size() >= max_batch_);
    }
    if (should_flush) {
        flush();
    }
}

void Reporter::run() {
    running_ = true;
    flusher_thread_ = std::thread([this]() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            flush();
            try {
                producer_->poll(std::chrono::milliseconds(0));
                producer_->flush();
            } catch (const std::exception& ex) {
                std::cerr << "poll/flush error: " << ex.what() << std::endl;
            }
        }
    });
}

void Reporter::stop() {
    if (running_) {
        running_ = false;
        if (flusher_thread_.joinable()) {
            flusher_thread_.join();
        }
        flush();
        try {
            producer_->flush();
        } catch (const std::exception& ex) {
            std::cerr << "Ignoring flush exception during shutdown: " << ex.what() << std::endl;
        }
    }
}

void Reporter::flush() {
    std::vector<TelemetryEvent> events;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (buffer_.empty()) return;
        events.swap(buffer_);
        buffer_.reserve(512);
    }

    try {
        auto builder = cppkafka::MessageBuilder(topic_);
        for (const auto& e : events) {
            json j = e;
            std::string payload = j.dump();
            builder.key(e.benchmark_run_id).payload(payload);
            producer_->produce(builder);
        }
    } catch (const std::exception& ex) {
        std::cerr << "failed to produce telemetry: " << ex.what() << std::endl;
    }
}

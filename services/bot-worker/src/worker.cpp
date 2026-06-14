#include "worker.h"
#include <iostream>
#include <chrono>

using json = nlohmann::json;

void from_json(const json& j, BotAssignment& a) {
    a.run_id = j.value("run_id", "");
    a.endpoint_url = j.value("endpoint_url", "");
    a.bot_count = j.value("bot_count", 0);
    a.phase = j.value("phase", "");
    a.duration_ms = j.value("duration_ms", 0);
    a.orders_per_sec = j.value("orders_per_sec_per_bot", 0);
    a.limit_ratio = j.value("limit_ratio", 0.0);
    a.market_ratio = j.value("market_ratio", 0.0);
    a.cancel_ratio = j.value("cancel_ratio", 0.0);
    if (j.contains("symbols") && j["symbols"].is_array()) {
        for (const auto& s : j["symbols"]) {
            a.symbols.push_back(s.get<std::string>());
        }
    }
}

BotFleet::BotFleet(const std::string& run_id) : run_id_(run_id) {}

BotFleet::~BotFleet() {
    stop();
}

void BotFleet::add_bot(std::unique_ptr<Bot> bot) {
    bot->run();
    bots_.push_back(std::move(bot));
}

void BotFleet::stop() {
    for (auto& bot : bots_) {
        bot->stop();
    }
}

Worker::Worker(const Config& cfg, std::shared_ptr<Reporter> reporter)
    : cfg_(cfg), reporter_(reporter), running_(false) {

    cppkafka::Configuration config = {
        { "metadata.broker.list", cfg.redpanda_brokers.empty() ? "localhost:19092" : cfg.redpanda_brokers[0] },
        { "group.id", cfg.consumer_group },
        { "auto.offset.reset", "latest" },
        { "enable.auto.commit", true }
    };

    std::string broker_list;
    for (size_t i = 0; i < cfg.redpanda_brokers.size(); ++i) {
        broker_list += cfg.redpanda_brokers[i];
        if (i < cfg.redpanda_brokers.size() - 1) broker_list += ",";
    }
    config.set("metadata.broker.list", broker_list);

    consumer_ = std::make_unique<cppkafka::Consumer>(config);
    consumer_->subscribe({cfg.command_topic});
}

Worker::~Worker() {
    stop();
}

void Worker::run() {
    running_ = true;
    consumer_thread_ = std::thread([this]() {
        std::cout << "worker consuming from topic " << cfg_.command_topic << std::endl;
        while (running_) {
            cppkafka::Message msg = consumer_->poll(std::chrono::milliseconds(100));
            if (!msg) continue;

            if (msg.get_error()) {
                if (!msg.is_eof()) {
                    std::cerr << "fetch error: " << msg.get_error().to_string() << std::endl;
                }
                continue;
            }

            try {
                std::string payload = msg.get_payload();
                json j = json::parse(payload);
                BotAssignment a = j.get<BotAssignment>();

                std::cout << "received bot assignment run_id=" << a.run_id 
                          << " phase=" << a.phase 
                          << " bot_count=" << a.bot_count 
                          << " endpoint=" << a.endpoint_url << std::endl;

                handle_assignment(a);
            } catch (const std::exception& e) {
                std::cerr << "failed to unmarshal assignment: " << e.what() << std::endl;
            }
        }
    });
}

void Worker::stop() {
    if (running_) {
        running_ = false;
        timer_cv_.notify_all();
        if (consumer_thread_.joinable()) {
            consumer_thread_.join();
        }
        std::lock_guard<std::mutex> lock(timer_mu_);
        for (auto& t : timer_threads_) {
            if (t.joinable()) t.join();
        }
    }
}

void Worker::wait() {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [run_id, fleet] : fleets_) {
        fleet->stop();
    }
    fleets_.clear();
}

void Worker::handle_assignment(const BotAssignment& a) {
    if (a.phase == "stop") {
        stop_fleet(a.run_id);
        return;
    }

    int bot_count = a.bot_count;
    if (bot_count > cfg_.max_bots_per_worker) {
        bot_count = cfg_.max_bots_per_worker;
    }

    stop_fleet(a.run_id);

    auto fleet = std::make_unique<BotFleet>(a.run_id);

    for (int i = 0; i < bot_count; ++i) {
        BotConfig bcfg;
        bcfg.bot_id = cfg_.worker_id + "-" + std::to_string(i);
        bcfg.run_id = a.run_id;
        bcfg.endpoint_url = a.endpoint_url;
        bcfg.orders_per_sec = a.orders_per_sec;
        bcfg.limit_ratio = a.limit_ratio;
        bcfg.market_ratio = a.market_ratio;
        bcfg.cancel_ratio = a.cancel_ratio;
        bcfg.symbols = a.symbols;
        bcfg.duration_ms = a.duration_ms;

        auto sender = std::make_shared<Sender>(a.endpoint_url);
        auto bot = std::make_unique<Bot>(bcfg, sender, reporter_);
        fleet->add_bot(std::move(bot));
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        fleets_[a.run_id] = std::move(fleet);
    }

    std::cout << "fleet spawned run_id=" << a.run_id 
              << " bot_count=" << bot_count 
              << " phase=" << a.phase << std::endl;

    if (a.duration_ms > 0) {
        std::lock_guard<std::mutex> lock(timer_mu_);
        timer_threads_.push_back(std::thread([this, run_id = a.run_id, dur = a.duration_ms]() {
            std::unique_lock<std::mutex> lk(timer_mu_);
            if (timer_cv_.wait_for(lk, std::chrono::milliseconds(dur), [this]() { return !running_.load(); })) {
                return;
            }
            lk.unlock();
            stop_fleet(run_id);
            std::cout << "fleet completed run_id=" << run_id << std::endl;
        }));
    }
}

void Worker::stop_fleet(const std::string& run_id) {
    std::unique_ptr<BotFleet> fleet_to_stop;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = fleets_.find(run_id);
        if (it != fleets_.end()) {
            fleet_to_stop = std::move(it->second);
            fleets_.erase(it);
        }
    }

    if (fleet_to_stop) {
        std::cout << "stopping fleet run_id=" << run_id << std::endl;
        fleet_to_stop->stop();
    }
}

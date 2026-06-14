#include <drogon/drogon.h>
#include <nlohmann/json.hpp>
#include <cppkafka/cppkafka.h>
#include <pqxx/pqxx>
#include <sw/redis++/redis++.h>
// Inline HDR Histogram replacement (the hdr_histogram C library is not in vcpkg)
// This provides a simple sorted-vector based percentile calculator with the same API surface.
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <algorithm>

struct hdr_histogram {
    std::vector<int64_t> counts;
    int64_t total_count = 0;
    int64_t _max = 0;
    int64_t _min = INT64_MAX;
    double _sum = 0.0;
    double _sum_sq = 0.0;

    hdr_histogram() { counts.resize(12000, 0); }
};

inline int hdr_init(int64_t, int64_t, int, struct hdr_histogram** h) {
    *h = new hdr_histogram();
    return 0;
}

inline size_t get_bucket(int64_t val) {
    if (val < 1000) return 0;
    size_t idx = static_cast<size_t>(std::log10(val) * 1000.0);
    if (idx >= 12000) return 11999;
    return idx;
}

inline int64_t get_bucket_value(size_t idx) {
    if (idx == 0) return 0;
    return static_cast<int64_t>(std::pow(10.0, idx / 1000.0));
}

inline bool hdr_record_value(struct hdr_histogram* h, int64_t value) {
    h->counts[get_bucket(value)]++;
    h->total_count++;
    h->_sum += (double)value;
    h->_sum_sq += (double)value * value;
    if (value > h->_max) h->_max = value;
    if (value < h->_min) h->_min = value;
    return true;
}

inline int64_t hdr_value_at_percentile(struct hdr_histogram* h, double percentile) {
    if (h->total_count == 0) return 0;
    int64_t target = std::ceil(percentile / 100.0 * h->total_count);
    int64_t sum = 0;
    for (size_t i = 0; i < h->counts.size(); ++i) {
        sum += h->counts[i];
        if (sum >= target) {
            return get_bucket_value(i);
        }
    }
    return h->_max;
}

inline double hdr_mean(struct hdr_histogram* h) {
    if (h->total_count == 0) return 0.0;
    return h->_sum / h->total_count;
}

inline int64_t hdr_max(struct hdr_histogram* h) { return h->_max; }
inline int64_t hdr_min(struct hdr_histogram* h) { return h->_min == INT64_MAX ? 0 : h->_min; }

inline double hdr_stddev(struct hdr_histogram* h) {
    if (h->total_count < 2) return 0.0;
    double mean = hdr_mean(h);
    double var = (h->_sum_sq / h->total_count) - (mean * mean);
    if (var < 0) var = 0;
    return std::sqrt(var);
}

// Iterator for recorded values
struct hdr_iter {
    struct hdr_histogram* h;
    size_t idx;
    int64_t lowest_equivalent_value;
    int64_t highest_equivalent_value;
    int64_t count_at_index;
};

inline void hdr_iter_recorded_init(struct hdr_iter* iter, struct hdr_histogram* h) {
    iter->h = h;
    iter->idx = 0;
}

inline bool hdr_iter_next(struct hdr_iter* iter) {
    while (iter->idx < iter->h->counts.size() && iter->h->counts[iter->idx] == 0) {
        iter->idx++;
    }
    if (iter->idx >= iter->h->counts.size()) return false;
    
    iter->lowest_equivalent_value = get_bucket_value(iter->idx);
    iter->highest_equivalent_value = get_bucket_value(iter->idx + 1);
    if (iter->idx == 0) iter->highest_equivalent_value = 1000;
    iter->count_at_index = iter->h->counts[iter->idx];
    iter->idx++;
    return true;
}
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <chrono>
#include <cmath>
#include <memory>
#include <shared_mutex>
#include <condition_variable>
#include <queue>
#include <optional>
#include <atomic>

using nlohmann::json;

std::atomic<bool> g_stop_flag{false};

struct Config {
    std::string port = "8094";
    std::string redpanda_brokers = "localhost:19092";
    std::string telemetry_topic = "telemetry-raw";
    std::string consumer_group = "scoring-engine";
    std::string timescaledb_url = "postgres://platform:platform@localhost:5433/telemetry";
    std::string redis_addr = "tcp://localhost:6379";
};

Config load_config() {
    Config cfg;
    if (auto p = std::getenv("SCORING_PORT")) cfg.port = p;
    if (auto p = std::getenv("REDPANDA_BROKERS")) cfg.redpanda_brokers = p;
    if (auto p = std::getenv("TELEMETRY_TOPIC")) cfg.telemetry_topic = p;
    if (auto p = std::getenv("CONSUMER_GROUP")) cfg.consumer_group = p;
    if (auto p = std::getenv("TIMESCALEDB_URL")) cfg.timescaledb_url = p;
    if (auto p = std::getenv("REDIS_ADDR")) {
        std::string addr = p;
        if (addr.find("://") == std::string::npos) {
            addr = "tcp://" + addr;
        }
        cfg.redis_addr = addr;
    }
    return cfg;
}

struct TelemetryEvent {
    std::string benchmark_run_id;
    std::string bot_id;
    std::string order_id;
    std::string order_type;
    std::string symbol;
    int64_t sent_at_ns = 0;
    int64_t acked_at_ns = 0;
    int64_t latency_ns = 0;
    bool success = false;
    std::string error_code;
};

template<typename T>
class Channel {
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<T> write_buf;
    bool closed = false;
public:
    Channel() { write_buf.reserve(5000); }
    void push(T val) {
        std::lock_guard<std::mutex> lk(mtx);
        write_buf.push_back(std::move(val));
        if (write_buf.size() >= 1000) cv.notify_one();
    }
    bool pop_batch(std::vector<T>& out_batch, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, timeout, [this]{ return write_buf.size() >= 1000 || closed; });
        if (write_buf.empty()) return false;
        out_batch.swap(write_buf);
        return true;
    }
    void close() {
        std::lock_guard<std::mutex> lk(mtx);
        closed = true;
        cv.notify_all();
    }
};

struct RunMetrics {
    std::shared_mutex mu;
    std::string run_id;
    struct hdr_histogram* histogram = nullptr;
    
    std::vector<int64_t> tps_window;
    int64_t current_tps = 0;
    int64_t max_tps = 0;
    
    int64_t total_orders = 0;
    int64_t successful_orders = 0;
    int64_t failed_orders = 0;
    
    std::chrono::system_clock::time_point started_at;
    std::chrono::system_clock::time_point last_event_at;
    
    RunMetrics(const std::string& id) : run_id(id) {
        hdr_init(1000, 30000000000LL, 3, &histogram);
        started_at = std::chrono::system_clock::now();
    }
    ~RunMetrics() {
        if (histogram) delete histogram;
    }
};

struct MetricsSnapshot {
    std::string run_id;
    int64_t latency_p50_us = 0;
    int64_t latency_p90_us = 0;
    int64_t latency_p99_us = 0;
    int64_t latency_avg_us = 0;
    int64_t latency_max_us = 0;
    int64_t current_tps = 0;
    int64_t max_tps = 0;
    int64_t total_orders = 0;
    int64_t successful_orders = 0;
    int64_t failed_orders = 0;
    double success_rate = 0.0;
    double elapsed_seconds = 0.0;
};

void to_json(json& j, const MetricsSnapshot& p) {
    j = json{
        {"run_id", p.run_id},
        {"latency_p50_us", p.latency_p50_us},
        {"latency_p90_us", p.latency_p90_us},
        {"latency_p99_us", p.latency_p99_us},
        {"latency_avg_us", p.latency_avg_us},
        {"latency_max_us", p.latency_max_us},
        {"current_tps", p.current_tps},
        {"max_tps", p.max_tps},
        {"total_orders", p.total_orders},
        {"successful_orders", p.successful_orders},
        {"failed_orders", p.failed_orders},
        {"success_rate", p.success_rate},
        {"elapsed_seconds", p.elapsed_seconds}
    };
}

struct HistogramBucket {
    int64_t from_us;
    int64_t to_us;
    int64_t count;
};

void to_json(json& j, const HistogramBucket& b) {
    j = json{{"from_us", b.from_us}, {"to_us", b.to_us}, {"count", b.count}};
}

struct HistogramData {
    std::vector<HistogramBucket> buckets;
    int64_t p50_us = 0;
    int64_t p90_us = 0;
    int64_t p99_us = 0;
    int64_t p999_us = 0;
    int64_t min_us = 0;
    int64_t max_us = 0;
    int64_t mean_us = 0;
    double std_dev_us = 0.0;
    int64_t count = 0;
};

void to_json(json& j, const HistogramData& d) {
    j = json{
        {"buckets", d.buckets},
        {"p50_us", d.p50_us},
        {"p90_us", d.p90_us},
        {"p99_us", d.p99_us},
        {"p999_us", d.p999_us},
        {"min_us", d.min_us},
        {"max_us", d.max_us},
        {"mean_us", d.mean_us},
        {"std_dev_us", d.std_dev_us},
        {"count", d.count}
    };
}

struct FinalScore {
    std::string run_id;
    std::string team_id;
    double composite_score = 0.0;
    double latency_score = 0.0;
    double throughput_score = 0.0;
    double correctness_score = 0.0;
    int64_t latency_p50_us = 0;
    int64_t latency_p90_us = 0;
    int64_t latency_p99_us = 0;
    int64_t max_tps = 0;
    double correctness = 0.0;
    int64_t total_orders = 0;
    int64_t successful_orders = 0;
};

void to_json(json& j, const FinalScore& s) {
    j = json{
        {"run_id", s.run_id},
        {"team_id", s.team_id},
        {"composite_score", s.composite_score},
        {"latency_score", s.latency_score},
        {"throughput_score", s.throughput_score},
        {"correctness_score", s.correctness_score},
        {"latency_p50_us", s.latency_p50_us},
        {"latency_p90_us", s.latency_p90_us},
        {"latency_p99_us", s.latency_p99_us},
        {"max_tps", s.max_tps},
        {"correctness", s.correctness},
        {"total_orders", s.total_orders},
        {"successful_orders", s.successful_orders}
    };
}

std::string format_timestamp(int64_t ns) {
    auto seconds = ns / 1000000000;
    auto nanoseconds = ns % 1000000000;
    std::chrono::system_clock::time_point tp{std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::seconds(seconds) + std::chrono::nanoseconds(nanoseconds))};
    auto time_c = std::chrono::system_clock::to_time_t(tp);
    struct tm tm_buf;
    gmtime_r(&time_c, &tm_buf);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return std::string(buf) + "." + std::to_string(nanoseconds) + "+00";
}

class Scorer {
public:
    Scorer(Config cfg) : config(cfg) {
        redis = std::make_unique<sw::redis::Redis>(cfg.redis_addr);
        db_conn = std::make_unique<pqxx::connection>(cfg.timescaledb_url);
    }
    
    void Ingest(const TelemetryEvent& event) {
        std::shared_ptr<RunMetrics> m;
        {
            std::shared_lock lock(runs_mu);
            auto it = runs.find(event.benchmark_run_id);
            if (it != runs.end()) {
                m = it->second;
            }
        }
        if (!m) {
            std::unique_lock lock(runs_mu);
            auto it = runs.find(event.benchmark_run_id);
            if (it == runs.end()) {
                m = std::make_shared<RunMetrics>(event.benchmark_run_id);
                runs[event.benchmark_run_id] = m;
            } else {
                m = it->second;
            }
        }
        
        {
            std::unique_lock lock(m->mu);
            m->total_orders++;
            if (event.success) {
                m->successful_orders++;
            } else {
                m->failed_orders++;
            }
            
            if (event.latency_ns > 0 && m->histogram) {
                if (!hdr_record_value(m->histogram, event.latency_ns)) {
                    hdr_record_value(m->histogram, 30000000000LL);
                }
            }
            
            m->current_tps++;
            m->last_event_at = std::chrono::system_clock::now();
        }
        
        telemetry_buf.push(event);
    }
    
    void flush_tps() {
        std::vector<std::string> run_ids;
        {
            std::shared_lock lock(runs_mu);
            for (const auto& [id, _] : runs) {
                run_ids.push_back(id);
            }
        }
        
        for (const auto& id : run_ids) {
            std::shared_ptr<RunMetrics> m;
            {
                std::shared_lock lock(runs_mu);
                auto it = runs.find(id);
                if (it == runs.end()) continue;
                m = it->second;
            }
            
            {
                std::unique_lock lock(m->mu);
                int64_t tps = m->current_tps;
                m->tps_window.push_back(tps);
                if (m->tps_window.size() > 60) {
                    m->tps_window.erase(m->tps_window.begin());
                }
                if (tps > m->max_tps) {
                    m->max_tps = tps;
                }
                m->current_tps = 0;
            }
        }
    }
    
    void run_flush_loop() {
        while (!g_stop_flag) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (g_stop_flag) break;
            flush_tps();
        }
    }
    
    void run_timescale_db_writer() {
        std::vector<TelemetryEvent> batch;
        
        auto flush = [&]() {
            if (batch.empty()) return;
            try {
                pqxx::work w(*db_conn);
                auto stream = pqxx::stream_to::table(w, pqxx::table_path{"telemetry_raw"}, {"time", "run_id", "bot_id", "order_id", "order_type", "latency_ns", "success", "error_code", "symbol"});
                
                for (auto& e : batch) {
                    stream << std::make_tuple(
                        format_timestamp(e.acked_at_ns),
                        e.benchmark_run_id,
                        e.bot_id,
                        e.order_id,
                        e.order_type,
                        e.latency_ns,
                        e.success,
                        e.error_code,
                        e.symbol
                    );
                }
                stream.complete();
                w.commit();
            } catch (const std::exception& ex) {
                std::cerr << "Failed to batch insert: " << ex.what() << "\n";
            }
            batch.clear();
        };

        while (!g_stop_flag) {
            if (telemetry_buf.pop_batch(batch, std::chrono::seconds(1))) {
                flush();
            } else {
                flush();
            }
        }
        flush();
    }
    
    std::optional<MetricsSnapshot> get_snapshot(const std::string& run_id) {
        std::shared_ptr<RunMetrics> m;
        {
            std::shared_lock lock(runs_mu);
            auto it = runs.find(run_id);
            if (it == runs.end()) return std::nullopt;
            m = it->second;
        }
        
        std::unique_lock lock(m->mu);
        MetricsSnapshot snap;
        snap.run_id = run_id;
        snap.total_orders = m->total_orders;
        snap.successful_orders = m->successful_orders;
        snap.failed_orders = m->failed_orders;
        snap.max_tps = m->max_tps;
        auto now = std::chrono::system_clock::now();
        snap.elapsed_seconds = std::chrono::duration<double>(now - m->started_at).count();
        
        if (m->total_orders > 0) {
            snap.success_rate = static_cast<double>(m->successful_orders) / m->total_orders;
        }
        
        if (m->histogram && m->histogram->total_count > 0) {
            snap.latency_p50_us = hdr_value_at_percentile(m->histogram, 50.0) / 1000;
            snap.latency_p90_us = hdr_value_at_percentile(m->histogram, 90.0) / 1000;
            snap.latency_p99_us = hdr_value_at_percentile(m->histogram, 99.0) / 1000;
            snap.latency_avg_us = static_cast<int64_t>(hdr_mean(m->histogram)) / 1000;
            snap.latency_max_us = hdr_max(m->histogram) / 1000;
        }
        
        if (!m->tps_window.empty()) {
            snap.current_tps = m->tps_window.back();
        }
        
        return snap;
    }
    
    std::optional<HistogramData> get_histogram(const std::string& run_id) {
        std::shared_ptr<RunMetrics> m;
        {
            std::shared_lock lock(runs_mu);
            auto it = runs.find(run_id);
            if (it == runs.end()) return std::nullopt;
            m = it->second;
        }
        
        std::unique_lock lock(m->mu);
        HistogramData data;
        if (!m->histogram || m->histogram->total_count == 0) {
            return data;
        }
        
        data.p50_us = hdr_value_at_percentile(m->histogram, 50.0) / 1000;
        data.p90_us = hdr_value_at_percentile(m->histogram, 90.0) / 1000;
        data.p99_us = hdr_value_at_percentile(m->histogram, 99.0) / 1000;
        data.p999_us = hdr_value_at_percentile(m->histogram, 99.9) / 1000;
        data.min_us = hdr_min(m->histogram) / 1000;
        data.max_us = hdr_max(m->histogram) / 1000;
        data.mean_us = static_cast<int64_t>(hdr_mean(m->histogram)) / 1000;
        data.std_dev_us = hdr_stddev(m->histogram) / 1000.0;
        data.count = m->histogram->total_count;
        
        hdr_iter iter;
        hdr_iter_recorded_init(&iter, m->histogram);
        while (hdr_iter_next(&iter)) {
            data.buckets.push_back({
                iter.lowest_equivalent_value / 1000,
                iter.highest_equivalent_value / 1000,
                iter.count_at_index
            });
        }
        
        return data;
    }
    
    std::optional<FinalScore> FinalizeRun(const std::string& run_id, const std::string& team_id) {
        std::shared_ptr<RunMetrics> m;
        {
            std::shared_lock lock(runs_mu);
            auto it = runs.find(run_id);
            if (it == runs.end()) return std::nullopt;
            m = it->second;
        }
        
        std::unique_lock lock(m->mu);
        
        FinalScore score;
        score.run_id = run_id;
        score.team_id = team_id;
        
        if (m->histogram && m->histogram->total_count > 0) {
            score.latency_p50_us = hdr_value_at_percentile(m->histogram, 50.0) / 1000;
            score.latency_p90_us = hdr_value_at_percentile(m->histogram, 90.0) / 1000;
            score.latency_p99_us = hdr_value_at_percentile(m->histogram, 99.0) / 1000;
        }
        
        score.max_tps = m->max_tps;
        if (m->total_orders > 0) {
            score.correctness = static_cast<double>(m->successful_orders) / m->total_orders;
        }
        score.total_orders = m->total_orders;
        score.successful_orders = m->successful_orders;
        
        double latency_ms = score.latency_p99_us / 1000.0;
        if (latency_ms < 1.0) latency_ms = 1.0;
        score.latency_score = std::min(1.0, 100.0 / latency_ms);
        score.throughput_score = std::min(1.0, static_cast<double>(score.max_tps) / 10000.0);
        score.correctness_score = score.correctness;
        
        score.composite_score = (0.5 * score.latency_score + 0.5 * score.throughput_score) * score.correctness_score;
        
        std::string histogram_json = "[]";
        if (m->histogram && m->histogram->total_count > 0) {
            nlohmann::json buckets = nlohmann::json::array();
            hdr_iter iter;
            hdr_iter_recorded_init(&iter, m->histogram);
            while (hdr_iter_next(&iter)) {
                buckets.push_back({
                    {"min_value_us", iter.lowest_equivalent_value / 1000},
                    {"max_value_us", iter.highest_equivalent_value / 1000},
                    {"count", iter.count_at_index}
                });
            }
            histogram_json = buckets.dump();
        }

        if (!team_id.empty() && redis) {
            try {
                std::cout << "[finalize] writing scores for team='" << team_id 
                          << "' run=" << run_id 
                          << " composite=" << score.composite_score << "\n";
                auto pipe = redis->pipeline();
                pipe.zadd("leaderboard", team_id, score.composite_score);
                
                std::unordered_map<std::string, std::string> hash_vals = {
                    {"composite", std::to_string(score.composite_score)},
                    {"latency_p50", std::to_string(score.latency_p50_us)},
                    {"latency_p90", std::to_string(score.latency_p90_us)},
                    {"latency_p99", std::to_string(score.latency_p99_us)},
                    {"max_tps", std::to_string(score.max_tps)},
                    {"current_tps", std::to_string(m->tps_window.empty() ? 0 : m->tps_window.back())},
                    {"correctness", std::to_string(score.correctness)},
                    {"histogram", histogram_json},
                    {"run_id", run_id}
                };
                pipe.hmset("scores:" + team_id, hash_vals.begin(), hash_vals.end());
                pipe.publish("leaderboard_updates", team_id);
                pipe.exec();
                std::cout << "[finalize] Redis leaderboard updated successfully for team='" << team_id << "'\n";
            } catch (const std::exception& e) {
                std::cerr << "Redis update failed: " << e.what() << "\n";
            }
        } else {
            std::cerr << "[finalize] WARNING: skipping Redis write — team_id='" << team_id 
                      << "' redis=" << (redis ? "connected" : "null") << "\n";
        }
        
        return score;
    }

    void shutdown() {
        g_stop_flag = true;
        telemetry_buf.close();
    }

private:
    Config config;
    std::unique_ptr<sw::redis::Redis> redis;
    std::unique_ptr<pqxx::connection> db_conn;
    
    Channel<TelemetryEvent> telemetry_buf;
    std::shared_mutex runs_mu;
    std::unordered_map<std::string, std::shared_ptr<RunMetrics>> runs;
};

std::unique_ptr<Scorer> global_scorer;

void run_consumer(Config cfg) {
    cppkafka::Configuration config = {
        { "metadata.broker.list", cfg.redpanda_brokers },
        { "group.id", cfg.consumer_group },
        { "auto.offset.reset", "earliest" }
    };
    cppkafka::Consumer consumer(config);
    consumer.subscribe({ cfg.telemetry_topic });
    
    std::cout << "telemetry consumer started\n";
    
    while (!g_stop_flag) {
        auto msgs = consumer.poll_batch(100, std::chrono::milliseconds(100));
        for (auto& msg : msgs) {
            if (msg.get_error()) continue;
            try {
                std::string payload(msg.get_payload().begin(), msg.get_payload().end());
                auto j = json::parse(payload);
                TelemetryEvent event;
                if (j.contains("benchmark_run_id")) event.benchmark_run_id = j["benchmark_run_id"];
                if (j.contains("bot_id")) event.bot_id = j["bot_id"];
                if (j.contains("client_order_id")) event.order_id = j["client_order_id"];
                if (j.contains("order_type")) event.order_type = j["order_type"];
                if (j.contains("symbol")) event.symbol = j["symbol"];
                if (j.contains("sent_at_ns")) event.sent_at_ns = j["sent_at_ns"];
                if (j.contains("acked_at_ns")) event.acked_at_ns = j["acked_at_ns"];
                if (j.contains("latency_ns")) event.latency_ns = j["latency_ns"];
                if (j.contains("success")) event.success = j["success"];
                if (j.contains("error_code")) event.error_code = j["error_code"];
                
                if (global_scorer) {
                    global_scorer->Ingest(event);
                }
            } catch (...) {
            }
        }
    }
}

int main() {
    Config cfg = load_config();
    std::cout << "starting scoring engine on port " << cfg.port << "\n";
    
    try {
        global_scorer = std::make_unique<Scorer>(cfg);
    } catch (const std::exception& e) {
        std::cerr << "Failed to initialize scorer: " << e.what() << "\n";
        return 1;
    }
    
    std::thread flush_thread(&Scorer::run_flush_loop, global_scorer.get());
    std::thread db_thread(&Scorer::run_timescale_db_writer, global_scorer.get());
    std::thread consumer_thread(run_consumer, cfg);
    
    drogon::app().registerHandler("/health", 
        [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            json j = {{"status", "healthy"}, {"service", "scoring-engine"}};
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k200OK);
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(j.dump());
            callback(resp);
        });
        
    drogon::app().registerHandler("/api/v1/benchmark/{runID}/telemetry", 
        [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, std::string runID) {
            auto snap = global_scorer->get_snapshot(runID);
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            if (!snap) {
                resp->setStatusCode(drogon::k404NotFound);
                json j = {{"error", "run not found"}};
                resp->setBody(j.dump());
            } else {
                resp->setStatusCode(drogon::k200OK);
                json j = *snap;
                resp->setBody(j.dump());
            }
            callback(resp);
        });
        
    drogon::app().registerHandler("/api/v1/benchmark/{runID}/telemetry/histogram", 
        [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, std::string runID) {
            auto data = global_scorer->get_histogram(runID);
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            if (!data) {
                resp->setStatusCode(drogon::k404NotFound);
                json j = {{"error", "run not found"}};
                resp->setBody(j.dump());
            } else {
                resp->setStatusCode(drogon::k200OK);
                json j = *data;
                resp->setBody(j.dump());
            }
            callback(resp);
        });
        
    drogon::app().registerHandler("/api/v1/benchmark/{runID}/telemetry/finalize", 
        [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, std::string runID) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            try {
                auto j = json::parse(req->body());
                std::string team_id = j.value("team_id", "");
                auto score = global_scorer->FinalizeRun(runID, team_id);
                if (!score) {
                    resp->setStatusCode(drogon::k404NotFound);
                    json err = {{"error", "run not found"}};
                    resp->setBody(err.dump());
                } else {
                    resp->setStatusCode(drogon::k200OK);
                    json res = *score;
                    resp->setBody(res.dump());
                }
            } catch (...) {
                resp->setStatusCode(drogon::k400BadRequest);
                json err = {{"error", "invalid request"}};
                resp->setBody(err.dump());
            }
            callback(resp);
        }, {drogon::Post});
        
    drogon::app().addListener("0.0.0.0", std::stoi(cfg.port));
    drogon::app().run();
    
    global_scorer->shutdown();
    flush_thread.join();
    db_thread.join();
    consumer_thread.join();
    
    return 0;
}

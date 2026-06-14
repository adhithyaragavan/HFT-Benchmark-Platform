#include <drogon/drogon.h>
#include <drogon/WebSocketController.h>
#include <drogon/HttpController.h>
#include <sw/redis++/redis++.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <thread>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <mutex>
#include <unordered_set>

using namespace drogon;

// --- Config & State ---
struct Config {
    std::string port;
    std::string redisAddr;
};

Config loadConfig() {
    Config cfg;
    const char* port_env = std::getenv("LEADERBOARD_PORT");
    cfg.port = port_env ? port_env : "8093";
    const char* redis_env = std::getenv("REDIS_ADDR");
    cfg.redisAddr = redis_env ? redis_env : "localhost:6379";
    return cfg;
}

std::atomic<bool> has_update{false};
std::atomic<bool> stop_subscriber{false};
std::shared_ptr<sw::redis::Redis> g_redis;

std::mutex clients_mu;
std::unordered_set<WebSocketConnectionPtr> clients;

void broadcast_update(const std::string& message) {
    std::lock_guard<std::mutex> lock(clients_mu);
    for (auto it = clients.begin(); it != clients.end(); ) {
        if ((*it)->connected()) {
            (*it)->send(message);
            ++it;
        } else {
            it = clients.erase(it);
        }
    }
}

// --- Models ---
struct LeaderboardEntry {
    int rank;
    std::string team_id;
    double composite_score;
    int64_t latency_p50_us;
    int64_t latency_p90_us;
    int64_t latency_p99_us;
    int64_t max_tps;
    double correctness;
};

void to_json(nlohmann::json& j, const LeaderboardEntry& e) {
    j = nlohmann::json{
        {"rank", e.rank},
        {"team_id", e.team_id},
        {"composite_score", e.composite_score},
        {"latency_p50_us", e.latency_p50_us},
        {"latency_p90_us", e.latency_p90_us},
        {"latency_p99_us", e.latency_p99_us},
        {"max_tps", e.max_tps},
        {"correctness", e.correctness}
    };
}

struct LeaderboardUpdate {
    std::string type;
    std::vector<LeaderboardEntry> entries;
    std::string updated_at;
};

void to_json(nlohmann::json& j, const LeaderboardUpdate& u) {
    j = nlohmann::json{
        {"type", u.type},
        {"entries", u.entries},
        {"updated_at", u.updated_at}
    };
}

// --- Utils ---
std::string current_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count() << "Z";
    return ss.str();
}

std::vector<LeaderboardEntry> fetchLeaderboard(sw::redis::Redis& redis) {
    std::vector<LeaderboardEntry> entries;
    std::vector<std::pair<std::string, double>> results;
    redis.zrevrange("leaderboard", 0, 49, std::back_inserter(results));
    
    if (results.empty()) return entries;

    auto pipe = redis.pipeline();
    for (const auto& [teamID, score] : results) {
        pipe.hgetall("scores:" + teamID);
    }
    auto replies = pipe.exec();

    int rank = 1;
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& [teamID, score] = results[i];
        LeaderboardEntry entry;
        entry.rank = rank++;
        entry.team_id = teamID;
        entry.composite_score = score;
        entry.latency_p50_us = 0;
        entry.latency_p90_us = 0;
        entry.latency_p99_us = 0;
        entry.max_tps = 0;
        entry.correctness = 0.0;
        
        std::unordered_map<std::string, std::string> details;
        try {
            details = replies.get<std::unordered_map<std::string, std::string>>(i);
        } catch (...) {}
        
        auto parse_json_number_int = [](const std::string& str) -> int64_t {
            try { return nlohmann::json::parse(str).get<int64_t>(); } catch (...) { return 0; }
        };
        auto parse_json_number_double = [](const std::string& str) -> double {
            try { return nlohmann::json::parse(str).get<double>(); } catch (...) { return 0.0; }
        };
        
        if (details.count("latency_p50")) entry.latency_p50_us = parse_json_number_int(details["latency_p50"]);
        if (details.count("latency_p90")) entry.latency_p90_us = parse_json_number_int(details["latency_p90"]);
        if (details.count("latency_p99")) entry.latency_p99_us = parse_json_number_int(details["latency_p99"]);
        if (details.count("max_tps")) entry.max_tps = parse_json_number_int(details["max_tps"]);
        if (details.count("correctness")) entry.correctness = parse_json_number_double(details["correctness"]);
        
        entries.push_back(entry);
    }
    return entries;
}

// --- Controllers ---
class LeaderboardController : public drogon::HttpController<LeaderboardController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(LeaderboardController::health, "/health", Get);
    ADD_METHOD_TO(LeaderboardController::getLeaderboard, "/api/v1/leaderboard", Get);
    ADD_METHOD_TO(LeaderboardController::updatePhase, "/api/v1/leaderboard/phase", Post);
    METHOD_LIST_END
    
    void health(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) const {
        nlohmann::json j = {{"status", "healthy"}, {"service", "leaderboard-svc"}};
        auto resp = HttpResponse::newHttpResponse();
        resp->setContentTypeCode(CT_APPLICATION_JSON);
        resp->setBody(j.dump());
        callback(resp);
    }
    
    void getLeaderboard(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) const {
        try {
            auto entries = fetchLeaderboard(*g_redis);
            nlohmann::json j = {{"success", true}, {"data", entries}};
            auto resp = HttpResponse::newHttpResponse();
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            resp->setBody(j.dump());
            callback(resp);
        } catch (const std::exception& e) {
            nlohmann::json j = {{"error", e.what()}};
            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k500InternalServerError);
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            resp->setBody(j.dump());
            callback(resp);
        }
    }

    void updatePhase(const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback) const {
        try {
            auto j = nlohmann::json::parse(req->getBody());
            broadcast_update(j.dump());
            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k200OK);
            callback(resp);
        } catch (...) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k400BadRequest);
            callback(resp);
        }
    }
};

std::unordered_map<WebSocketConnectionPtr, std::string> detail_subs;

class LeaderboardStream : public drogon::WebSocketController<LeaderboardStream>
{
public:
    LeaderboardStream() {
        drogon::app().getLoop()->runEvery(10.0, [this]() {
            std::lock_guard<std::mutex> lock(clients_mu);
            for (auto ws : clients) {
                if (!ws->disconnected()) ws->send("", drogon::WebSocketMessageType::Ping);
            }
        });
    }
    
    void handleNewMessage(const WebSocketConnectionPtr& wsConnPtr, std::string &&message, const WebSocketMessageType &type) override {
        if (type == WebSocketMessageType::Text) {
            try {
                auto j = nlohmann::json::parse(message);
                if (j.count("type") && j["type"] == "subscribe_detail" && j.count("submission_id")) {
                    std::lock_guard<std::mutex> lock(clients_mu);
                    detail_subs[wsConnPtr] = j["submission_id"].get<std::string>();
                    LOG_INFO << "Client subscribed to details for: " << detail_subs[wsConnPtr];
                }
            } catch (...) {}
        }
    }
    
    void handleNewConnection(const HttpRequestPtr &req, const WebSocketConnectionPtr& wsConnPtr) override {
        LOG_INFO << "new WebSocket client connected";
        {
            std::lock_guard<std::mutex> lock(clients_mu);
            clients.insert(wsConnPtr);
        }
        
        try {
            auto entries = fetchLeaderboard(*g_redis);
            LeaderboardUpdate update{"full", entries, current_iso8601()};
            nlohmann::json j = update;
            wsConnPtr->send(j.dump());
        } catch (const std::exception& e) {
            LOG_ERROR << "Failed to send initial snapshot: " << e.what();
        }
    }
    
    void handleConnectionClosed(const WebSocketConnectionPtr& wsConnPtr) override {
        LOG_INFO << "client disconnected";
        std::lock_guard<std::mutex> lock(clients_mu);
        clients.erase(wsConnPtr);
        detail_subs.erase(wsConnPtr);
    }
    
    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/api/v1/leaderboard/stream");
    WS_PATH_LIST_END
};

// --- Main ---
int main() {
    auto cfg = loadConfig();

    // Parse redis address
    std::string redis_host = "127.0.0.1";
    int redis_port = 6379;
    std::string redis_raw = cfg.redisAddr;
    if (redis_raw.substr(0, 6) == "tcp://") redis_raw = redis_raw.substr(6);
    auto cp = redis_raw.find(':');
    if (cp != std::string::npos) {
        redis_host = redis_raw.substr(0, cp);
        redis_port = std::stoi(redis_raw.substr(cp + 1));
    } else {
        redis_host = redis_raw;
    }

    sw::redis::ConnectionOptions main_opts;
    main_opts.host = redis_host;
    main_opts.port = redis_port;
    g_redis = std::make_shared<sw::redis::Redis>(main_opts);
    
    // Subscriber thread
    std::thread subscriber_thread([redis_host, redis_port]() {
        while (!stop_subscriber.load()) {
            try {
                sw::redis::ConnectionOptions opts;
                opts.host = redis_host;
                opts.port = redis_port;
                opts.socket_timeout = std::chrono::milliseconds(1000);
                sw::redis::Redis redis(opts);
                auto sub = redis.subscriber();
                sub.on_message([](std::string channel, std::string msg) {
                    LOG_DEBUG << "leaderboard update received: " << msg;
                    has_update.store(true);
                });
                sub.subscribe("leaderboard_updates");
                
                while (!stop_subscriber.load()) {
                    try {
                        sub.consume();
                    } catch (const sw::redis::TimeoutError&) {
                        // ignore
                    } catch (const sw::redis::Error& e) {
                        LOG_ERROR << "Redis subscriber consume error: " << e.what();
                        break; // Reconnect
                    }
                }
            } catch (const std::exception& e) {
                LOG_ERROR << "Redis subscriber error: " << e.what();
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    });

    app().registerSyncAdvice([](const HttpRequestPtr &req) -> HttpResponsePtr {
        if (req->method() == Options) {
            auto resp = HttpResponse::newHttpResponse();
            resp->addHeader("Access-Control-Allow-Origin", "*");
            resp->addHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
            resp->addHeader("Access-Control-Allow-Headers", "Content-Type");
            return resp;
        }
        return nullptr;
    });

    app().registerPostHandlingAdvice([](const HttpRequestPtr &req, const HttpResponsePtr &resp) {
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
        resp->addHeader("Access-Control-Allow-Headers", "Content-Type");
    });
    
    app().addListener("0.0.0.0", std::stoi(cfg.port));
    
    std::thread broadcast_thread([]() {
        while (!stop_subscriber.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (has_update.exchange(false)) {
                try {
                    auto entries = fetchLeaderboard(*g_redis);
                    LeaderboardUpdate update{"full", entries, current_iso8601()};
                    nlohmann::json j = update;
                    broadcast_update(j.dump());
                } catch (const std::exception& e) {
                    LOG_ERROR << "Failed to broadcast leaderboard: " << e.what();
                }
            }
            
            // Detail subscriptions broadcast
            std::unordered_set<std::string> active_subs;
            std::vector<WebSocketConnectionPtr> conns;
            {
                std::lock_guard<std::mutex> lock(clients_mu);
                for (const auto& [conn, sub_id] : detail_subs) {
                    active_subs.insert(sub_id);
                    conns.push_back(conn);
                }
            }
            
            for (const auto& sub_id : active_subs) {
                try {
                    std::unordered_map<std::string, std::string> details;
                    g_redis->hgetall("scores:" + sub_id, std::inserter(details, details.begin()));
                    
                    if (!details.empty()) {
                        nlohmann::json du;
                        du["type"] = "detail_update";
                        du["submission_id"] = sub_id;
                        auto get_i = [&](const std::string& k) -> int64_t {
                            if (details.count(k)) {
                                try { return std::stoll(details[k]); } catch(...) {}
                            }
                            return 0;
                        };
                        du["latency_p50_us"] = get_i("latency_p50");
                        du["latency_p90_us"] = get_i("latency_p90");
                        du["latency_p99_us"] = get_i("latency_p99");
                        du["current_tps"] = get_i("current_tps");
                        
                        if (details.count("histogram")) {
                            try { du["buckets"] = nlohmann::json::parse(details["histogram"]); } catch(...) {}
                        }
                        
                        std::string msg = du.dump();
                        std::lock_guard<std::mutex> lock(clients_mu);
                        for (auto& conn : conns) {
                            auto it = detail_subs.find(conn);
                            if (it != detail_subs.end() && it->second == sub_id && conn->connected()) {
                                conn->send(msg);
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR << "Detail update error: " << e.what();
                }
            }
        }
    });

    LOG_INFO << "starting leaderboard service on port " << cfg.port;
    app().run();
    
    stop_subscriber.store(true);
    if (subscriber_thread.joinable()) subscriber_thread.join();
    if (broadcast_thread.joinable()) broadcast_thread.join();
    
    return 0;
}

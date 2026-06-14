#include <drogon/drogon.h>
#include <nlohmann/json.hpp>
#include <cppkafka/cppkafka.h>
#include <mutex>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <string>
#include <vector>
#include <cmath>
#include <iostream>
#include <memory>
#include <atomic>
#include <condition_variable>
#include <shared_mutex>
#include <sstream>

using json = nlohmann::json;

// --- Config ---
struct Config {
    int port = 8092;
    std::vector<std::string> redpanda_brokers = {"localhost:19092"};
    std::string command_topic = "bot-commands";
    int warmup_duration_ms = 10000;
    int ramp_duration_ms = 20000;
    int cooldown_duration_ms = 10000;
    int default_orders_per_sec = 10;
    std::vector<std::string> default_symbols = {"AAPL", "GOOG", "MSFT", "AMZN", "TSLA"};
};

std::vector<std::string> split_string(const std::string& str, char delim) {
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;
    while (std::getline(ss, token, delim)) {
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }
    return tokens;
}

Config load_config() {
    Config cfg;
    if (const char* v = std::getenv("ORCHESTRATOR_PORT")) cfg.port = std::stoi(v);
    if (const char* v = std::getenv("REDPANDA_BROKERS")) cfg.redpanda_brokers = split_string(v, ',');
    if (const char* v = std::getenv("COMMAND_TOPIC")) cfg.command_topic = v;
    if (const char* v = std::getenv("WARMUP_DURATION_MS")) cfg.warmup_duration_ms = std::stoi(v);
    if (const char* v = std::getenv("RAMP_DURATION_MS")) cfg.ramp_duration_ms = std::stoi(v);
    if (const char* v = std::getenv("COOLDOWN_DURATION_MS")) cfg.cooldown_duration_ms = std::stoi(v);
    if (const char* v = std::getenv("DEFAULT_ORDERS_PER_SEC")) cfg.default_orders_per_sec = std::stoi(v);
    if (const char* v = std::getenv("DEFAULT_SYMBOLS")) cfg.default_symbols = split_string(v, ',');
    return cfg;
}

// --- Data Models ---
struct Profile {
    double limit_ratio = 0.0;
    double market_ratio = 0.0;
    double cancel_ratio = 0.0;
    int orders_per_sec_per_bot = 0;
    std::vector<std::string> symbols;
};
void to_json(json& j, const Profile& p) {
    j = json{{"limit_ratio", p.limit_ratio}, {"market_ratio", p.market_ratio},
             {"cancel_ratio", p.cancel_ratio}, {"orders_per_sec_per_bot", p.orders_per_sec_per_bot},
             {"symbols", p.symbols}};
}
void from_json(const json& j, Profile& p) {
    p.limit_ratio = j.value("limit_ratio", 0.0);
    p.market_ratio = j.value("market_ratio", 0.0);
    p.cancel_ratio = j.value("cancel_ratio", 0.0);
    p.orders_per_sec_per_bot = j.value("orders_per_sec_per_bot", 0);
    p.symbols = j.value("symbols", std::vector<std::string>{});
}

struct StartBenchmarkRequest {
    std::string run_id;
    std::string submission_id;
    std::string endpoint_url;
    int bot_count = 0;
    int duration_seconds = 0;
    Profile profile;
};
void to_json(json& j, const StartBenchmarkRequest& r) {
    j = json{{"run_id", r.run_id}, {"submission_id", r.submission_id},
             {"endpoint_url", r.endpoint_url}, {"bot_count", r.bot_count},
             {"duration_seconds", r.duration_seconds}, {"profile", r.profile}};
}
void from_json(const json& j, StartBenchmarkRequest& r) {
    r.run_id = j.value("run_id", "");
    r.submission_id = j.value("submission_id", "");
    r.endpoint_url = j.value("endpoint_url", "");
    r.bot_count = j.value("bot_count", 0);
    r.duration_seconds = j.value("duration_seconds", 0);
    r.profile = j.value("profile", Profile{});
}

struct StopBenchmarkRequest {
    std::string run_id;
};
void from_json(const json& j, StopBenchmarkRequest& r) {
    r.run_id = j.value("run_id", "");
}

struct BotAssignment {
    std::string run_id;
    std::string endpoint_url;
    int bot_count = 0;
    std::string phase;
    int duration_ms = 0;
    int orders_per_sec_per_bot = 0;
    double limit_ratio = 0.0;
    double market_ratio = 0.0;
    double cancel_ratio = 0.0;
    std::vector<std::string> symbols;
};
void to_json(json& j, const BotAssignment& a) {
    j = json{
        {"run_id", a.run_id},
        {"endpoint_url", a.endpoint_url},
        {"bot_count", a.bot_count},
        {"phase", a.phase},
        {"duration_ms", a.duration_ms},
        {"orders_per_sec_per_bot", a.orders_per_sec_per_bot},
        {"limit_ratio", a.limit_ratio},
        {"market_ratio", a.market_ratio},
        {"cancel_ratio", a.cancel_ratio},
        {"symbols", a.symbols}
    };
}

enum class RunStatus {
    Pending, Warmup, Ramping, Sustained, Cooldown, Completed, Stopped, Failed
};

std::string run_status_to_string(RunStatus status) {
    switch (status) {
        case RunStatus::Pending: return "pending";
        case RunStatus::Warmup: return "warmup";
        case RunStatus::Ramping: return "ramping";
        case RunStatus::Sustained: return "sustained";
        case RunStatus::Cooldown: return "cooldown";
        case RunStatus::Completed: return "completed";
        case RunStatus::Stopped: return "stopped";
        case RunStatus::Failed: return "failed";
    }
    return "unknown";
}

struct BenchmarkRun {
    std::string run_id;
    std::string submission_id;
    std::string endpoint_url;
    int bot_count = 0;
    int duration_seconds = 0;
    RunStatus status = RunStatus::Pending;
    std::string current_phase = "pending";
    std::string started_at;
    std::string completed_at;
    std::string error_msg;
    StartBenchmarkRequest profile;
    
    std::shared_ptr<std::atomic<bool>> cancel_flag;
    std::shared_ptr<std::mutex> cancel_mu;
    std::shared_ptr<std::condition_variable> cancel_cv;

    BenchmarkRun() 
        : cancel_flag(std::make_shared<std::atomic<bool>>(false)),
          cancel_mu(std::make_shared<std::mutex>()),
          cancel_cv(std::make_shared<std::condition_variable>()) {}
};

std::string now_string() {
    return trantor::Date::now().toDbString();
}

// --- Orchestrator ---
class Orchestrator {
public:
    Orchestrator(Config cfg, std::shared_ptr<cppkafka::Producer> producer)
        : cfg_(cfg), producer_(producer) {}

    std::shared_ptr<BenchmarkRun> start_benchmark(StartBenchmarkRequest req) {
        if (req.run_id.empty()) {
            req.run_id = drogon::utils::getUuid();
        }
        if (req.endpoint_url.empty()) throw std::invalid_argument("endpoint_url is required");
        if (req.bot_count <= 0) throw std::invalid_argument("bot_count must be positive");
        if (req.duration_seconds <= 0) throw std::invalid_argument("duration_seconds must be positive");

        if (req.profile.orders_per_sec_per_bot <= 0) req.profile.orders_per_sec_per_bot = cfg_.default_orders_per_sec;
        if (req.profile.symbols.empty()) req.profile.symbols = cfg_.default_symbols;
        if (req.profile.limit_ratio == 0 && req.profile.market_ratio == 0 && req.profile.cancel_ratio == 0) {
            req.profile.limit_ratio = 0.6;
            req.profile.market_ratio = 0.3;
            req.profile.cancel_ratio = 0.1;
        }

        double ratio_sum = req.profile.limit_ratio + req.profile.market_ratio + req.profile.cancel_ratio;
        if (std::abs(ratio_sum - 1.0) > 0.01) {
            throw std::invalid_argument("profile ratios must sum to 1.0");
        }

        std::shared_ptr<BenchmarkRun> run;
        {
            std::lock_guard<std::shared_mutex> lock(mu_);
            if (runs_.find(req.run_id) != runs_.end()) {
                auto existing = runs_[req.run_id];
                if (existing->status != RunStatus::Completed && existing->status != RunStatus::Stopped && existing->status != RunStatus::Failed) {
                    throw std::invalid_argument("benchmark run already running");
                }
            }

            run = std::make_shared<BenchmarkRun>();
            run->run_id = req.run_id;
            run->submission_id = req.submission_id;
            run->endpoint_url = req.endpoint_url;
            run->bot_count = req.bot_count;
            run->duration_seconds = req.duration_seconds;
            run->status = RunStatus::Pending;
            run->current_phase = "pending";
            run->started_at = now_string();
            run->profile = req;
            
            runs_[req.run_id] = run;
        }

        LOG_INFO << "starting benchmark run run_id=" << req.run_id;
        
        std::thread([this, run, req]() {
            this->execute_phases(run, req);
        }).detach();

        return run;
    }

    void stop_benchmark(const std::string& run_id) {
        std::shared_ptr<BenchmarkRun> run;
        {
            std::lock_guard<std::shared_mutex> lock(mu_);
            auto it = runs_.find(run_id);
            if (it == runs_.end()) throw std::invalid_argument("benchmark run not found");
            run = it->second;
            
            if (run->status == RunStatus::Completed || run->status == RunStatus::Stopped) {
                throw std::invalid_argument("benchmark run already " + run_status_to_string(run->status));
            }
            
            run->cancel_flag->store(true);
            run->cancel_cv->notify_all();
            run->status = RunStatus::Stopped;
            run->current_phase = "stopped";
            run->completed_at = now_string();
        }

        LOG_INFO << "stopping benchmark run run_id=" << run_id;

        BotAssignment stop_assignment;
        stop_assignment.run_id = run_id;
        stop_assignment.endpoint_url = run->endpoint_url;
        stop_assignment.phase = "stop";
        stop_assignment.bot_count = 0;

        publish_assignment(stop_assignment);
    }

    std::shared_ptr<BenchmarkRun> get_run_status(const std::string& run_id) {
        std::shared_lock<std::shared_mutex> lock(mu_);
        auto it = runs_.find(run_id);
        if (it == runs_.end()) throw std::runtime_error("benchmark run not found");
        return it->second;
    }

private:
    Config cfg_;
    std::shared_ptr<cppkafka::Producer> producer_;
    std::shared_mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<BenchmarkRun>> runs_;

    struct PhaseSpec {
        std::string name;
        RunStatus status;
        int bot_count;
        int duration_ms;
        bool is_ramp;
    };

    std::vector<PhaseSpec> build_phases(const StartBenchmarkRequest& req) {
        int warmup_bots = std::max(1, static_cast<int>(std::ceil(req.bot_count * 0.1)));
        
        int total_ms = req.duration_seconds * 1000;
        int warmup_ms = total_ms * 0.1;
        int ramp_ms = total_ms * 0.2;
        int burst_ms = total_ms * 0.2;
        int cooldown_ms = total_ms * 0.1;
        int peak_ms = total_ms - warmup_ms - ramp_ms - burst_ms - cooldown_ms;

        return {
            {"warmup", RunStatus::Warmup, warmup_bots, warmup_ms, false},
            {"ramp", RunStatus::Ramping, req.bot_count, ramp_ms, true},
            {"peak", RunStatus::Sustained, req.bot_count, peak_ms, false},
            {"burst", RunStatus::Sustained, req.bot_count * 2, burst_ms, false},
            {"cooldown", RunStatus::Cooldown, warmup_bots, cooldown_ms, false}
        };
    }

    void execute_phases(std::shared_ptr<BenchmarkRun> run, StartBenchmarkRequest req) {
        auto phases = build_phases(req);
        for (const auto& phase : phases) {
            if (run->cancel_flag->load()) {
                LOG_INFO << "benchmark run cancelled, aborting phases";
                return;
            }

            update_run_phase(run, phase.name, phase.status, phase.duration_ms);

            if (phase.is_ramp) {
                if (!execute_ramp_phase(run, req, phase)) return;
            } else {
                auto assignment = build_assignment(run->run_id, req, phase.name, phase.bot_count, phase.duration_ms);
                try {
                    publish_assignment(assignment);
                } catch (const std::exception& e) {
                    handle_phase_error(run, phase.name, e.what());
                    return;
                }
                
                if (!wait_for_phase(run, phase.duration_ms)) return;
            }
        }

        {
            std::lock_guard<std::shared_mutex> lock(mu_);
            if (run->status != RunStatus::Stopped && run->status != RunStatus::Failed) {
                run->status = RunStatus::Completed;
                run->current_phase = "completed";
                run->completed_at = now_string();
            }
        }
        
        update_run_phase(run, "idle", RunStatus::Completed, 0);
    }

    bool execute_ramp_phase(std::shared_ptr<BenchmarkRun> run, const StartBenchmarkRequest& req, const PhaseSpec& phase) {
        int warmup_bots = std::max(1, static_cast<int>(std::ceil(req.bot_count * 0.1)));
        int target_bots = req.bot_count;

        int step_interval_ms = 2000;
        int total_steps = std::max(1, phase.duration_ms / step_interval_ms);
        double bot_increment = static_cast<double>(target_bots - warmup_bots) / total_steps;

        for (int step = 0; step <= total_steps; ++step) {
            if (run->cancel_flag->load()) return false;

            int current_bots = warmup_bots + static_cast<int>(std::round(step * bot_increment));
            current_bots = std::min(current_bots, target_bots);
            current_bots = std::max(current_bots, 1);

            int remaining_ms = std::max(step_interval_ms, phase.duration_ms - (step * step_interval_ms));

            auto assignment = build_assignment(run->run_id, req, "ramp", current_bots, remaining_ms);
            try {
                publish_assignment(assignment);
            } catch (const std::exception& e) {
                handle_phase_error(run, "ramp", e.what());
                return false;
            }

            if (step < total_steps) {
                if (!wait_for_phase(run, step_interval_ms)) return false;
            }
        }
        return true;
    }

    BotAssignment build_assignment(const std::string& run_id, const StartBenchmarkRequest& req, const std::string& phase, int bot_count, int duration_ms) {
        BotAssignment a;
        a.run_id = run_id;
        a.endpoint_url = req.endpoint_url;
        a.bot_count = bot_count;
        a.phase = phase;
        a.duration_ms = duration_ms;
        a.orders_per_sec_per_bot = req.profile.orders_per_sec_per_bot;
        if (phase == "burst") {
            a.orders_per_sec_per_bot *= 2;
        }
        a.limit_ratio = req.profile.limit_ratio;
        a.market_ratio = req.profile.market_ratio;
        a.cancel_ratio = req.profile.cancel_ratio;
        a.symbols = req.profile.symbols;
        return a;
    }

    void publish_assignment(const BotAssignment& assignment) {
        json j = assignment;
        std::string data = j.dump();

        producer_->produce(cppkafka::MessageBuilder(cfg_.command_topic)
                               .key(assignment.run_id)
                               .payload(data));
        producer_->flush();
        LOG_DEBUG << "published bot assignment run_id=" << assignment.run_id;
    }

    bool wait_for_phase(std::shared_ptr<BenchmarkRun> run, int duration_ms) {
        std::unique_lock<std::mutex> lock(*(run->cancel_mu));
        bool cancelled = run->cancel_cv->wait_for(lock, std::chrono::milliseconds(duration_ms), [&]() {
            return run->cancel_flag->load();
        });
        return !cancelled;
    }

    void update_run_phase(std::shared_ptr<BenchmarkRun> run, const std::string& phase, RunStatus status, int duration_ms = 0) {
        {
            std::lock_guard<std::shared_mutex> lock(mu_);
            run->current_phase = phase;
            run->status = status;
        }

        if (status == RunStatus::Warmup || status == RunStatus::Ramping || status == RunStatus::Sustained || status == RunStatus::Cooldown || status == RunStatus::Completed) {
            auto client = drogon::HttpClient::newHttpClient("http://localhost:8093");
            auto req = drogon::HttpRequest::newHttpRequest();
            req->setPath("/api/v1/leaderboard/phase");
            req->setMethod(drogon::Post);
            req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            
            std::string display_phase = phase;
            std::transform(display_phase.begin(), display_phase.end(), display_phase.begin(), ::toupper);

            nlohmann::json j = {
                {"type", "phase"},
                {"current", display_phase},
                {"remaining_seconds", duration_ms / 1000}
            };
            req->setBody(j.dump());
            
            client->sendRequest(req, [](drogon::ReqResult res, const drogon::HttpResponsePtr& resp) {
                // ignore
            });
        }
    }

    void handle_phase_error(std::shared_ptr<BenchmarkRun> run, const std::string& phase, const std::string& error) {
        std::lock_guard<std::shared_mutex> lock(mu_);
        run->status = RunStatus::Failed;
        run->current_phase = phase;
        run->error_msg = error;
        run->completed_at = now_string();
        LOG_ERROR << "phase execution failed phase=" << phase << " error=" << error;
    }
};

// --- HTTP Handler ---
class OrchestratorHandler : public drogon::HttpController<OrchestratorHandler> {
public:
    static void init(std::shared_ptr<Orchestrator> orch) {
        orchestrator_ = orch;
    }

    METHOD_LIST_BEGIN
    ADD_METHOD_TO(OrchestratorHandler::healthz, "/healthz", drogon::Get);
    ADD_METHOD_TO(OrchestratorHandler::health, "/health", drogon::Get);
    ADD_METHOD_TO(OrchestratorHandler::start_benchmark, "/api/v1/orchestrator/start", drogon::Post);
    ADD_METHOD_TO(OrchestratorHandler::stop_benchmark, "/api/v1/orchestrator/stop", drogon::Post);
    ADD_METHOD_TO(OrchestratorHandler::get_status, "/api/v1/orchestrator/status/{1}", drogon::Get);
    METHOD_LIST_END

    void healthz(const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k200OK);
        callback(resp);
    }

    void health(const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        json data = {
            {"success", true},
            {"data", {
                {"status", "healthy"},
                {"service", "bot-orchestrator"},
                {"version", "1.0.0"}
            }}
        };
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k200OK);
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        resp->setBody(data.dump());
        callback(resp);
    }

    void start_benchmark(const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        try {
            auto req_json = nlohmann::json::parse(req->body());
            auto breq = req_json.get<StartBenchmarkRequest>();
            auto run = orchestrator_->start_benchmark(breq);

            json data = {
                {"success", true},
                {"data", {
                    {"run_id", run->run_id},
                    {"submission_id", run->submission_id},
                    {"endpoint_url", run->endpoint_url},
                    {"bot_count", run->bot_count},
                    {"duration_seconds", run->duration_seconds},
                    {"status", run_status_to_string(run->status)},
                    {"started_at", run->started_at}
                }}
            };
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k202Accepted);
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(data.dump());
            callback(resp);

        } catch (const nlohmann::json::exception& e) {
            json err = {{"success", false}, {"error", std::string("invalid request body: ") + e.what()}};
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k400BadRequest);
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(err.dump());
            callback(resp);
        } catch (const std::invalid_argument& e) {
            json err = {{"success", false}, {"error", e.what()}};
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k422UnprocessableEntity);
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(err.dump());
            callback(resp);
        } catch (const std::exception& e) {
            json err = {{"success", false}, {"error", e.what()}};
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k422UnprocessableEntity);
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(err.dump());
            callback(resp);
        }
    }

    void stop_benchmark(const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        try {
            auto req_json = nlohmann::json::parse(req->body());
            if (!req_json.contains("run_id")) {
                throw std::invalid_argument("run_id is required");
            }
            auto breq = req_json.get<StopBenchmarkRequest>();

            orchestrator_->stop_benchmark(breq.run_id);

            json data = {
                {"success", true},
                {"data", {
                    {"run_id", breq.run_id},
                    {"status", "stopped"},
                    {"message", "benchmark run stop signal sent"}
                }}
            };
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k200OK);
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(data.dump());
            callback(resp);
        } catch (const nlohmann::json::exception& e) {
            json err = {{"success", false}, {"error", std::string("invalid request body: ") + e.what()}};
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k400BadRequest);
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(err.dump());
            callback(resp);
        } catch (const std::invalid_argument& e) {
            json err = {{"success", false}, {"error", e.what()}};
            auto resp = drogon::HttpResponse::newHttpResponse();
            std::string msg = e.what();
            if (msg == "run_id is required") {
                resp->setStatusCode(drogon::k400BadRequest);
            } else {
                resp->setStatusCode(drogon::k422UnprocessableEntity);
            }
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(err.dump());
            callback(resp);
        } catch (const std::exception& e) {
            json err = {{"success", false}, {"error", e.what()}};
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k422UnprocessableEntity);
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(err.dump());
            callback(resp);
        }
    }

    void get_status(const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, std::string run_id) {
        try {
            if (run_id.empty()) {
                json err = {{"success", false}, {"error", "run_id is required"}};
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k400BadRequest);
                resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                resp->setBody(err.dump());
                callback(resp);
                return;
            }

            auto run = orchestrator_->get_run_status(run_id);

            json data = {
                {"success", true},
                {"data", {
                    {"run_id", run->run_id},
                    {"submission_id", run->submission_id},
                    {"endpoint_url", run->endpoint_url},
                    {"bot_count", run->bot_count},
                    {"status", run_status_to_string(run->status)},
                    {"current_phase", run->current_phase},
                    {"started_at", run->started_at}
                }}
            };
            if (!run->completed_at.empty()) data["data"]["completed_at"] = run->completed_at;
            if (!run->error_msg.empty()) data["data"]["error_msg"] = run->error_msg;

            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k200OK);
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(data.dump());
            callback(resp);
        } catch (const std::exception& e) {
            json err = {{"success", false}, {"error", e.what()}};
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k404NotFound);
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(err.dump());
            callback(resp);
        }
    }

private:
    static std::shared_ptr<Orchestrator> orchestrator_;
};

std::shared_ptr<Orchestrator> OrchestratorHandler::orchestrator_ = nullptr;

// --- Main ---
int main() {
    try {
        Config cfg = load_config();

        LOG_INFO << "starting bot orchestrator port=" << cfg.port 
                 << " command_topic=" << cfg.command_topic;

        std::string brokers_str;
        for (size_t i = 0; i < cfg.redpanda_brokers.size(); ++i) {
            brokers_str += cfg.redpanda_brokers[i];
            if (i < cfg.redpanda_brokers.size() - 1) brokers_str += ",";
        }

        cppkafka::Configuration kafka_cfg = {
            {"metadata.broker.list", brokers_str},
            {"client.id", "bot-orchestrator"},
            {"compression.codec", "snappy"},
            {"linger.ms", "5"}
        };
        auto producer = std::make_shared<cppkafka::Producer>(kafka_cfg);
        
        LOG_INFO << "connected to Redpanda brokers";

        auto orchestrator = std::make_shared<Orchestrator>(cfg, producer);
        OrchestratorHandler::init(orchestrator);

        drogon::app().addListener("0.0.0.0", cfg.port);
        drogon::app().run();
        
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to start bot-orchestrator: " << e.what();
        return 1;
    }
    return 0;
}

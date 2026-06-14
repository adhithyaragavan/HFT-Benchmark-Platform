#include <drogon/drogon.h>
#include <drogon/HttpClient.h>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <string>
#include <iostream>

using namespace drogon;

std::string envOrDefault(const char* key, const std::string& defaultVal) {
    const char* val = std::getenv(key);
    return val ? std::string(val) : defaultVal;
}

struct Config {
    std::string port;
    bool devMode;
    std::string apiKey;
    std::string submissionSvcUrl;
    std::string leaderboardSvcUrl;
    std::string scoringEngineUrl;
    std::string orchestratorSvcUrl;

    static Config load() {
        Config c;
        c.port = envOrDefault("GATEWAY_PORT", "8090");
        c.devMode = envOrDefault("DEV_MODE", "true") == "true";
        c.apiKey = envOrDefault("API_KEY", "");
        c.submissionSvcUrl = envOrDefault("SUBMISSION_SVC_URL", "http://localhost:8091");
        c.leaderboardSvcUrl = envOrDefault("LEADERBOARD_SVC_URL", "http://localhost:8093");
        c.scoringEngineUrl = envOrDefault("SCORING_ENGINE_URL", "http://localhost:8094");
        c.orchestratorSvcUrl = envOrDefault("ORCHESTRATOR_SVC_URL", "http://localhost:8092");
        return c;
    }

    void validate() const {
        if (!devMode && apiKey.empty()) {
            LOG_ERROR << "API_KEY is required when DEV_MODE is not enabled";
            exit(1);
        }
    }
};

bool constantTimeCompare(const std::string& a, const std::string& b) {
    if (a.length() != b.length()) return false;
    volatile char result = 0;
    for (size_t i = 0; i < a.length(); ++i) {
        result |= a[i] ^ b[i];
    }
    return result == 0;
}

int main() {
    Config cfg = Config::load();
    cfg.validate();

    LOG_INFO << "Starting gateway on port " << cfg.port << ", dev_mode: " << cfg.devMode;

    // Create Http Clients for backends
    auto subClient = HttpClient::newHttpClient(cfg.submissionSvcUrl);
    auto leadClient = HttpClient::newHttpClient(cfg.leaderboardSvcUrl);
    auto scoreClient = HttpClient::newHttpClient(cfg.scoringEngineUrl);
    auto orchClient = HttpClient::newHttpClient(cfg.orchestratorSvcUrl);

    // Pre-routing advice for CORS Preflight (OPTIONS), Authentication, and Request ID
    app().registerPreHandlingAdvice([devMode = cfg.devMode, expectedApiKey = cfg.apiKey](const HttpRequestPtr &req, AdviceCallback &&acb, AdviceChainCallback &&accb) {
        if (req->method() == Options) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k204NoContent);
            acb(resp);
            return;
        }

        if (!devMode) {
            std::string apiKey = req->getHeader("x-api-key");
            if (apiKey.empty()) {
                auto resp = HttpResponse::newHttpResponse();
                resp->setStatusCode(k401Unauthorized);
                resp->setContentTypeCode(CT_APPLICATION_JSON);
                resp->setBody(R"({"success":false,"error":"missing X-API-Key header"})");
                acb(resp);
                return;
            }
            if (!constantTimeCompare(apiKey, expectedApiKey)) {
                auto resp = HttpResponse::newHttpResponse();
                resp->setStatusCode(k403Forbidden);
                resp->setContentTypeCode(CT_APPLICATION_JSON);
                resp->setBody(R"({"success":false,"error":"invalid API key"})");
                acb(resp);
                return;
            }
        }

        // Request ID Injection
        std::string reqId = req->getHeader("x-request-id");
        if (reqId.empty()) {
            reqId = utils::getUuid();
            req->addHeader("x-request-id", reqId);
        }

        accb();
    });

    // Post-routing advice for CORS Headers and logging
    app().registerPostHandlingAdvice([devMode = cfg.devMode](const HttpRequestPtr &req, const HttpResponsePtr &resp) {
        if (devMode) {
            resp->addHeader("Access-Control-Allow-Origin", "*");
            resp->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS, PATCH");
            resp->addHeader("Access-Control-Allow-Headers", "Accept, Authorization, Content-Type, X-API-Key, X-Request-ID");
            resp->addHeader("Access-Control-Expose-Headers", "X-Request-ID");
            resp->addHeader("Access-Control-Max-Age", "86400");
        } else {
            std::string origin = req->getHeader("origin");
            if (!origin.empty()) {
                resp->addHeader("Access-Control-Allow-Origin", origin);
                resp->addHeader("Vary", "Origin");
            }
            resp->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            resp->addHeader("Access-Control-Allow-Headers", "Accept, Authorization, Content-Type, X-API-Key, X-Request-ID");
            resp->addHeader("Access-Control-Expose-Headers", "X-Request-ID");
            resp->addHeader("Access-Control-Max-Age", "3600");
        }

        std::string reqId = req->getHeader("x-request-id");
        if (!reqId.empty()) {
            resp->addHeader("X-Request-ID", reqId);
        }

        LOG_INFO << "Request completed " << req->methodString() << " " << req->path() 
                 << " status: " << resp->statusCode() 
                 << " request_id: " << reqId;
    });

    // Proxy Helper
    auto proxyHandler = [](const HttpRequestPtr &req, std::function<void(const HttpResponsePtr &)> &&callback, const HttpClientPtr &client, const std::string &backendName) {
        auto newReq = HttpRequest::newHttpRequest();
        newReq->setMethod(req->method());

        std::string targetPath = req->path();
        if (!req->query().empty()) {
            targetPath += "?" + req->query();
        }
        newReq->setPath(targetPath);
        newReq->setBody(std::string(req->getBody()));
        
        std::string ct = req->getHeader("content-type");
        if (!ct.empty()) {
            newReq->setContentTypeString(ct);
        } else {
            newReq->setContentTypeCode(req->contentType());
        }

        // Forward headers except those not meant to be proxied
        for (auto const& [key, value] : req->headers()) {
            if (key != "host" && key != "connection" && key != "upgrade" && key != "content-length" && key != "content-type") {
                newReq->addHeader(key, value);
            }
        }

        // Note: For WebSocket upgrade requests, a true WebSocket proxy would be required.
        // We log it here to map the Go behavior.
        if (req->getHeader("upgrade") == "websocket" || req->getHeader("connection").find("upgrade") != std::string::npos) {
            LOG_INFO << "websocket upgrade detected backend: " << backendName << " path: " << req->path();
        }

        client->sendRequest(newReq, [callback, backendName](ReqResult result, const HttpResponsePtr &resp) {
            if (result == ReqResult::Ok && resp) {
                resp->removeHeader("content-length");
                resp->removeHeader("transfer-encoding");
                callback(resp);
            } else {
                auto errResp = HttpResponse::newHttpResponse();
                errResp->setStatusCode(k502BadGateway);
                errResp->setContentTypeCode(CT_APPLICATION_JSON);
                nlohmann::json j = {{"success", false}, {"error", "service unavailable: " + backendName}};
                errResp->setBody(j.dump());
                callback(errResp);
            }
        });
    };

    // --- Health Check ---
    app().registerHandler("/api/v1/health", [](const HttpRequestPtr &, std::function<void(const HttpResponsePtr &)> &&callback) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setContentTypeCode(CT_APPLICATION_JSON);
        nlohmann::json j = {
            {"success", true},
            {"data", {
                {"status", "healthy"},
                {"service", "gateway"},
                {"version", "0.1.0"}
            }}
        };
        resp->setBody(j.dump());
        callback(resp);
    }, {Get});

    // --- Submission Service Routes ---
    app().registerHandler("/api/v1/submissions", [client = subClient, proxyHandler](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        proxyHandler(req, std::move(callback), client, "submission-svc");
    }, {Get, Post});

    app().registerHandler("/api/submit", [client = subClient, proxyHandler](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        proxyHandler(req, std::move(callback), client, "submission-svc");
    }, {Post});

    app().registerHandler("/api/submission/{id}/status", [client = subClient, proxyHandler](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string&) {
        proxyHandler(req, std::move(callback), client, "submission-svc");
    }, {Get});

    app().registerHandler("/api/v1/submissions/{id}", [client = subClient, proxyHandler](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string&) {
        proxyHandler(req, std::move(callback), client, "submission-svc");
    }, {Get});

    app().registerHandler("/api/v1/submissions/{id}/benchmark", [client = subClient, proxyHandler](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string&) {
        proxyHandler(req, std::move(callback), client, "submission-svc");
    }, {Post});

    app().registerHandler("/api/v1/submissions/{id}/start", [client = subClient, proxyHandler](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string&) {
        proxyHandler(req, std::move(callback), client, "submission-svc");
    }, {Post});

    app().registerHandler("/api/v1/submissions/{id}/stop", [client = subClient, proxyHandler](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string&) {
        proxyHandler(req, std::move(callback), client, "submission-svc");
    }, {Post});

    app().registerHandler("/api/v1/teams/{name}", [client = subClient, proxyHandler](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string&) {
        proxyHandler(req, std::move(callback), client, "submission-svc");
    }, {Delete});

    app().registerHandler("/api/v1/teams/{name}/start", [client = subClient, proxyHandler](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string&) {
        proxyHandler(req, std::move(callback), client, "submission-svc");
    }, {Post});

    app().registerHandler("/api/v1/teams/{name}/stop", [client = subClient, proxyHandler](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string&) {
        proxyHandler(req, std::move(callback), client, "submission-svc");
    }, {Post});

    // --- Leaderboard Service Routes ---
    app().registerHandler("/api/v1/leaderboard", [client = leadClient, proxyHandler](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        proxyHandler(req, std::move(callback), client, "leaderboard-svc");
    }, {Get});

    app().registerHandler("/api/v1/leaderboard/stream", [client = leadClient, proxyHandler](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        proxyHandler(req, std::move(callback), client, "leaderboard-svc");
    }, {Get});

    // --- Scoring Engine Routes ---
    app().registerHandler("/api/v1/benchmark/{id}/telemetry", [client = scoreClient, proxyHandler](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string&) {
        proxyHandler(req, std::move(callback), client, "scoring-engine");
    }, {Get});

    app().registerHandler("/api/v1/benchmark/{id}/telemetry/histogram", [client = scoreClient, proxyHandler](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string&) {
        proxyHandler(req, std::move(callback), client, "scoring-engine");
    }, {Get});

    app().registerHandler("/api/v1/benchmark/{id}/telemetry/finalize", [client = scoreClient, proxyHandler](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string&) {
        proxyHandler(req, std::move(callback), client, "scoring-engine");
    }, {Post});

    // --- Bot Orchestrator Routes ---
    app().registerHandler("/api/v1/orchestrator/start", [client = orchClient, proxyHandler](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        proxyHandler(req, std::move(callback), client, "bot-orchestrator");
    }, {Post});

    app().registerHandler("/api/v1/orchestrator/stop", [client = orchClient, proxyHandler](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        proxyHandler(req, std::move(callback), client, "bot-orchestrator");
    }, {Post});

    app().registerHandler("/api/v1/orchestrator/status/{id}", [client = orchClient, proxyHandler](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string&) {
        proxyHandler(req, std::move(callback), client, "bot-orchestrator");
    }, {Get});

    app().addListener("0.0.0.0", std::stoi(cfg.port));
    
    // Timeouts and threading can be set via app()
    app().setThreadNum(16);
    app().setClientMaxBodySize(1024 * 1024 * 600); // 600 MB
    app().setIdleConnectionTimeout(120);

    LOG_INFO << "Gateway listening";
    app().run();

    return 0;
}

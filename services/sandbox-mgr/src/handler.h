#pragma once
#include <drogon/drogon.h>
#include <nlohmann/json.hpp>
#include <thread>
#include "sandbox_service.h"
#include "models.h"

// Global service pointer, defined in main.cpp
extern std::shared_ptr<SandboxService> g_sandbox_svc;

// Register all sandbox handler routes as lambda handlers
inline void register_sandbox_handlers() {
    auto& app = drogon::app();

    app.registerHandler("/health",
        [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(R"({"status":"healthy","service":"sandbox-mgr","version":"0.1.0"})");
            callback(resp);
        }, {drogon::Get});

    app.registerHandler("/api/v1/sandbox/build",
        [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            try {
                auto j = nlohmann::json::parse(req->body());
                BuildAndDeployRequest build_req;
                from_json(j, build_req);

                if (build_req.submission_id.empty() || build_req.language.empty() || build_req.source_url.empty()) {
                    resp->setStatusCode(drogon::k400BadRequest);
                    nlohmann::json err = {{"error", "submission_id, language, and source_url are required"}};
                    resp->setBody(err.dump());
                    callback(resp);
                    return;
                }
                if (build_req.language != "go" && build_req.language != "cpp" && build_req.language != "rust") {
                    resp->setStatusCode(drogon::k400BadRequest);
                    nlohmann::json err = {{"error", "unsupported language: " + build_req.language}};
                    resp->setBody(err.dump());
                    callback(resp);
                    return;
                }

                std::string build_id = drogon::utils::getUuid();

                // Start build and deploy asynchronously using task queue
                g_sandbox_svc->enqueue_build(build_req);

                resp->setStatusCode(drogon::k202Accepted);
                nlohmann::json res_json = {
                    {"build_id", build_id},
                    {"submission_id", build_req.submission_id},
                    {"status", "pending_build"}
                };
                resp->setBody(res_json.dump());
            } catch (const std::exception& e) {
                resp->setStatusCode(drogon::k400BadRequest);
                nlohmann::json err = {{"error", std::string("invalid request body: ") + e.what()}};
                resp->setBody(err.dump());
            }
            callback(resp);
        }, {drogon::Post});

    app.registerHandler("/api/v1/sandbox/{submissionID}",
        [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& submissionID) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            if (submissionID.empty()) {
                resp->setStatusCode(drogon::k400BadRequest);
                resp->setBody(R"({"error":"submission_id required"})");
                callback(resp);
                return;
            }
            if (!g_sandbox_svc->teardown(submissionID)) {
                resp->setStatusCode(drogon::k500InternalServerError);
                nlohmann::json err = {{"error", "deployment not found"}};
                resp->setBody(err.dump());
                callback(resp);
                return;
            }
            nlohmann::json j = {{"status", "torn_down"}, {"submission_id", submissionID}};
            resp->setBody(j.dump());
            callback(resp);
        }, {drogon::Delete});

    app.registerHandler("/api/v1/sandbox/{submissionID}/status",
        [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& submissionID) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            auto st = g_sandbox_svc->get_status(submissionID);
            if (!st) {
                resp->setStatusCode(drogon::k404NotFound);
                nlohmann::json err = {{"error", "deployment not found: " + submissionID}};
                resp->setBody(err.dump());
            } else {
                nlohmann::json j;
                to_json(j, *st);
                resp->setBody(j.dump());
            }
            callback(resp);
        }, {drogon::Get});

    app.registerHandler("/api/v1/sandbox/{submissionID}/logs",
        [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& submissionID) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            auto st = g_sandbox_svc->get_status(submissionID);
            if (!st) {
                resp->setStatusCode(drogon::k404NotFound);
                nlohmann::json err = {{"error", "deployment not found: " + submissionID}};
                resp->setBody(err.dump());
            } else {
                nlohmann::json j = {{"submission_id", submissionID}, {"logs", st->build_logs}};
                resp->setBody(j.dump());
            }
            callback(resp);
        }, {drogon::Get});
}

#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>
#include <pqxx/pqxx>
#include <nlohmann/json.hpp>
#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/HeadBucketRequest.h>
#include <aws/s3/model/CreateBucketRequest.h>
#include <aws/core/auth/AWSCredentials.h>

#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sys/wait.h>
#include <sw/redis++/redis++.h>

using json = nlohmann::json;

struct Config {
    int port;
    std::string db_url;
    std::string minio_endpoint;
    std::string minio_access_key;
    std::string minio_secret_key;
    bool minio_use_ssl;
    std::string minio_bucket;
    int64_t max_upload_size;
    std::string redis_url;
    std::string redpanda_brokers;
};

auto getEnv = [](const char* key, const char* default_val) -> std::string {
    const char* val = std::getenv(key);
    return val ? val : default_val;
};

Config loadConfig() {
    Config c;
    c.port = std::stoi(getEnv("SUBMISSION_PORT", "8091"));
    c.db_url = getEnv("DATABASE_URL", "postgres://platform:platform@localhost:5432/benchmarks");
    c.minio_endpoint = getEnv("MINIO_ENDPOINT", "localhost:9000");
    c.minio_access_key = getEnv("MINIO_ACCESS_KEY", "minioadmin");
    c.minio_secret_key = getEnv("MINIO_SECRET_KEY", "minioadmin");
    c.minio_use_ssl = std::string(getEnv("MINIO_USE_SSL", "false")) == "true";
    c.minio_bucket = getEnv("MINIO_BUCKET", "submissions");
    c.max_upload_size = std::stoll(getEnv("MAX_UPLOAD_SIZE", "524288000")); // 500MB
    c.redis_url = getEnv("REDIS_URL", "tcp://localhost:6379");
    c.redpanda_brokers = getEnv("REDPANDA_BROKERS", "localhost:19092");
    return c;
}

class DbPool {
public:
    DbPool(const std::string& conn_str, size_t pool_size) : conn_str_(conn_str) {
        for (size_t i = 0; i < pool_size; ++i) {
            conns_.push_back(std::make_unique<pqxx::connection>(conn_str));
        }
    }

    class Connection {
    public:
        Connection(DbPool* pool, std::unique_ptr<pqxx::connection> conn) : pool_(pool), conn_(std::move(conn)) {}
        Connection(Connection&& other) noexcept : pool_(other.pool_), conn_(std::move(other.conn_)) {
            other.pool_ = nullptr;
        }
        ~Connection() { if (pool_ && conn_) pool_->release(std::move(conn_)); }
        pqxx::connection* get() { return conn_.get(); }
    private:
        DbPool* pool_;
        std::unique_ptr<pqxx::connection> conn_;
    };

    Connection acquire() {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this] { return !conns_.empty(); });
        auto conn = std::move(conns_.back());
        conns_.pop_back();
        try {
            if (!conn->is_open()) {
                conn = std::make_unique<pqxx::connection>(conn_str_);
            }
        } catch (...) {
            conn = std::make_unique<pqxx::connection>(conn_str_);
        }
        return Connection(this, std::move(conn));
    }

    void release(std::unique_ptr<pqxx::connection> conn) {
        std::unique_lock<std::mutex> lock(mtx_);
        conns_.push_back(std::move(conn));
        cv_.notify_one();
    }

private:
    std::string conn_str_;
    std::vector<std::unique_ptr<pqxx::connection>> conns_;
    std::mutex mtx_;
    std::condition_variable cv_;
};

void initDatabase(DbPool* dbPool) {
    auto conn = dbPool->acquire();
    pqxx::work w(*conn.get());
    w.exec(R"(
        CREATE TABLE IF NOT EXISTS submissions (
			id           TEXT PRIMARY KEY,
			team_id      TEXT NOT NULL,
			language     TEXT NOT NULL,
			status       TEXT NOT NULL DEFAULT 'pending',
			source_url   TEXT NOT NULL DEFAULT '',
			image_tag    TEXT NOT NULL DEFAULT '',
			endpoint_url TEXT NOT NULL DEFAULT '',
			namespace    TEXT NOT NULL DEFAULT '',
			error_msg    TEXT NOT NULL DEFAULT '',
			created_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
			updated_at   TIMESTAMPTZ NOT NULL DEFAULT NOW()
		);
		CREATE INDEX IF NOT EXISTS idx_submissions_team_id ON submissions(team_id);
		CREATE INDEX IF NOT EXISTS idx_submissions_status ON submissions(status);

		CREATE TABLE IF NOT EXISTS benchmark_runs (
			id               TEXT PRIMARY KEY,
			submission_id    TEXT NOT NULL REFERENCES submissions(id),
			status           TEXT NOT NULL DEFAULT 'pending',
			bot_count        INTEGER NOT NULL DEFAULT 10,
			duration_seconds INTEGER NOT NULL DEFAULT 60,
			profile          JSONB NOT NULL DEFAULT '{}',
			created_at       TIMESTAMPTZ NOT NULL DEFAULT NOW(),
			updated_at       TIMESTAMPTZ NOT NULL DEFAULT NOW()
		);
		CREATE INDEX IF NOT EXISTS idx_benchmark_runs_submission_id ON benchmark_runs(submission_id);
		CREATE INDEX IF NOT EXISTS idx_benchmark_runs_status ON benchmark_runs(status);
		CREATE INDEX IF NOT EXISTS idx_benchmark_runs_status ON benchmark_runs(status);
    )");
    
    // Add columns if not exist
    w.exec("ALTER TABLE submissions ADD COLUMN IF NOT EXISTS email TEXT DEFAULT ''");
    w.exec("ALTER TABLE submissions ADD COLUMN IF NOT EXISTS file_hash TEXT DEFAULT ''");
    w.exec("ALTER TABLE submissions ADD COLUMN IF NOT EXISTS file_size BIGINT DEFAULT 0");

    w.commit();
}

class ObjectStorage {
public:
    ObjectStorage(const Config& cfg) : bucket_(cfg.minio_bucket) {
        Aws::Client::ClientConfiguration clientConfig;
        clientConfig.endpointOverride = cfg.minio_endpoint;
        if (clientConfig.endpointOverride.find("http://") != 0 && clientConfig.endpointOverride.find("https://") != 0) {
            clientConfig.endpointOverride = (cfg.minio_use_ssl ? "https://" : "http://") + clientConfig.endpointOverride;
        }
        clientConfig.scheme = cfg.minio_use_ssl ? Aws::Http::Scheme::HTTPS : Aws::Http::Scheme::HTTP;
        Aws::Auth::AWSCredentials credentials(cfg.minio_access_key, cfg.minio_secret_key);
        
        Aws::Client::ClientConfiguration s3Cfg(clientConfig);
        s3Cfg.region = "us-east-1";
        s3_client_ = std::make_shared<Aws::S3::S3Client>(credentials, s3Cfg, Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never, false);

        ensureBucket();
    }

    void ensureBucket() {
        Aws::S3::Model::HeadBucketRequest head_req;
        head_req.SetBucket(bucket_);
        auto head_outcome = s3_client_->HeadBucket(head_req);
        if (!head_outcome.IsSuccess()) {
            Aws::S3::Model::CreateBucketRequest create_req;
            create_req.SetBucket(bucket_);
            auto create_outcome = s3_client_->CreateBucket(create_req);
            if (!create_outcome.IsSuccess()) {
                throw std::runtime_error("Failed to create bucket: " + create_outcome.GetError().GetMessage());
            }
        }
    }

    std::string UploadSubmission(const std::string& submissionID, const std::string& fileContent, const std::string& contentType) {
        std::string objectKey = submissionID + "/source.tar.gz";
        Aws::S3::Model::PutObjectRequest put_req;
        put_req.SetBucket(bucket_);
        put_req.SetKey(objectKey);
        put_req.SetContentType(contentType);

        auto input_data = Aws::MakeShared<Aws::StringStream>("PutObjectInputStream");
        *input_data << fileContent;
        put_req.SetBody(input_data);

        auto outcome = s3_client_->PutObject(put_req);
        if (!outcome.IsSuccess()) {
            throw std::runtime_error("Upload failed: " + outcome.GetError().GetMessage());
        }
        return objectKey;
    }

    std::string GetSubmissionURL(const std::string& submissionID) {
        return "http://localhost:9000/" + bucket_ + "/" + submissionID + "/source.tar.gz";
    }

    void DeleteSubmission(const std::string& submissionID) {
        std::string objectKey = submissionID + "/source.tar.gz";
        Aws::S3::Model::DeleteObjectRequest del_req;
        del_req.SetBucket(bucket_);
        del_req.SetKey(objectKey);
        s3_client_->DeleteObject(del_req);
    }

private:
    std::shared_ptr<Aws::S3::S3Client> s3_client_;
    std::string bucket_;
};

struct Submission {
    std::string id;
    std::string team_id;
    std::string language;
    std::string status;
    std::string email;
    std::string file_hash;
    int64_t file_size;
    std::string source_url;
    std::string image_tag;
    std::string endpoint_url;
    std::string namespace_;
    std::string error_msg;
    std::string created_at;
    std::string updated_at;
};

void to_json(json& j, const Submission& s) {
    j = json{
        {"id", s.id},
        {"team_id", s.team_id},
        {"language", s.language},
        {"status", s.status},
        {"email", s.email},
        {"file_hash", s.file_hash},
        {"file_size", s.file_size},
        {"created_at", s.created_at},
        {"updated_at", s.updated_at}
    };
    if (!s.source_url.empty()) j["source_url"] = s.source_url;
    if (!s.image_tag.empty()) j["image_tag"] = s.image_tag;
    if (!s.endpoint_url.empty()) j["endpoint_url"] = s.endpoint_url;
    if (!s.namespace_.empty()) j["namespace"] = s.namespace_;
    if (!s.error_msg.empty()) j["error_msg"] = s.error_msg;
}

template<typename RowType>
Submission rowToSubmission(const RowType& row) {
    Submission s;
    s.id = row["id"].c_str();
    s.team_id = row["team_id"].c_str();
    s.language = row["language"].c_str();
    s.status = row["status"].c_str();
    s.email = row["email"].c_str();
    s.file_hash = row["file_hash"].c_str();
    s.file_size = std::stoll(row["file_size"].c_str() ? row["file_size"].c_str() : "0");
    s.source_url = row["source_url"].c_str();
    s.image_tag = row["image_tag"].c_str();
    s.endpoint_url = row["endpoint_url"].c_str();
    s.namespace_ = row["namespace"].c_str();
    s.error_msg = row["error_msg"].c_str();
    s.created_at = row["created_at"].c_str();
    s.updated_at = row["updated_at"].c_str();
    return s;
}

void sendJson(std::function<void(const drogon::HttpResponsePtr&)> callback, int status, const json& data) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    resp->setBody(data.dump());
    callback(resp);
}

void sendError(std::function<void(const drogon::HttpResponsePtr&)> callback, int status, const std::string& errorMsg) {
    json j = {{"success", false}, {"error", errorMsg}};
    sendJson(callback, status, j);
}

void sendSuccess(std::function<void(const drogon::HttpResponsePtr&)> callback, int status, const json& data) {
    json j = {{"success", true}, {"data", data}};
    sendJson(callback, status, j);
}

std::string resolveTeamUuid(pqxx::transaction_base& w, const std::string& input, bool create_if_not_found = false) {
    if (input.empty()) return "";
    
    std::regex uuid_regex("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
    
    // 1. Try to find by UUID directly if the input looks like a UUID
    if (std::regex_match(input, uuid_regex)) {
        pqxx::result res = w.exec_params(
            "SELECT id FROM teams WHERE id = $1",
            input
        );
        if (!res.empty()) {
            return res[0][0].as<std::string>();
        }
    }
    
    // 2. Try to find by name or api_key
    pqxx::result res = w.exec_params(
        "SELECT id FROM teams WHERE name = $1 OR api_key = $1",
        input
    );
    if (!res.empty()) {
        return res[0][0].as<std::string>();
    }
    
    // 3. Create if not found and requested
    if (create_if_not_found) {
        std::regex name_regex("^[a-zA-Z0-9-]+$");
        if (input.length() >= 3 && input.length() <= 32 && std::regex_match(input, name_regex)) {
            std::string generated_api_key = "api-key-" + drogon::utils::getUuid();
            pqxx::result ins_res = w.exec_params(
                "INSERT INTO teams (name, api_key, members) VALUES ($1, $2, '[]') RETURNING id",
                input, generated_api_key
            );
            if (!ins_res.empty()) {
                return ins_res[0][0].as<std::string>();
            }
        }
    }
    
    return "";
}

// ============================================================================
// Verification helper functions for Magic Bytes, Zip Bomb, and ELF static linking
// ============================================================================
bool verify_magic_bytes(const char* data, size_t length, const std::string& ext, std::string& error_msg) {
    if (length < 4) {
        error_msg = "File payload too small to identify format";
        return false;
    }

    unsigned char b0 = static_cast<unsigned char>(data[0]);
    unsigned char b1 = static_cast<unsigned char>(data[1]);
    unsigned char b2 = static_cast<unsigned char>(data[2]);
    unsigned char b3 = static_cast<unsigned char>(data[3]);

    bool is_elf = (b0 == 0x7F && b1 == 0x45 && b2 == 0x4C && b3 == 0x46);
    bool is_zip = (b0 == 0x50 && b1 == 0x4B && b2 == 0x03 && b3 == 0x04);
    bool is_gzip = (b0 == 0x1F && b1 == 0x8B);

    if (b0 == 0x4D && b1 == 0x5A) {
        error_msg = "Validation Error: Windows PE executables (.exe) are not allowed";
        return false;
    }

    if ((b0 == 0xFE && b1 == 0xED && b2 == 0xFA && b3 == 0xCE) ||
        (b0 == 0xCE && b1 == 0xFA && b2 == 0xED && b3 == 0xFE) ||
        (b0 == 0xFE && b1 == 0xED && b2 == 0xFA && b3 == 0xCF) ||
        (b0 == 0xCF && b1 == 0xFA && b2 == 0xED && b3 == 0xFE)) {
        error_msg = "Validation Error: macOS Mach-O executables are not allowed";
        return false;
    }

    if (ext == ".elf" || ext == "") {
        if (!is_elf) {
            error_msg = "400 Bad Request: Invalid Binary Format. Expected ELF header (0x7F454C46)";
            return false;
        }
    } else {
        if (!is_zip && !is_gzip) {
            error_msg = "400 Bad Request: Invalid Archive Format. Expected Gzip (0x1F8B) or ZIP (0x504B0304) header";
            return false;
        }
    }
    return true;
}

bool scan_zip_for_bombs(const char* data, size_t length, std::string& error_msg) {
    const uint64_t MAX_UNCOMPRESSED_SIZE = 100 * 1024 * 1024; // 100 MB
    const double MAX_COMPRESSION_RATIO = 5.0;

    if (length < 2) return true;
    unsigned char b0 = static_cast<unsigned char>(data[0]);
    unsigned char b1 = static_cast<unsigned char>(data[1]);

    if (b0 == 0x1F && b1 == 0x8B) {
        if (length < 8) return true;
        uint32_t uncompressed_size = 0;
        std::memcpy(&uncompressed_size, data + length - 4, 4);
        if (uncompressed_size > MAX_UNCOMPRESSED_SIZE) {
            error_msg = "DoS Protection: Gzip uncompressed size exceeds 100MB limit";
            return false;
        }
        return true;
    }

    size_t offset = 0;
    uint64_t total_uncompressed = 0;
    uint64_t total_compressed = 0;

    while (offset + 30 <= length) {
        if (std::memcmp(data + offset, "\x50\x4B\x03\x04", 4) != 0) {
            break;
        }

        uint32_t compressed_size = 0;
        uint32_t uncompressed_size = 0;
        uint16_t filename_len = 0;
        uint16_t extra_len = 0;

        std::memcpy(&compressed_size, data + offset + 18, 4);
        std::memcpy(&uncompressed_size, data + offset + 22, 4);
        std::memcpy(&filename_len, data + offset + 26, 2);
        std::memcpy(&extra_len, data + offset + 28, 2);

        total_uncompressed += uncompressed_size;
        total_compressed += compressed_size;

        if (total_uncompressed > MAX_UNCOMPRESSED_SIZE) {
            error_msg = "Zip bomb protection: Uncompressed size exceeds 100MB limit";
            return false;
        }

        offset += 30 + filename_len + extra_len + compressed_size;
    }

    if (total_compressed > 0) {
        double ratio = static_cast<double>(total_uncompressed) / total_compressed;
        if (ratio > MAX_COMPRESSION_RATIO && total_uncompressed > 1024 * 1024) {
            error_msg = "Zip bomb protection: Compression ratio exceeds 5x limit (" + std::to_string(ratio) + "x)";
            return false;
        }
    }

    return true;
}

bool is_elf_statically_linked(const char* data, size_t length, std::string& error_msg) {
    if (length < 64) {
        error_msg = "Invalid ELF file size";
        return false;
    }

    if (std::memcmp(data, "\x7F\x45\x4C\x46", 4) != 0) {
        error_msg = "Not an ELF binary";
        return false;
    }

    unsigned char elf_class = data[4];
    
    if (elf_class == 2) { // ELF64
        uint64_t phoff = 0;
        std::memcpy(&phoff, data + 32, 8);
        uint16_t phentsize = 0;
        std::memcpy(&phentsize, data + 54, 2);
        uint16_t phnum = 0;
        std::memcpy(&phnum, data + 56, 2);

        if (phoff + (phnum * phentsize) > length) {
            error_msg = "Corrupted ELF program header table layout";
            return false;
        }

        for (int i = 0; i < phnum; ++i) {
            size_t entry_offset = phoff + (i * phentsize);
            uint32_t p_type = 0;
            std::memcpy(&p_type, data + entry_offset, 4);
            if (p_type == 3) {
                error_msg = "Security constraint: Dynamically linked ELF rejected (contains PT_INTERP dynamic linker path)";
                return false;
            }
        }
    } else if (elf_class == 1) { // ELF32
        uint32_t phoff = 0;
        std::memcpy(&phoff, data + 28, 4);
        uint16_t phentsize = 0;
        std::memcpy(&phentsize, data + 42, 2);
        uint16_t phnum = 0;
        std::memcpy(&phnum, data + 44, 2);

        if (phoff + (phnum * phentsize) > length) {
            error_msg = "Corrupted ELF program header table layout";
            return false;
        }

        for (int i = 0; i < phnum; ++i) {
            size_t entry_offset = phoff + (i * phentsize);
            uint32_t p_type = 0;
            std::memcpy(&p_type, data + entry_offset, 4);
            if (p_type == 3) {
                error_msg = "Security constraint: Dynamically linked ELF rejected (contains PT_INTERP dynamic linker path)";
                return false;
            }
        }
    } else {
        error_msg = "Unsupported ELF class";
        return false;
    }

    return true;
}

int main() {
    Aws::SDKOptions options;
    Aws::InitAPI(options);

    {
        Config cfg = loadConfig();
        
        auto dbPool = std::make_shared<DbPool>(cfg.db_url, 5);
        initDatabase(dbPool.get());

        auto storage = std::make_shared<ObjectStorage>(cfg);

        auto redis = std::make_shared<sw::redis::Redis>(cfg.redis_url);

        drogon::app().registerHandler("/health",
            [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k200OK);
                resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                resp->setBody("{\"success\":true,\"data\":{\"status\":\"healthy\",\"service\":\"submission-svc\",\"version\":\"0.1.0\"}}");
                callback(resp);
            }, {drogon::Get});
        auto cfg_ptr = std::make_shared<Config>(cfg);

        drogon::app().registerHandler("/api/submit", 
            [cfg_ptr, dbPool, storage, redis](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                drogon::MultiPartParser parser;
                if (parser.parse(req) != 0) {
                    sendError(callback, 400, "failed to parse multipart form");
                    return;
                }

                auto params = parser.getParameters();
                std::string team_id = "";
                if (params.count("team_id")) {
                    team_id = params["team_id"];
                } else if (params.count("team_name")) {
                    team_id = params["team_name"];
                }
                if (team_id.empty()) {
                    sendError(callback, 400, "team_id or team_name is required");
                    return;
                }
                
                std::regex team_regex("^[a-zA-Z0-9-]+$");
                if (team_id.length() < 3 || team_id.length() > 32 || !std::regex_match(team_id, team_regex)) {
                    sendError(callback, 400, "validation error on field \"team_name\": 3-32 chars, alphanumeric and hyphens only");
                    return;
                }

                if (params.find("email") == params.end()) {
                    sendError(callback, 400, "email is required");
                    return;
                }
                std::string email = params["email"];
                std::regex email_regex(R"(^[a-zA-Z0-9_.+-]+@[a-zA-Z0-9-]+\.[a-zA-Z0-9-.]+$)");
                if (!std::regex_match(email, email_regex)) {
                    sendError(callback, 400, "validation error on field \"email\": invalid email format");
                    return;
                }

                if (params.find("language") == params.end()) {
                    sendError(callback, 400, "language is required");
                    return;
                }
                std::string language = params["language"];

                auto files = parser.getFiles();
                const drogon::HttpFile* target_file = nullptr;
                for (const auto& f : files) {
                    if (f.getItemName() == "file") {
                        target_file = &f;
                        break;
                    }
                }
                if (!target_file) {
                    sendError(callback, 400, "file is required");
                    return;
                }

                if (language != "cpp" && language != "rust" && language != "go") {
                    sendError(callback, 400, "validation error on field \"language\": unsupported language");
                    return;
                }

                std::string filename = target_file->getFileName();
                std::string ext = filename;
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext.length() >= 7 && ext.substr(ext.length() - 7) == ".tar.gz") {
                    ext = ".tar.gz";
                } else {
                    size_t pos = ext.find_last_of('.');
                    if (pos != std::string::npos) ext = ext.substr(pos);
                    else ext = "";
                }
                
                if (ext != ".tar.gz" && ext != ".tgz" && ext != ".zip" && ext != ".elf" && ext != "") {
                    // Note: elf binaries often have no extension
                    if (ext != ".zip" && ext != ".tar.gz" && ext != ".tgz") {
                        sendError(callback, 400, "validation error on field \"file\": invalid file extension. Must be a zip archive or elf binary");
                        return;
                    }
                }

                if ((int64_t)target_file->fileLength() > cfg_ptr->max_upload_size) {
                    sendError(callback, 400, "validation error on field \"file\": file size exceeds maximum 500MB");
                    return;
                }

                // Run Magic Byte Verification
                std::string validation_error;
                if (!verify_magic_bytes(target_file->fileData(), target_file->fileLength(), ext, validation_error)) {
                    LOG_WARN << "Upload Security Violation: " << validation_error;
                    sendError(callback, 400, validation_error);
                    return;
                }

                // Run Zip Bomb / DoS Check for archives
                if (ext != ".elf" && ext != "") {
                    if (!scan_zip_for_bombs(target_file->fileData(), target_file->fileLength(), validation_error)) {
                        LOG_WARN << "Upload DoS Violation: " << validation_error;
                        sendError(callback, 400, validation_error);
                        return;
                    }
                }

                // Run Static Link Check for ELF binaries
                if (ext == ".elf" || ext == "") {
                    if (!is_elf_statically_linked(target_file->fileData(), target_file->fileLength(), validation_error)) {
                        LOG_WARN << "Upload Static Linking Violation: " << validation_error;
                        sendError(callback, 400, validation_error);
                        return;
                    }
                }

                std::string submissionID = drogon::utils::getUuid();
                
                // ClamAV scan
                bool disable_clamav = (std::getenv("DISABLE_CLAMAV") && std::string(std::getenv("DISABLE_CLAMAV")) == "true");
                int scan_res = 0;
                bool cmd_not_found = false;

                if (!disable_clamav) {
                    std::string tmp_path = "/tmp/sub_" + submissionID;
                    std::ofstream ofs(tmp_path, std::ios::binary);
                    ofs.write(target_file->fileData(), target_file->fileLength());
                    ofs.close();

                    std::string cmd = "clamdscan --no-summary --fdpass " + tmp_path + " > /dev/null 2>&1";
                    scan_res = std::system(cmd.c_str());
                    if (scan_res != 0) {
                        // Try fallback clamscan
                        cmd = "clamscan --no-summary " + tmp_path + " > /dev/null 2>&1";
                        scan_res = std::system(cmd.c_str());
                    }

                    // On POSIX systems, std::system returns the termination status as returned by wait().
                    // Exit code 127 indicates the command was not found.
                    // (127 in wait status is typically 127 << 8 = 32512)
                    if (scan_res != 0) {
                        int exit_status = -1;
#ifdef WIFEXITED
                        if (WIFEXITED(scan_res)) {
                            exit_status = WEXITSTATUS(scan_res);
                        }
#endif
                        if (scan_res == 127 || scan_res == 32512 || exit_status == 127) {
                            cmd_not_found = true;
                            LOG_WARN << "ClamAV scanner command not found (exit code: " << scan_res << "). Skipping malware scan.";
                        }
                    }

                    if (scan_res != 0 && !cmd_not_found) {
                        std::remove(tmp_path.c_str());
                        sendError(callback, 400, "Submission rejected: Malware detected by antivirus scanner");
                        return;
                    }
                    std::remove(tmp_path.c_str());
                }

                std::string file_hash = drogon::utils::getMd5(target_file->fileData(), target_file->fileLength());
                int64_t file_size = target_file->fileLength();
                
                try {
                    std::string source_url;
                    try {
                        storage->UploadSubmission(submissionID, std::string(target_file->fileData(), target_file->fileLength()), "application/gzip");
                        source_url = storage->GetSubmissionURL(submissionID);
                    } catch (const std::exception& e) {
                        sendError(callback, 500, std::string("uploading submission archive: ") + e.what());
                        return;
                    }

                    auto conn = dbPool->acquire();
                    pqxx::work w(*conn.get());
                    std::string resolved_team_uuid = resolveTeamUuid(w, team_id, true);
                    if (resolved_team_uuid.empty()) {
                        sendError(callback, 400, "Team not found and could not be created.");
                        return;
                    }
                    pqxx::result res = w.exec_params(
                        "INSERT INTO submissions (id, team_id, language, status, email, file_hash, file_size, source_url, created_at, updated_at) "
                        "VALUES ($1, $2, $3, 'pending', $4, $5, $6, $7, NOW(), NOW()) "
                        "RETURNING id, team_id, language, status, email, file_hash, file_size, source_url, image_tag, endpoint_url, namespace, error_msg, created_at::text, updated_at::text",
                        submissionID, resolved_team_uuid, language, email, file_hash, file_size, source_url
                    );
                    w.commit();

                    // Trigger sandbox-mgr via HTTP instead of Kafka/publish_bin
                    auto client = drogon::HttpClient::newHttpClient("http://localhost:8095");
                    nlohmann::json build_req = {
                        {"submission_id", submissionID},
                        {"language", language},
                        {"source_url", source_url}
                    };
                    auto drogon_req = drogon::HttpRequest::newHttpRequest();
                    drogon_req->setPath("/api/v1/sandbox/build");
                    drogon_req->setMethod(drogon::Post);
                    drogon_req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                    drogon_req->setBody(build_req.dump());
                    client->sendRequest(drogon_req, [](drogon::ReqResult res, const drogon::HttpResponsePtr& resp) {
                        if (res != drogon::ReqResult::Ok || !resp || resp->getStatusCode() >= 400) {
                            LOG_ERROR << "Failed to trigger build in sandbox-mgr";
                        }
                    });
                    
                    // Add to redis
                    try {
                        redis->sadd("active_submissions", submissionID);
                    } catch(...) {}

                    Submission sub = rowToSubmission(res[0]);
                    json out = {
                        {"submission_id", submissionID},
                        {"status", "pending"}
                    };
                    sendSuccess(callback, 200, out);
                } catch (const std::exception& e) {
                    storage->DeleteSubmission(submissionID);
                    sendError(callback, 500, std::string("internal server error: ") + e.what());
                }
        }, {drogon::Post});
        
        drogon::app().registerHandler("/api/submission/{id}/status",
            [dbPool](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& id) {
                std::string current_status;
                try {
                    auto conn = dbPool->acquire();
                    pqxx::nontransaction n(*conn.get());
                    pqxx::result db_res = n.exec_params("SELECT status FROM submissions WHERE id = $1", id);
                    if (db_res.empty()) {
                        sendError(callback, 404, "submission not found");
                        return;
                    }
                    current_status = db_res[0]["status"].c_str();
                } catch (const std::exception& e) {
                    sendError(callback, 500, e.what());
                    return;
                }

                if (current_status == "testing") {
                    auto client = drogon::HttpClient::newHttpClient("http://localhost:8092");
                    auto drogon_req = drogon::HttpRequest::newHttpRequest();
                    drogon_req->setPath("/api/v1/orchestrator/status/" + id);
                    drogon_req->setMethod(drogon::Get);
                    client->sendRequest(drogon_req, [dbPool, callback = std::move(callback), id](drogon::ReqResult res, const drogon::HttpResponsePtr& resp) {
                        std::string new_status = "";
                        if (res == drogon::ReqResult::Ok && resp && resp->getStatusCode() == 200) {
                            try {
                                auto json_obj = nlohmann::json::parse(resp->getBody());
                                if (json_obj.contains("data") && json_obj["data"].contains("status")) {
                                    std::string s = json_obj["data"]["status"].get<std::string>();
                                    if (s == "completed") new_status = "complete";
                                    else if (s == "failed") new_status = "failed";
                                }
                            } catch (...) {}
                        }
                        
                        try {
                            auto conn = dbPool->acquire();
                            if (!new_status.empty()) {
                                pqxx::work w(*conn.get());
                                w.exec_params("UPDATE submissions SET status = $1, updated_at = NOW() WHERE id = $2", new_status, id);
                                w.commit();

                                if (new_status == "complete") {
                                    pqxx::nontransaction n_t(*conn.get());
                                    pqxx::result tr = n_t.exec_params("SELECT t.name FROM teams t JOIN submissions s ON t.id = s.team_id WHERE s.id = $1", id);
                                    std::string team_name = tr.empty() ? id : tr[0][0].c_str();
                                    
                                    auto sc_client = drogon::HttpClient::newHttpClient("http://localhost:8094");
                                    auto sc_req = drogon::HttpRequest::newHttpRequest();
                                    sc_req->setPath("/api/v1/benchmark/" + id + "/telemetry/finalize");
                                    sc_req->setMethod(drogon::Post);
                                    sc_req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                                    nlohmann::json f_payload = {{"team_id", team_name}};
                                    sc_req->setBody(f_payload.dump());
                                    sc_client->sendRequest(sc_req, [](drogon::ReqResult res, const drogon::HttpResponsePtr& resp) {});
                                }
                            }
                            pqxx::nontransaction n(*conn.get());
                            pqxx::result db_res = n.exec_params("SELECT status, error_msg FROM submissions WHERE id = $1", id);
                            nlohmann::json out = {{"status", db_res[0]["status"].c_str()}, {"error", db_res[0]["error_msg"].c_str()}};
                            if (out["error"].get<std::string>().empty()) out["error"] = nullptr;
                            sendSuccess(callback, 200, out);
                        } catch (const std::exception& e) {
                            sendError(callback, 500, e.what());
                        }
                    });
                    return;
                }

                if (current_status != "complete" && current_status != "failed") {
                    auto client = drogon::HttpClient::newHttpClient("http://localhost:8095");
                    auto drogon_req = drogon::HttpRequest::newHttpRequest();
                    drogon_req->setPath("/api/v1/sandbox/" + id + "/status");
                    drogon_req->setMethod(drogon::Get);
                    client->sendRequest(drogon_req, [dbPool, callback = std::move(callback), id](drogon::ReqResult res, const drogon::HttpResponsePtr& resp) {
                        std::string new_status = "";
                        std::string new_error = "";
                        std::string endpoint_url = "";
                        if (res == drogon::ReqResult::Ok && resp && resp->getStatusCode() == 200) {
                            try {
                                auto json_obj = nlohmann::json::parse(resp->getBody());
                                if (json_obj.contains("status")) new_status = json_obj["status"].get<std::string>();
                                if (json_obj.contains("error_msg")) new_error = json_obj["error_msg"].get<std::string>();
                                if (json_obj.contains("endpoint_url")) endpoint_url = json_obj["endpoint_url"].get<std::string>();
                            } catch (...) {}
                        }

                        if (new_status == "ready" && !endpoint_url.empty()) {
                            // Automatically starting the benchmark is disabled to allow interactive start.
                            // We just let new_status remain "ready".
                        }

                        try {
                            auto conn = dbPool->acquire();
                            if (!new_status.empty()) {
                                pqxx::work w(*conn.get());
                                w.exec_params("UPDATE submissions SET status = $1, error_msg = $2, updated_at = NOW() WHERE id = $3", new_status, new_error, id);
                                w.commit();
                            }
                            pqxx::nontransaction n(*conn.get());
                            pqxx::result db_res = n.exec_params("SELECT status, error_msg FROM submissions WHERE id = $1", id);
                            nlohmann::json out = {{"status", db_res[0]["status"].c_str()}, {"error", db_res[0]["error_msg"].c_str()}};
                            if (out["error"].get<std::string>().empty()) out["error"] = nullptr;
                            sendSuccess(callback, 200, out);
                        } catch (const std::exception& e) {
                            sendError(callback, 500, e.what());
                        }
                    });
                    return;
                }

                try {
                    auto conn = dbPool->acquire();
                    pqxx::nontransaction n(*conn.get());
                    pqxx::result db_res = n.exec_params("SELECT status, error_msg FROM submissions WHERE id = $1", id);
                    nlohmann::json out = {{"status", db_res[0]["status"].c_str()}, {"error", db_res[0]["error_msg"].c_str()}};
                    if (out["error"].get<std::string>().empty()) out["error"] = nullptr;
                    sendSuccess(callback, 200, out);
                } catch (const std::exception& e) {
                    sendError(callback, 500, e.what());
                }
        }, {drogon::Get});

        drogon::app().registerHandler("/api/v1/submissions",
            [dbPool](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                std::string team_id = req->getParameter("team_id");
                try {
                    auto conn = dbPool->acquire();
                    pqxx::result res;
                    if (!team_id.empty()) {
                        pqxx::work w(*conn.get());
                        std::string resolved_team_uuid = resolveTeamUuid(w, team_id, false);
                        if (resolved_team_uuid.empty()) {
                            sendSuccess(callback, 200, json::array());
                            return;
                        }
                        res = w.exec_params(
                            "SELECT id, team_id, language, status, source_url, image_tag, endpoint_url, namespace, error_msg, created_at::text, updated_at::text "
                            "FROM submissions WHERE team_id = $1 ORDER BY created_at DESC LIMIT 100",
                            resolved_team_uuid
                        );
                        w.commit();
                    } else {
                        pqxx::nontransaction w(*conn.get());
                        res = w.exec(
                            "SELECT id, team_id, language, status, source_url, image_tag, endpoint_url, namespace, error_msg, created_at::text, updated_at::text "
                            "FROM submissions ORDER BY created_at DESC LIMIT 100"
                        );
                    }

                    std::vector<Submission> subs;
                    for (const auto& row : res) {
                        subs.push_back(rowToSubmission(row));
                    }
                    sendSuccess(callback, 200, subs);
                } catch (const std::exception& e) {
                    sendError(callback, 500, e.what());
                }
        }, {drogon::Get});

        drogon::app().registerHandler("/api/v1/submissions/{id}",
            [dbPool](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& id) {
                try {
                    auto conn = dbPool->acquire();
                    pqxx::nontransaction w(*conn.get());
                    pqxx::result res = w.exec_params(
                        "SELECT id, team_id, language, status, source_url, image_tag, endpoint_url, namespace, error_msg, created_at::text, updated_at::text "
                        "FROM submissions WHERE id = $1",
                        id
                    );

                    if (res.empty()) {
                        sendError(callback, 404, "submission not found");
                        return;
                    }

                    sendSuccess(callback, 200, rowToSubmission(res[0]));
                } catch (const std::exception& e) {
                    sendError(callback, 500, e.what());
                }
        }, {drogon::Get});

        drogon::app().registerHandler("/api/v1/submissions/{id}/benchmark",
            [dbPool](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& id) {
                try {
                    auto req_json = req->getJsonObject();
                    if (!req_json) {
                        sendError(callback, 400, "invalid JSON body");
                        return;
                    }

                    int bot_count = 10;
                    if (req_json->isMember("bot_count") && (*req_json)["bot_count"].isInt()) {
                        bot_count = (*req_json)["bot_count"].asInt();
                    }
                    int duration_seconds = 60;
                    if (req_json->isMember("duration_seconds") && (*req_json)["duration_seconds"].isInt()) {
                        duration_seconds = (*req_json)["duration_seconds"].asInt();
                    }
                    std::string profile_str = "{}";
                    if (req_json->isMember("profile") && (*req_json)["profile"].isObject()) {
                        Json::FastWriter writer;
                        profile_str = writer.write((*req_json)["profile"]);
                    }

                    if (bot_count < 1 || bot_count > 1000) {
                        sendError(callback, 400, "validation error on field \"bot_count\"");
                        return;
                    }
                    if (duration_seconds < 1 || duration_seconds > 3600) {
                        sendError(callback, 400, "validation error on field \"duration_seconds\"");
                        return;
                    }

                    auto conn = dbPool->acquire();
                    pqxx::work w(*conn.get());

                    pqxx::result sub_res = w.exec_params("SELECT id FROM submissions WHERE id = $1", id);
                    if (sub_res.empty()) {
                        sendError(callback, 404, "submission not found");
                        return;
                    }

                    std::string run_id = drogon::utils::getUuid();
                    pqxx::result run_res = w.exec_params(
                        "INSERT INTO benchmark_runs (id, submission_id, status, bot_count, duration_seconds, profile, created_at, updated_at) "
                        "VALUES ($1, $2, 'pending', $3, $4, $5, NOW(), NOW()) "
                        "RETURNING id, submission_id, status",
                        run_id, id, bot_count, duration_seconds, profile_str
                    );
                    w.commit();

                    json j = {
                        {"run_id", run_res[0]["id"].c_str()},
                        {"submission_id", run_res[0]["submission_id"].c_str()},
                        {"status", run_res[0]["status"].c_str()}
                    };
                    sendSuccess(callback, 201, j);
                } catch (const std::exception& e) {
                    sendError(callback, 500, e.what());
                }
        }, {drogon::Post});

        drogon::app().registerHandler("/api/v1/submissions/{id}/start",
            [dbPool](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& id) {
                auto client = drogon::HttpClient::newHttpClient("http://localhost:8095");
                auto drogon_req = drogon::HttpRequest::newHttpRequest();
                drogon_req->setPath("/api/v1/sandbox/" + id + "/status");
                drogon_req->setMethod(drogon::Get);
                client->sendRequest(drogon_req, [dbPool, callback, id](drogon::ReqResult res, const drogon::HttpResponsePtr& resp) {
                    std::string new_status = "";
                    std::string endpoint_url = "";
                    if (res == drogon::ReqResult::Ok && resp && resp->getStatusCode() == 200) {
                        try {
                            auto json_obj = nlohmann::json::parse(resp->getBody());
                            if (json_obj.contains("status")) new_status = json_obj["status"].get<std::string>();
                            if (json_obj.contains("endpoint_url")) endpoint_url = json_obj["endpoint_url"].get<std::string>();
                        } catch (...) {}
                    }
                    if (new_status != "ready" || endpoint_url.empty()) {
                        sendError(callback, 400, "sandbox is not ready or endpoint_url missing");
                        return;
                    }
                    
                    auto orch_client = drogon::HttpClient::newHttpClient("http://localhost:8092");
                    auto orch_req = drogon::HttpRequest::newHttpRequest();
                    orch_req->setPath("/api/v1/orchestrator/start");
                    orch_req->setMethod(drogon::Post);
                    orch_req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                    nlohmann::json start_payload = {
                        {"run_id", id},
                        {"submission_id", id},
                        {"endpoint_url", endpoint_url},
                        {"bot_count", 10},
                        {"duration_seconds", 30},
                        {"profile", {
                            {"limit_ratio", 0.6},
                            {"market_ratio", 0.3},
                            {"cancel_ratio", 0.1},
                            {"orders_per_sec_per_bot", 5},
                            {"symbols", {"AAPL", "GOOG"}}
                        }}
                    };
                    orch_req->setBody(start_payload.dump());
                    orch_client->sendRequest(orch_req, [dbPool, callback, id](drogon::ReqResult res, const drogon::HttpResponsePtr& resp) {
                        if (res == drogon::ReqResult::Ok && resp && (resp->getStatusCode() == 200 || resp->getStatusCode() == 202)) {
                            try {
                                auto conn = dbPool->acquire();
                                pqxx::work w(*conn.get());
                                w.exec_params("UPDATE submissions SET status = 'testing', updated_at = NOW() WHERE id = $1", id);
                                w.commit();
                                json j = {{"success", true}, {"status", "testing"}};
                                sendSuccess(callback, 200, j);
                            } catch (const std::exception& e) {
                                sendError(callback, 500, e.what());
                            }
                        } else {
                            sendError(callback, 500, "failed to start benchmark in orchestrator");
                        }
                    });
                });
        }, {drogon::Post});

        drogon::app().registerHandler("/api/v1/submissions/{id}/stop",
            [dbPool](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& id) {
                auto orch_client = drogon::HttpClient::newHttpClient("http://localhost:8092");
                auto orch_req = drogon::HttpRequest::newHttpRequest();
                orch_req->setPath("/api/v1/orchestrator/stop");
                orch_req->setMethod(drogon::Post);
                orch_req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                
                nlohmann::json stop_payload = {{"run_id", id}};
                orch_req->setBody(stop_payload.dump());
                
                orch_client->sendRequest(orch_req, [dbPool, callback, id](drogon::ReqResult res, const drogon::HttpResponsePtr& resp) {
                    if (res == drogon::ReqResult::Ok && resp && resp->getStatusCode() == 200) {
                        try {
                            auto conn = dbPool->acquire();
                            pqxx::work w(*conn.get());
                            w.exec_params("UPDATE submissions SET status = 'ready', updated_at = NOW() WHERE id = $1 AND status = 'testing'", id);
                            w.commit();
                            json j = {{"success", true}, {"status", "ready"}};
                            sendSuccess(callback, 200, j);
                        } catch (const std::exception& e) {
                            sendError(callback, 500, e.what());
                        }
                    } else {
                        sendError(callback, 500, "failed to stop benchmark in orchestrator");
                    }
                });
        }, {drogon::Post});

        drogon::app().registerHandler("/api/v1/teams/{name}",
            [dbPool](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& name) {
                try {
                    auto conn = dbPool->acquire();
                    std::string team_id_str;
                    bool in_db = false;
                    {
                        pqxx::nontransaction n(*conn.get());
                        pqxx::result res = n.exec_params("SELECT id FROM teams WHERE name = $1", name);
                        if (!res.empty()) {
                            team_id_str = res[0]["id"].c_str();
                            in_db = true;
                        }
                    }
                    
                    if (in_db) {
                        pqxx::work w(*conn.get());
                        w.exec_params("DELETE FROM benchmark_runs WHERE submission_id IN (SELECT id FROM submissions WHERE team_id = $1)", team_id_str);
                        w.exec_params("DELETE FROM submissions WHERE team_id = $1", team_id_str);
                        w.exec_params("DELETE FROM teams WHERE id = $1", team_id_str);
                        w.commit();
                    }
                    
                    bool in_redis = false;
                    try {
                        sw::redis::ConnectionOptions opts;
                        opts.host = "localhost";
                        opts.port = 6379;
                        sw::redis::Redis redis(opts);
                        long long rem_count = redis.zrem("leaderboard", name);
                        redis.del("scores:" + name);
                        if (rem_count > 0) in_redis = true;
                        redis.publish("leaderboard_updates", "refresh");
                    } catch (...) {}
                    
                    if (!in_db && !in_redis) {
                        sendError(callback, 404, "team not found");
                        return;
                    }
                    
                    json j = {{"success", true}};
                    sendSuccess(callback, 200, j);
                } catch (const std::exception& e) {
                    sendError(callback, 500, e.what());
                }
        }, {drogon::Delete});

        drogon::app().registerHandler("/api/v1/teams/{name}/start",
            [dbPool](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& name) {
                try {
                    auto conn = dbPool->acquire();
                    std::string sub_id;
                    {
                        pqxx::nontransaction n(*conn.get());
                        pqxx::result res = n.exec_params(
                            "SELECT s.id FROM submissions s JOIN teams t ON t.id = s.team_id WHERE t.name = $1 ORDER BY s.created_at DESC LIMIT 1", name);
                        if (res.empty()) {
                            sendError(callback, 404, "no submission found for team");
                            return;
                        }
                        sub_id = res[0]["id"].c_str();
                    }
                    
                    // Call the submission start endpoint internally
                    auto sub_client = drogon::HttpClient::newHttpClient("http://localhost:8091");
                    auto sub_req = drogon::HttpRequest::newHttpRequest();
                    sub_req->setPath("/api/v1/submissions/" + sub_id + "/start");
                    sub_req->setMethod(drogon::Post);
                    sub_client->sendRequest(sub_req, [callback](drogon::ReqResult res, const drogon::HttpResponsePtr& resp) {
                        if (res == drogon::ReqResult::Ok && resp) {
                            auto cb_resp = drogon::HttpResponse::newHttpResponse();
                            cb_resp->setStatusCode(resp->getStatusCode());
                            cb_resp->setContentTypeCode(resp->contentType());
                            cb_resp->setBody(std::string(resp->getBody()));
                            callback(cb_resp);
                        } else {
                            sendError(callback, 500, "internal error calling submission start");
                        }
                    });
                } catch (const std::exception& e) {
                    sendError(callback, 500, e.what());
                }
        }, {drogon::Post});

        drogon::app().registerHandler("/api/v1/teams/{name}/stop",
            [dbPool](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& callback, const std::string& name) {
                try {
                    auto conn = dbPool->acquire();
                    std::string sub_id;
                    {
                        pqxx::nontransaction n(*conn.get());
                        pqxx::result res = n.exec_params(
                            "SELECT s.id FROM submissions s JOIN teams t ON t.id = s.team_id WHERE t.name = $1 AND s.status = 'testing' ORDER BY s.created_at DESC LIMIT 1", name);
                        if (res.empty()) {
                            // try to fallback to latest submission if testing one isn't found
                            res = n.exec_params(
                                "SELECT s.id FROM submissions s JOIN teams t ON t.id = s.team_id WHERE t.name = $1 ORDER BY s.created_at DESC LIMIT 1", name);
                            if (res.empty()) {
                                sendError(callback, 404, "no submission found for team");
                                return;
                            }
                        }
                        sub_id = res[0]["id"].c_str();
                    }
                    
                    auto sub_client = drogon::HttpClient::newHttpClient("http://localhost:8091");
                    auto sub_req = drogon::HttpRequest::newHttpRequest();
                    sub_req->setPath("/api/v1/submissions/" + sub_id + "/stop");
                    sub_req->setMethod(drogon::Post);
                    sub_client->sendRequest(sub_req, [callback](drogon::ReqResult res, const drogon::HttpResponsePtr& resp) {
                        if (res == drogon::ReqResult::Ok && resp) {
                            auto cb_resp = drogon::HttpResponse::newHttpResponse();
                            cb_resp->setStatusCode(resp->getStatusCode());
                            cb_resp->setContentTypeCode(resp->contentType());
                            cb_resp->setBody(std::string(resp->getBody()));
                            callback(cb_resp);
                        } else {
                            sendError(callback, 500, "internal error calling submission stop");
                        }
                    });
                } catch (const std::exception& e) {
                    sendError(callback, 500, e.what());
                }
        }, {drogon::Post});

        drogon::app().addListener("0.0.0.0", cfg.port);
        drogon::app().setClientMaxBodySize(1024 * 1024 * 600); // 600 MB
        std::cout << "submission-svc listening on " << cfg.port << std::endl;

        // Background finalization poller — ensures leaderboard gets updated
        // even if the frontend isn't polling the status endpoint.
        std::atomic<bool> stop_finalizer{false};
        auto dbPoolPtr = dbPool;
        std::thread finalizer_thread([dbPoolPtr, &stop_finalizer]() {
            std::cout << "[finalizer] background finalization poller started\n";
            while (!stop_finalizer.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                if (stop_finalizer.load()) break;

                try {
                    auto conn = dbPoolPtr->acquire();
                    pqxx::nontransaction n(*conn.get());
                    pqxx::result testing_subs = n.exec(
                        "SELECT s.id, t.name as team_name FROM submissions s "
                        "JOIN teams t ON t.id = s.team_id "
                        "WHERE s.status = 'testing'"
                    );
                    
                    for (const auto& row : testing_subs) {
                        std::string sub_id = row["id"].c_str();
                        std::string team_name = row["team_name"].c_str();
                        
                        // Check orchestrator status
                        auto orch_client = drogon::HttpClient::newHttpClient("http://localhost:8092");
                        auto orch_req = drogon::HttpRequest::newHttpRequest();
                        orch_req->setPath("/api/v1/orchestrator/status/" + sub_id);
                        orch_req->setMethod(drogon::Get);
                        orch_client->sendRequest(orch_req, 
                            [dbPoolPtr, sub_id, team_name](drogon::ReqResult res, const drogon::HttpResponsePtr& resp) {
                            if (res != drogon::ReqResult::Ok || !resp || resp->getStatusCode() != 200) return;
                            
                            try {
                                auto json_obj = nlohmann::json::parse(resp->getBody());
                                std::string orch_status;
                                if (json_obj.contains("data") && json_obj["data"].contains("status")) {
                                    orch_status = json_obj["data"]["status"].get<std::string>();
                                }
                                
                                if (orch_status == "completed") {
                                    std::cout << "[finalizer] run " << sub_id << " completed, triggering finalize\n";
                                    
                                    // Update DB status
                                    auto conn2 = dbPoolPtr->acquire();
                                    pqxx::work w(*conn2.get());
                                    w.exec_params("UPDATE submissions SET status = 'complete', updated_at = NOW() WHERE id = $1 AND status = 'testing'", sub_id);
                                    w.commit();
                                    
                                    // Trigger scoring-engine finalize
                                    auto sc_client = drogon::HttpClient::newHttpClient("http://localhost:8094");
                                    auto sc_req = drogon::HttpRequest::newHttpRequest();
                                    sc_req->setPath("/api/v1/benchmark/" + sub_id + "/telemetry/finalize");
                                    sc_req->setMethod(drogon::Post);
                                    sc_req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                                    nlohmann::json f_payload = {{"team_id", team_name}};
                                    sc_req->setBody(f_payload.dump());
                                    sc_client->sendRequest(sc_req, [sub_id](drogon::ReqResult res, const drogon::HttpResponsePtr& resp) {
                                        if (res == drogon::ReqResult::Ok && resp && resp->getStatusCode() == 200) {
                                            std::cout << "[finalizer] successfully finalized run " << sub_id << " -> leaderboard updated\n";
                                        } else {
                                            std::cerr << "[finalizer] WARN: finalize call failed for " << sub_id << "\n";
                                        }
                                    });
                                } else if (orch_status == "failed") {
                                    auto conn2 = dbPoolPtr->acquire();
                                    pqxx::work w(*conn2.get());
                                    w.exec_params("UPDATE submissions SET status = 'failed', updated_at = NOW() WHERE id = $1 AND status = 'testing'", sub_id);
                                    w.commit();
                                    std::cout << "[finalizer] run " << sub_id << " failed\n";
                                } else if (orch_status == "stopped") {
                                    auto conn2 = dbPoolPtr->acquire();
                                    pqxx::work w(*conn2.get());
                                    w.exec_params("UPDATE submissions SET status = 'ready', updated_at = NOW() WHERE id = $1 AND status = 'testing'", sub_id);
                                    w.commit();
                                    std::cout << "[finalizer] run " << sub_id << " stopped, returned to ready\n";
                                }
                            } catch (const std::exception& e) {
                                std::cerr << "[finalizer] error processing " << sub_id << ": " << e.what() << "\n";
                            }
                        });
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[finalizer] poll error: " << e.what() << "\n";
                }
            }
            std::cout << "[finalizer] background finalization poller stopped\n";
        });
        finalizer_thread.detach();

        drogon::app().run();
    }

    Aws::ShutdownAPI(options);
    return 0;
}

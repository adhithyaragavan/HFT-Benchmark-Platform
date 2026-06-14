#pragma once

#include <string>
#include <memory>
#include <nlohmann/json.hpp>
#include <httplib.h>

struct OrderRequest {
    std::string client_order_id;
    std::string symbol;
    std::string side;
    std::string type;
    int64_t price = 0;
    int64_t quantity = 0;
    int64_t timestamp_ns = 0;
};

void to_json(nlohmann::json& j, const OrderRequest& o);

struct OrderResponse {
    std::string order_id;
    std::string client_order_id;
    std::string status;
    int64_t filled_qty = 0;
    int64_t filled_price = 0;
    int64_t leaves_qty = 0;
    int64_t cum_qty = 0;
    int64_t timestamp_ns = 0;
};

void from_json(const nlohmann::json& j, OrderResponse& o);

class Sender {
public:
    explicit Sender(const std::string& endpoint_url);
    ~Sender();

    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;

    std::unique_ptr<OrderResponse> send_order(const OrderRequest& order, std::string& error_msg);
    std::unique_ptr<OrderResponse> cancel_order(const std::string& order_id, std::string& error_msg);

private:
    std::unique_ptr<httplib::Client> client_;
    std::string order_path_;
};

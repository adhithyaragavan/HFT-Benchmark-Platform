#include "sender.h"
#include <iostream>

using json = nlohmann::json;

void to_json(json& j, const OrderRequest& o) {
    j = json{
        {"client_order_id", o.client_order_id},
        {"symbol", o.symbol},
        {"side", o.side},
        {"type", o.type},
        {"quantity", o.quantity},
        {"timestamp_ns", o.timestamp_ns}
    };
    if (o.type == "limit") {
        j["price"] = o.price;
    }
}

void from_json(const json& j, OrderResponse& o) {
    o.order_id = j.value("order_id", "");
    o.client_order_id = j.value("client_order_id", "");
    o.status = j.value("status", "");
    o.filled_qty = j.value("filled_qty", 0LL);
    o.filled_price = j.value("filled_price", 0LL);
    o.leaves_qty = j.value("leaves_qty", 0LL);
    o.cum_qty = j.value("cum_qty", 0LL);
    o.timestamp_ns = j.value("timestamp_ns", 0LL);
}

Sender::Sender(const std::string& endpoint_url) {
    client_ = std::make_unique<httplib::Client>(endpoint_url);
    client_->set_connection_timeout(5, 0);
    client_->set_read_timeout(10, 0);
    client_->set_write_timeout(10, 0);
    client_->set_keep_alive(true);

    // Assuming the URL is correct, we just use /api/v1/orders
    // httplib Client needs the path to start with /
    order_path_ = "/api/v1/orders";
}

Sender::~Sender() = default;

std::unique_ptr<OrderResponse> Sender::send_order(const OrderRequest& order, std::string& error_msg) {
    json j = order;
    std::string body = j.dump();

    if (auto res = client_->Post(order_path_, body, "application/json")) {
        if (res->status >= 200 && res->status < 300) {
            try {
                auto res_json = json::parse(res->body);
                auto resp = std::make_unique<OrderResponse>();
                *resp = res_json.get<OrderResponse>();
                return resp;
            } catch (const std::exception& e) {
                error_msg = "unmarshal response: " + std::string(e.what());
                return nullptr;
            }
        } else {
            error_msg = "order rejected (HTTP " + std::to_string(res->status) + "): " + res->body;
            return nullptr;
        }
    } else {
        error_msg = "send order error: " + to_string(res.error());
        return nullptr;
    }
}

std::unique_ptr<OrderResponse> Sender::cancel_order(const std::string& order_id, std::string& error_msg) {
    std::string cancel_path = order_path_ + "/" + order_id;

    if (auto res = client_->Delete(cancel_path)) {
        if (res->status == 404) {
            return nullptr;
        }
        if (res->status >= 200 && res->status < 300) {
            try {
                auto res_json = json::parse(res->body);
                auto resp = std::make_unique<OrderResponse>();
                *resp = res_json.get<OrderResponse>();
                return resp;
            } catch (const std::exception& e) {
                error_msg = "unmarshal cancel response: " + std::string(e.what());
                return nullptr;
            }
        } else {
            error_msg = "cancel rejected (HTTP " + std::to_string(res->status) + "): " + res->body;
            return nullptr;
        }
    } else {
        error_msg = "send cancel error: " + to_string(res.error());
        return nullptr;
    }
}

#include "json.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

std::int64_t future_seconds(std::int64_t seconds) {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() + seconds;
}

void respond(const cqt::json::Value& value) {
    std::cout << cqt::json::stringify(value) << '\n' << std::flush;
}

cqt::json::Value response(std::int64_t id, cqt::json::Value result) {
    cqt::json::Value::Object object;
    object["id"] = id;
    object["result"] = std::move(result);
    return object;
}

} // namespace

int main() {
    std::string line;
    while (std::getline(std::cin, line)) {
        const auto parsed = cqt::json::parse(line);
        if (!parsed || !parsed.value.is_object()) continue;
        const auto* method_value = parsed.value.find("method");
        if (!method_value || !method_value->is_string()) continue;
        const std::string method = method_value->as_string();
        const auto* id_value = parsed.value.find("id");
        if (!id_value || !id_value->is_number()) continue;
        const std::int64_t id = id_value->as_int64();

        if (method == "initialize") {
            cqt::json::Value::Object result;
            result["userAgent"] = "cqt-fake/1.0";
            result["codexHome"] = "C:/fake/.codex";
            result["platformFamily"] = "windows";
            result["platformOs"] = "Windows";
            respond(response(id, std::move(result)));
        } else if (method == "account/read") {
            cqt::json::Value::Object account;
            account["type"] = "chatgpt";
            account["planType"] = "pro";
            cqt::json::Value::Object result;
            result["account"] = std::move(account);
            result["requiresOpenaiAuth"] = true;
            respond(response(id, std::move(result)));
        } else if (method == "account/rateLimits/read") {
            cqt::json::Value::Object primary;
            primary["usedPercent"] = 32;
            primary["windowDurationMins"] = 300;
            primary["resetsAt"] = future_seconds(3 * 3600);
            cqt::json::Value::Object secondary;
            secondary["usedPercent"] = 61;
            secondary["windowDurationMins"] = 10080;
            secondary["resetsAt"] = future_seconds(4 * 86400);
            cqt::json::Value::Object limits;
            limits["limitId"] = "codex";
            limits["planType"] = "pro";
            limits["primary"] = std::move(primary);
            limits["secondary"] = std::move(secondary);
            cqt::json::Value::Object result;
            result["rateLimits"] = std::move(limits);
            respond(response(id, std::move(result)));
        } else if (method == "account/usage/read") {
            cqt::json::Value::Object summary;
            summary["lifetimeTokens"] = std::int64_t{2'600'000'000};
            summary["peakDailyTokens"] = std::int64_t{180'000'000};
            cqt::json::Value::Array buckets;
            cqt::json::Value::Object bucket;
            bucket["startDate"] = "2026-08-26";
            bucket["tokens"] = 31'000'000;
            buckets.emplace_back(std::move(bucket));
            cqt::json::Value::Object result;
            result["summary"] = std::move(summary);
            result["dailyUsageBuckets"] = std::move(buckets);
            respond(response(id, std::move(result)));
        } else {
            cqt::json::Value::Object error;
            error["code"] = -32601;
            error["message"] = "Method not found";
            cqt::json::Value::Object result;
            result["id"] = id;
            result["error"] = std::move(error);
            respond(result);
        }
    }
    return 0;
}


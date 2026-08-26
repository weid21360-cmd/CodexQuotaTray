#include "json.hpp"
#include "models.hpp"
#include "usage_history.hpp"
#include "usage_parser.hpp"

#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>

namespace {
int failures = 0;
void check(bool condition, const char* expression, int line) {
    if (condition) return;
    std::cerr << "FAILED line " << line << ": " << expression << '\n';
    ++failures;
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

void test_json() {
    const auto parsed = cqt::json::parse(R"({"name":"Codex \u989d\u5ea6","ok":true,"values":[1,2.5,null]})");
    CHECK(parsed);
    CHECK(parsed.value.find("ok") && parsed.value.find("ok")->as_bool());
    CHECK(parsed.value.find("values") && parsed.value.find("values")->as_array().size() == 3);
    CHECK(parsed.value.find("name") && parsed.value.find("name")->as_string().find("Codex") == 0);
    CHECK(cqt::json::parse(cqt::json::stringify(parsed.value)));
    CHECK(!cqt::json::parse(R"({"bad":])"));
    const auto huge = cqt::json::parse("1e300");
    CHECK(huge);
    CHECK(huge.value.as_int64(42) == 42);
    CHECK(!cqt::json::stringify(huge.value).empty());
}

void test_quota_classification() {
    CHECK(cqt::classify_quota(300) == cqt::QuotaKind::ShortTerm);
    CHECK(cqt::classify_quota(10080) == cqt::QuotaKind::Weekly);
    CHECK(cqt::classify_quota(43200) == cqt::QuotaKind::Monthly);
    CHECK(cqt::classify_quota(2880) == cqt::QuotaKind::Unknown);
}

void test_modern_rate_limits() {
    const auto parsed = cqt::json::parse(R"({
      "rateLimits": {"primary": null, "secondary": null},
      "rateLimitsByLimitId": {"codex": {"limitId": "codex", "planType": "pro",
        "primary": {"usedPercent": 23.5, "windowDurationMins": 300, "resetsAt": 1900000000},
        "secondary": {"usedPercent": 87, "windowDurationMins": 10080, "resetsAt": 1900500000},
        "monthly": {"usedPercent": 12, "windowDurationMins": 43200, "resetsAt": 1900900000}}}
    })");
    CHECK(parsed);
    cqt::UsageSnapshot snapshot;
    CHECK(cqt::parse_rate_limits(parsed.value, snapshot).ok);
    CHECK(snapshot.plan_type == "pro");
    CHECK(snapshot.quota_windows.size() == 3);
    CHECK(snapshot.quota_windows[0].kind == cqt::QuotaKind::ShortTerm);
    CHECK(std::abs(snapshot.quota_windows[0].remaining_percent - 76.5) < 0.001);
    CHECK(snapshot.quota_windows[1].kind == cqt::QuotaKind::Weekly);
    CHECK(snapshot.quota_windows[2].kind == cqt::QuotaKind::Monthly);
    const auto* short_quota = cqt::select_taskbar_quota(snapshot, cqt::TaskbarMetric::ShortTerm);
    const auto* urgent_quota = cqt::select_taskbar_quota(snapshot, cqt::TaskbarMetric::MostUrgent);
    CHECK(short_quota && short_quota->kind == cqt::QuotaKind::ShortTerm);
    CHECK(urgent_quota && urgent_quota->kind == cqt::QuotaKind::Weekly);
}

void test_legacy_and_clamping() {
    const auto parsed = cqt::json::parse(R"({"rateLimits":{"primary":{"usedPercent":140,"windowDurationMins":10080,"resetsAt":-5},"secondary":null}})");
    cqt::UsageSnapshot snapshot;
    CHECK(cqt::parse_rate_limits(parsed.value, snapshot).ok);
    CHECK(snapshot.quota_windows.size() == 1);
    CHECK(snapshot.quota_windows[0].used_percent == 100.0);
    CHECK(snapshot.quota_windows[0].remaining_percent == 0.0);
    CHECK(snapshot.quota_windows[0].resets_at == 0);
}

void test_account_and_usage() {
    const auto account = cqt::json::parse(R"({"account":{"type":"chatgpt","planType":"plus"},"requiresOpenaiAuth":true})");
    cqt::UsageSnapshot snapshot;
    CHECK(cqt::parse_account(account.value, snapshot).ok);
    CHECK(snapshot.plan_type == "plus");
    const auto usage = cqt::json::parse(R"({"summary":{"lifetimeTokens":2600000000,"peakDailyTokens":120000000},"dailyUsageBuckets":[{"startDate":"2026-08-25","tokens":1000},{"startDate":"2026-08-26","tokens":2500}]})");
    CHECK(cqt::parse_account_usage(usage.value, snapshot).ok);
    CHECK(snapshot.lifetime_tokens == 2600000000LL);
    CHECK(snapshot.account_daily.size() == 2);
    CHECK(snapshot.account_daily[1].tokens == 2500);
    const auto api_key = cqt::json::parse(R"({"account":{"type":"apiKey"},"requiresOpenaiAuth":true})");
    CHECK(!cqt::parse_account(api_key.value, snapshot).ok);
    CHECK(snapshot.health == cqt::AppHealth::LoginRequired);
    const auto personal_token = cqt::json::parse(R"({"account":{"type":"personalAccessToken"}})");
    CHECK(!cqt::parse_account(personal_token.value, snapshot).ok);
    CHECK(!snapshot.chatgpt_account);

    const auto missing_window = cqt::json::parse(R"({"rateLimits":{"planType":"pro"}})");
    CHECK(!cqt::parse_rate_limits(missing_window.value, snapshot).ok);
}

void test_incremental_history() {
    const auto unique = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() / ("cqt-history-" + unique);
    const auto sessions = root / "sessions";
    std::filesystem::create_directories(sessions);
    const auto log = sessions / "session.jsonl";

    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_s(&utc, &now);
    char timestamp[32]{};
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    const std::string prefix = std::string("{\"timestamp\":\"") + timestamp +
        "\",\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"total_token_usage\":{\"total_tokens\":";
    const std::string first = prefix + "100}}}}";
    const std::string second = prefix + "160}}}}";
    const std::size_t split = second.size() / 2;
    {
        std::ofstream stream(log, std::ios::binary);
        stream << first << '\n' << second.substr(0, split);
    }

    cqt::UsageHistory history(root);
    cqt::UsageSnapshot snapshot;
    history.refresh(snapshot);
    auto total = std::accumulate(snapshot.local_hourly.begin(), snapshot.local_hourly.end(), std::int64_t{},
                                 [](std::int64_t sum, const cqt::TokenBucket& bucket) { return sum + bucket.tokens; });
    CHECK(total == 100);
    {
        std::ofstream stream(log, std::ios::binary | std::ios::app);
        stream << second.substr(split) << '\n';
    }
    history.refresh(snapshot);
    total = std::accumulate(snapshot.local_hourly.begin(), snapshot.local_hourly.end(), std::int64_t{},
                            [](std::int64_t sum, const cqt::TokenBucket& bucket) { return sum + bucket.tokens; });
    CHECK(total == 160);
    std::error_code error;
    std::filesystem::remove_all(root, error);
}
} // namespace

int main() {
    test_json();
    test_quota_classification();
    test_modern_rate_limits();
    test_legacy_and_clamping();
    test_account_and_usage();
    test_incremental_history();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All CodexQuotaTray tests passed\n";
    return 0;
}

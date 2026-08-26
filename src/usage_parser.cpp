#include "usage_parser.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cqt {
namespace {

std::string quota_default_name(QuotaKind kind, std::int64_t duration) {
    switch (kind) {
    case QuotaKind::ShortTerm: return "Short-term quota";
    case QuotaKind::Weekly: return "Weekly quota";
    case QuotaKind::Monthly: return "Monthly quota";
    case QuotaKind::Unknown: return std::to_string(duration) + " minute quota";
    }
    return "Codex quota";
}

void append_window(const json::Value* value, std::string id, const std::string& limit_name, std::vector<QuotaWindow>& output) {
    if (!value || !value->is_object()) return;
    const auto* used = value->find("usedPercent");
    const auto* duration = value->find("windowDurationMins");
    if (!used || !duration || !used->is_number() || !duration->is_number()) return;

    QuotaWindow window;
    window.id = std::move(id);
    window.used_percent = clamp_percent(used->as_number());
    window.remaining_percent = clamp_percent(100.0 - window.used_percent);
    window.duration_minutes = std::max<std::int64_t>(0, duration->as_int64());
    window.kind = classify_quota(window.duration_minutes);
    window.name = limit_name.empty() ? quota_default_name(window.kind, window.duration_minutes) : limit_name;
    if (const auto* reset = value->find("resetsAt")) window.resets_at = std::max<std::int64_t>(0, reset->as_int64());
    output.push_back(std::move(window));
}

} // namespace

ParseStatus parse_account(const json::Value& result, UsageSnapshot& snapshot) {
    const auto* account = result.find("account");
    if (!account || account->is_null()) {
        snapshot.chatgpt_account = false;
        snapshot.plan_type.clear();
        snapshot.quota_windows.clear();
        snapshot.health = AppHealth::LoginRequired;
        snapshot.status_detail = "No Codex account is signed in";
        return {false, snapshot.status_detail};
    }
    if (!account->is_object()) return {false, "Invalid account response"};
    const std::string type = account->find("type") ? account->find("type")->string_or() : std::string{};
    if (type != "chatgpt") {
        snapshot.chatgpt_account = false;
        snapshot.plan_type.clear();
        snapshot.quota_windows.clear();
        snapshot.health = AppHealth::LoginRequired;
        snapshot.status_detail = "ChatGPT sign-in is required for quota data";
        return {false, snapshot.status_detail};
    }
    snapshot.chatgpt_account = true;
    if (const auto* plan = account->find("planType")) snapshot.plan_type = plan->string_or();
    return {true, {}};
}

ParseStatus parse_rate_limits(const json::Value& result, UsageSnapshot& snapshot) {
    const json::Value* limits = nullptr;
    if (const auto* by_id = result.find("rateLimitsByLimitId"); by_id && by_id->is_object()) {
        const auto* candidate = by_id->find("codex");
        if (candidate && candidate->is_object()) limits = candidate;
    }
    if (!limits) limits = result.find("rateLimits");
    if (!limits || !limits->is_object()) return {false, "Rate-limit response did not contain Codex limits"};

    std::string limit_name;
    if (const auto* value = limits->find("limitName")) limit_name = value->string_or();
    if (const auto* value = limits->find("planType"); value && snapshot.plan_type.empty()) snapshot.plan_type = value->string_or();

    std::vector<QuotaWindow> windows;
    append_window(limits->find("primary"), "primary", limit_name, windows);
    append_window(limits->find("secondary"), "secondary", limit_name, windows);
    for (const auto& [key, value] : limits->as_object()) {
        if (key == "primary" || key == "secondary") continue;
        if (value.is_object() && value.find("usedPercent") && value.find("windowDurationMins")) {
            append_window(&value, key, limit_name, windows);
        }
    }
    if (windows.empty()) return {false, "Rate-limit response did not contain a usable quota window"};
    std::stable_sort(windows.begin(), windows.end(), [](const auto& left, const auto& right) {
        return left.duration_minutes < right.duration_minutes;
    });
    snapshot.quota_windows = std::move(windows);
    snapshot.health = AppHealth::Healthy;
    snapshot.status_detail.clear();
    snapshot.from_cache = false;
    snapshot.updated_at = unix_now();
    return {true, {}};
}

ParseStatus parse_account_usage(const json::Value& result, UsageSnapshot& snapshot) {
    const auto* summary = result.find("summary");
    if (summary && summary->is_object()) {
        if (const auto* item = summary->find("lifetimeTokens")) snapshot.lifetime_tokens = std::max<std::int64_t>(0, item->as_int64());
        if (const auto* item = summary->find("peakDailyTokens")) snapshot.peak_daily_tokens = std::max<std::int64_t>(0, item->as_int64());
    }
    const auto* buckets = result.find("dailyUsageBuckets");
    if (buckets && buckets->is_array()) {
        std::vector<TokenBucket> parsed;
        for (const auto& item : buckets->as_array()) {
            if (!item.is_object()) continue;
            const auto* date = item.find("startDate");
            const auto* tokens = item.find("tokens");
            if (!date || !tokens) continue;
            TokenBucket bucket;
            bucket.date = date->string_or();
            bucket.tokens = std::max<std::int64_t>(0, tokens->as_int64());
            bucket.scope = UsageScope::Account;
            parsed.push_back(std::move(bucket));
        }
        std::stable_sort(parsed.begin(), parsed.end(), [](const auto& left, const auto& right) {
            return left.date < right.date;
        });
        snapshot.account_daily = std::move(parsed);
    }
    return {summary || buckets, (summary || buckets) ? std::string{} : "Token usage is unavailable"};
}

QuotaKind classify_quota(std::int64_t duration_minutes) {
    if (duration_minutes <= 0) return QuotaKind::Unknown;
    if (duration_minutes <= 24 * 60) return QuotaKind::ShortTerm;
    if (duration_minutes >= 6 * 24 * 60 && duration_minutes <= 8 * 24 * 60) return QuotaKind::Weekly;
    if (duration_minutes >= 27 * 24 * 60 && duration_minutes <= 32 * 24 * 60) return QuotaKind::Monthly;
    return QuotaKind::Unknown;
}

const QuotaWindow* select_taskbar_quota(const UsageSnapshot& snapshot, TaskbarMetric metric) {
    if (snapshot.quota_windows.empty()) return nullptr;
    const auto by_kind = [&](QuotaKind kind) -> const QuotaWindow* {
        const auto iterator = std::find_if(snapshot.quota_windows.begin(), snapshot.quota_windows.end(), [kind](const auto& window) { return window.kind == kind; });
        return iterator == snapshot.quota_windows.end() ? nullptr : &*iterator;
    };
    if (metric == TaskbarMetric::Weekly) {
        if (const auto* weekly = by_kind(QuotaKind::Weekly)) return weekly;
    } else if (metric == TaskbarMetric::ShortTerm) {
        if (const auto* short_term = by_kind(QuotaKind::ShortTerm)) return short_term;
    } else {
        return &*std::min_element(snapshot.quota_windows.begin(), snapshot.quota_windows.end(), [](const auto& left, const auto& right) {
            return left.remaining_percent < right.remaining_percent;
        });
    }
    return &*std::min_element(snapshot.quota_windows.begin(), snapshot.quota_windows.end(), [](const auto& left, const auto& right) {
        return left.duration_minutes < right.duration_minutes;
    });
}

} // namespace cqt

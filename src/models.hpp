#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace cqt {

enum class AppHealth {
    Starting,
    Healthy,
    Cached,
    Stale,
    LoginRequired,
    Unavailable,
};

enum class QuotaKind {
    ShortTerm,
    Weekly,
    Monthly,
    Unknown,
};

struct QuotaWindow {
    std::string id;
    std::string name;
    QuotaKind kind = QuotaKind::Unknown;
    double used_percent = 0.0;
    double remaining_percent = 100.0;
    std::int64_t duration_minutes = 0;
    std::int64_t resets_at = 0;
};

enum class UsageScope {
    Account,
    Local,
};

struct TokenBucket {
    std::int64_t start_epoch = 0;
    std::string date;
    std::int64_t tokens = 0;
    UsageScope scope = UsageScope::Account;
};

struct UsageSnapshot {
    std::string plan_type;
    bool chatgpt_account = false;
    std::vector<QuotaWindow> quota_windows;
    std::vector<TokenBucket> account_daily;
    std::vector<TokenBucket> local_hourly;
    std::vector<TokenBucket> local_daily;
    std::int64_t lifetime_tokens = 0;
    std::int64_t peak_daily_tokens = 0;
    AppHealth health = AppHealth::Starting;
    std::string status_detail;
    std::int64_t updated_at = 0;
    bool from_cache = false;
};

enum class ThemeMode { FollowSystem, Dark, Light };
enum class LanguageMode { FollowSystem, Chinese, English };
enum class TaskbarMetric { ShortTerm, Weekly, MostUrgent };
enum class ChartRange { Hours24, Days7, Days30 };

struct Settings {
    ThemeMode theme = ThemeMode::FollowSystem;
    LanguageMode language = LanguageMode::FollowSystem;
    TaskbarMetric taskbar_metric = TaskbarMetric::ShortTerm;
    ChartRange chart_range = ChartRange::Days30;
    int palette = 1;
    std::uint32_t custom_primary = 0xff5ac8df;
    std::uint32_t custom_secondary = 0xff459ee2;
    double window_scale = 1.04;
    double font_scale = 1.0;
    double taskbar_scale = 1.0;
    double glass_opacity = 0.86;
    bool soft_glass = true;
    bool capsule_enabled = false;
    std::wstring codex_executable;
};

inline std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

inline double clamp_percent(double value) {
    return std::clamp(value, 0.0, 100.0);
}

} // namespace cqt

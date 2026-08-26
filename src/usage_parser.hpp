#pragma once

#include "json.hpp"
#include "models.hpp"

#include <string>

namespace cqt {

struct ParseStatus {
    bool ok = false;
    std::string error;
};

[[nodiscard]] ParseStatus parse_account(const json::Value& result, UsageSnapshot& snapshot);
[[nodiscard]] ParseStatus parse_rate_limits(const json::Value& result, UsageSnapshot& snapshot);
[[nodiscard]] ParseStatus parse_account_usage(const json::Value& result, UsageSnapshot& snapshot);
[[nodiscard]] QuotaKind classify_quota(std::int64_t duration_minutes);
[[nodiscard]] const QuotaWindow* select_taskbar_quota(const UsageSnapshot& snapshot, TaskbarMetric metric);

} // namespace cqt


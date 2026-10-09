#pragma once

#include "models.hpp"

#include <atomic>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace cqt {

class UsageHistory {
public:
    explicit UsageHistory(std::filesystem::path codex_home = {});

    // Returns only the tokens discovered during this incremental refresh.
    [[nodiscard]] std::int64_t refresh(UsageSnapshot& snapshot);
    // Fast path used by the directory watcher; only the changed JSONL files are touched.
    [[nodiscard]] std::int64_t refresh_changed(
        UsageSnapshot& snapshot, const std::vector<std::filesystem::path>& relative_paths);
    void cancel() { stop_requested_.store(true); }
    [[nodiscard]] const std::filesystem::path& codex_home() const { return codex_home_; }

private:
    struct FileState {
        std::filesystem::path path;
        std::uintmax_t offset = 0;
        std::int64_t last_total = 0;
    };

    void discover_files();
    [[nodiscard]] std::int64_t scan_file(FileState& state);
    void prune();
    void apply_to(UsageSnapshot& snapshot) const;

    std::filesystem::path codex_home_;
    std::map<std::wstring, FileState, std::less<>> files_;
    std::map<std::int64_t, std::int64_t> hourly_tokens_;
    std::map<std::string, std::int64_t, std::less<>> daily_tokens_;
    std::atomic_bool stop_requested_{false};
};

} // namespace cqt

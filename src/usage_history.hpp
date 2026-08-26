#pragma once

#include "models.hpp"

#include <atomic>
#include <filesystem>
#include <map>
#include <string>

namespace cqt {

class UsageHistory {
public:
    explicit UsageHistory(std::filesystem::path codex_home = {});

    void refresh(UsageSnapshot& snapshot);
    void cancel() { stop_requested_.store(true); }
    [[nodiscard]] const std::filesystem::path& codex_home() const { return codex_home_; }

private:
    struct FileState {
        std::filesystem::path path;
        std::uintmax_t offset = 0;
        std::int64_t last_total = 0;
    };

    void discover_files();
    void scan_file(FileState& state);
    void prune();

    std::filesystem::path codex_home_;
    std::map<std::wstring, FileState, std::less<>> files_;
    std::map<std::int64_t, std::int64_t> hourly_tokens_;
    std::map<std::string, std::int64_t, std::less<>> daily_tokens_;
    std::atomic_bool stop_requested_{false};
};

} // namespace cqt

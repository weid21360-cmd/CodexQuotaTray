#pragma once

#include "models.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace cqt {

class SettingsStore {
public:
    SettingsStore();

    [[nodiscard]] Settings load_settings() const;
    [[nodiscard]] bool save_settings(const Settings& settings) const;
    [[nodiscard]] UsageSnapshot load_cache() const;
    [[nodiscard]] bool save_cache(const UsageSnapshot& snapshot) const;

    [[nodiscard]] bool startup_enabled() const;
    [[nodiscard]] bool set_startup_enabled(bool enabled) const;

    [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }
    [[nodiscard]] const std::filesystem::path& settings_path() const { return settings_path_; }
    [[nodiscard]] const std::filesystem::path& cache_path() const { return cache_path_; }

    static std::string wide_to_utf8(std::wstring_view text);
    static std::wstring utf8_to_wide(std::string_view text);

private:
    [[nodiscard]] static bool atomic_write(const std::filesystem::path& path, const std::string& data);
    [[nodiscard]] static std::string read_all(const std::filesystem::path& path);

    std::filesystem::path directory_;
    std::filesystem::path settings_path_;
    std::filesystem::path cache_path_;
};

} // namespace cqt

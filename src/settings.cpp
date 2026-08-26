#include "settings.hpp"

#include "json.hpp"

#include <Windows.h>
#include <ShlObj.h>

#include <fstream>
#include <iterator>
#include <system_error>

namespace cqt {
namespace {

template <typename Enum>
Enum enum_value(const json::Value* value, Enum fallback, int maximum) {
    if (!value || !value->is_number()) return fallback;
    const int raw = static_cast<int>(value->as_number());
    if (raw < 0 || raw > maximum) return fallback;
    return static_cast<Enum>(raw);
}

double number_value(const json::Value* value, double fallback, double minimum, double maximum) {
    if (!value || !value->is_number()) return fallback;
    return std::clamp(value->as_number(), minimum, maximum);
}

json::Value quota_to_json(const QuotaWindow& window) {
    json::Value::Object object;
    object["id"] = window.id;
    object["name"] = window.name;
    object["kind"] = static_cast<int>(window.kind);
    object["usedPercent"] = window.used_percent;
    object["remainingPercent"] = window.remaining_percent;
    object["durationMinutes"] = window.duration_minutes;
    object["resetsAt"] = window.resets_at;
    return object;
}

QuotaWindow quota_from_json(const json::Value& value) {
    QuotaWindow window;
    if (const auto* item = value.find("id")) window.id = item->string_or();
    if (const auto* item = value.find("name")) window.name = item->string_or();
    window.kind = enum_value(value.find("kind"), QuotaKind::Unknown, 3);
    if (const auto* item = value.find("usedPercent")) window.used_percent = clamp_percent(item->as_number());
    if (const auto* item = value.find("remainingPercent")) window.remaining_percent = clamp_percent(item->as_number());
    if (const auto* item = value.find("durationMinutes")) window.duration_minutes = item->as_int64();
    if (const auto* item = value.find("resetsAt")) window.resets_at = item->as_int64();
    return window;
}

json::Value bucket_to_json(const TokenBucket& bucket) {
    json::Value::Object object;
    object["startEpoch"] = bucket.start_epoch;
    object["date"] = bucket.date;
    object["tokens"] = bucket.tokens;
    object["scope"] = static_cast<int>(bucket.scope);
    return object;
}

TokenBucket bucket_from_json(const json::Value& value) {
    TokenBucket bucket;
    if (const auto* item = value.find("startEpoch")) bucket.start_epoch = item->as_int64();
    if (const auto* item = value.find("date")) bucket.date = item->string_or();
    if (const auto* item = value.find("tokens")) bucket.tokens = std::max<std::int64_t>(0, item->as_int64());
    bucket.scope = enum_value(value.find("scope"), UsageScope::Account, 1);
    return bucket;
}

void append_buckets(json::Value::Object& root, const char* key, const std::vector<TokenBucket>& buckets) {
    json::Value::Array array;
    array.reserve(buckets.size());
    for (const auto& bucket : buckets) array.push_back(bucket_to_json(bucket));
    root[key] = std::move(array);
}

void read_buckets(const json::Value* value, std::vector<TokenBucket>& target) {
    if (!value || !value->is_array()) return;
    target.clear();
    for (const auto& item : value->as_array()) {
        if (!item.is_object()) continue;
        target.push_back(bucket_from_json(item));
    }
}

} // namespace

SettingsStore::SettingsStore() {
    PWSTR raw_path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw_path)) && raw_path) {
        directory_ = std::filesystem::path(raw_path) / L"CodexQuotaTray";
        CoTaskMemFree(raw_path);
    } else {
        wchar_t fallback[MAX_PATH]{};
        GetTempPathW(MAX_PATH, fallback);
        directory_ = std::filesystem::path(fallback) / L"CodexQuotaTray";
    }
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    settings_path_ = directory_ / L"settings.json";
    cache_path_ = directory_ / L"usage-cache.json";
}

Settings SettingsStore::load_settings() const {
    Settings settings;
    const auto result = json::parse(read_all(settings_path_));
    if (!result || !result.value.is_object()) return settings;

    const auto& root = result.value;
    settings.theme = enum_value(root.find("theme"), settings.theme, 2);
    settings.language = enum_value(root.find("language"), settings.language, 2);
    settings.taskbar_metric = enum_value(root.find("taskbarMetric"), settings.taskbar_metric, 2);
    settings.chart_range = enum_value(root.find("chartRange"), settings.chart_range, 2);
    if (const auto* item = root.find("palette")) settings.palette = std::clamp(static_cast<int>(item->as_number()), 0, 4);
    if (const auto* item = root.find("customPrimary")) settings.custom_primary = static_cast<std::uint32_t>(item->as_int64(settings.custom_primary));
    if (const auto* item = root.find("customSecondary")) settings.custom_secondary = static_cast<std::uint32_t>(item->as_int64(settings.custom_secondary));
    settings.window_scale = number_value(root.find("windowScale"), settings.window_scale, 0.8, 1.4);
    settings.font_scale = number_value(root.find("fontScale"), settings.font_scale, 0.8, 1.4);
    settings.taskbar_scale = number_value(root.find("taskbarScale"), settings.taskbar_scale, 0.8, 1.4);
    settings.glass_opacity = number_value(root.find("glassOpacity"), settings.glass_opacity, 0.2, 1.0);
    if (const auto* item = root.find("softGlass")) settings.soft_glass = item->as_bool(settings.soft_glass);
    if (const auto* item = root.find("capsuleEnabled")) settings.capsule_enabled = item->as_bool(settings.capsule_enabled);
    if (const auto* item = root.find("codexExecutable")) settings.codex_executable = utf8_to_wide(item->string_or());
    return settings;
}

bool SettingsStore::save_settings(const Settings& settings) const {
    json::Value::Object root;
    root["schemaVersion"] = 1;
    root["theme"] = static_cast<int>(settings.theme);
    root["language"] = static_cast<int>(settings.language);
    root["taskbarMetric"] = static_cast<int>(settings.taskbar_metric);
    root["chartRange"] = static_cast<int>(settings.chart_range);
    root["palette"] = settings.palette;
    root["customPrimary"] = static_cast<std::int64_t>(settings.custom_primary);
    root["customSecondary"] = static_cast<std::int64_t>(settings.custom_secondary);
    root["windowScale"] = settings.window_scale;
    root["fontScale"] = settings.font_scale;
    root["taskbarScale"] = settings.taskbar_scale;
    root["glassOpacity"] = settings.glass_opacity;
    root["softGlass"] = settings.soft_glass;
    root["capsuleEnabled"] = settings.capsule_enabled;
    root["codexExecutable"] = wide_to_utf8(settings.codex_executable);
    return atomic_write(settings_path_, json::stringify(root, true));
}

UsageSnapshot SettingsStore::load_cache() const {
    UsageSnapshot snapshot;
    const auto result = json::parse(read_all(cache_path_));
    if (!result || !result.value.is_object()) return snapshot;

    const auto& root = result.value;
    if (const auto* item = root.find("planType")) snapshot.plan_type = item->string_or();
    if (const auto* item = root.find("lifetimeTokens")) snapshot.lifetime_tokens = std::max<std::int64_t>(0, item->as_int64());
    if (const auto* item = root.find("peakDailyTokens")) snapshot.peak_daily_tokens = std::max<std::int64_t>(0, item->as_int64());
    if (const auto* item = root.find("updatedAt")) snapshot.updated_at = item->as_int64();
    if (const auto* items = root.find("quotaWindows"); items && items->is_array()) {
        for (const auto& item : items->as_array()) {
            if (item.is_object()) snapshot.quota_windows.push_back(quota_from_json(item));
        }
    }
    read_buckets(root.find("accountDaily"), snapshot.account_daily);
    read_buckets(root.find("localHourly"), snapshot.local_hourly);
    read_buckets(root.find("localDaily"), snapshot.local_daily);
    if (snapshot.updated_at > 0) {
        snapshot.from_cache = true;
        snapshot.health = unix_now() - snapshot.updated_at <= 600 ? AppHealth::Cached : AppHealth::Stale;
    }
    return snapshot;
}

bool SettingsStore::save_cache(const UsageSnapshot& snapshot) const {
    json::Value::Object root;
    root["schemaVersion"] = 1;
    root["planType"] = snapshot.plan_type;
    root["lifetimeTokens"] = snapshot.lifetime_tokens;
    root["peakDailyTokens"] = snapshot.peak_daily_tokens;
    root["updatedAt"] = snapshot.updated_at;
    json::Value::Array quotas;
    quotas.reserve(snapshot.quota_windows.size());
    for (const auto& window : snapshot.quota_windows) quotas.push_back(quota_to_json(window));
    root["quotaWindows"] = std::move(quotas);
    append_buckets(root, "accountDaily", snapshot.account_daily);
    append_buckets(root, "localHourly", snapshot.local_hourly);
    append_buckets(root, "localDaily", snapshot.local_daily);
    return atomic_write(cache_path_, json::stringify(root, true));
}

bool SettingsStore::startup_enabled() const {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    const LONG result = RegQueryValueExW(key, L"CodexQuotaTray", nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

bool SettingsStore::set_startup_enabled(bool enabled) const {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    LONG result = ERROR_SUCCESS;
    if (enabled) {
        std::wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        executable.resize(length);
        const std::wstring command = L"\"" + executable + L"\" --background";
        result = RegSetValueExW(key, L"CodexQuotaTray", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, L"CodexQuotaTray");
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

std::string SettingsStore::wide_to_utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string output(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), output.data(), length, nullptr, nullptr);
    return output;
}

std::wstring SettingsStore::utf8_to_wide(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring output(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), output.data(), length);
    return output;
}

bool SettingsStore::atomic_write(const std::filesystem::path& path, const std::string& data) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const auto temporary = path.wstring() + L".tmp";
    {
        std::ofstream stream(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        stream.write(data.data(), static_cast<std::streamsize>(data.size()));
        stream.flush();
        if (!stream) return false;
    }
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

std::string SettingsStore::read_all(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

} // namespace cqt

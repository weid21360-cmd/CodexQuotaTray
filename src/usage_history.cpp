#include "usage_history.hpp"

#include "json.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace cqt {
namespace {

constexpr std::int64_t kSecondsPerHour = 60 * 60;
constexpr std::int64_t kSecondsPerDay = 24 * kSecondsPerHour;

const json::Value* find_recursive_key(const json::Value& value, std::string_view key, int depth = 0) {
    if (depth > 8) return nullptr;
    if (value.is_object()) {
        if (const auto* direct = value.find(key)) return direct;
        for (const auto& [name, child] : value.as_object()) {
            (void)name;
            if (const auto* found = find_recursive_key(child, key, depth + 1)) return found;
        }
    } else if (value.is_array()) {
        for (const auto& child : value.as_array()) {
            if (const auto* found = find_recursive_key(child, key, depth + 1)) return found;
        }
    }
    return nullptr;
}

std::int64_t parse_timestamp(std::string_view text) {
    if (text.size() < 19) return 0;
    auto number = [&](std::size_t offset, std::size_t length) -> int {
        int result = 0;
        for (std::size_t i = 0; i < length; ++i) {
            const char c = text[offset + i];
            if (c < '0' || c > '9') return -1;
            result = result * 10 + (c - '0');
        }
        return result;
    };
    SYSTEMTIME system_time{};
    const int year = number(0, 4);
    const int month = number(5, 2);
    const int day = number(8, 2);
    const int hour = number(11, 2);
    const int minute = number(14, 2);
    const int second = number(17, 2);
    if (year < 1970 || month < 1 || day < 1 || hour < 0 || minute < 0 || second < 0) return 0;
    system_time.wYear = static_cast<WORD>(year);
    system_time.wMonth = static_cast<WORD>(month);
    system_time.wDay = static_cast<WORD>(day);
    system_time.wHour = static_cast<WORD>(hour);
    system_time.wMinute = static_cast<WORD>(minute);
    system_time.wSecond = static_cast<WORD>(second);
    FILETIME file_time{};
    if (!SystemTimeToFileTime(&system_time, &file_time)) return 0;
    ULARGE_INTEGER ticks{};
    ticks.LowPart = file_time.dwLowDateTime;
    ticks.HighPart = file_time.dwHighDateTime;
    constexpr std::uint64_t kUnixEpochTicks = 116444736000000000ULL;
    if (ticks.QuadPart < kUnixEpochTicks) return 0;
    return static_cast<std::int64_t>((ticks.QuadPart - kUnixEpochTicks) / 10000000ULL);
}

std::string local_date(std::int64_t epoch) {
    std::time_t time = static_cast<std::time_t>(epoch);
    std::tm local{};
    if (localtime_s(&local, &time) != 0) return {};
    char buffer[16]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local);
    return buffer;
}

bool is_token_count_event(const json::Value& line) {
    const auto* type = line.find("type");
    if (!type || type->string_or() != "event_msg") return false;
    const auto* payload = line.find("payload");
    if (!payload || !payload->is_object()) return false;
    const auto* payload_type = payload->find("type");
    return payload_type && payload_type->string_or() == "token_count";
}

std::int64_t cumulative_total(const json::Value& line) {
    const auto* total_usage = find_recursive_key(line, "total_token_usage");
    if (!total_usage || !total_usage->is_object()) return -1;

    // Codex reports cached prompt tokens inside input_tokens. They are useful for
    // billing/cache diagnostics, but counting them again makes a tiny turn look
    // like a 100K+ Token burst. Use the cumulative non-cached input plus output so
    // subtracting consecutive samples yields the work newly consumed by the turn.
    const auto* input = total_usage->find("input_tokens");
    const auto* output = total_usage->find("output_tokens");
    if (input && input->is_number() && output && output->is_number()) {
        const std::int64_t input_tokens = std::max<std::int64_t>(0, input->as_int64());
        const std::int64_t output_tokens = std::max<std::int64_t>(0, output->as_int64());
        std::int64_t cached_input_tokens = 0;
        if (const auto* cached = total_usage->find("cached_input_tokens"); cached && cached->is_number()) {
            cached_input_tokens = std::max<std::int64_t>(0, cached->as_int64());
        }
        return std::max<std::int64_t>(0, input_tokens - cached_input_tokens) + output_tokens;
    }

    // Older Codex builds only expose total_tokens.
    if (const auto* total = total_usage->find("total_tokens"); total && total->is_number()) {
        return total->as_int64(-1);
    }
    return -1;
}

} // namespace

UsageHistory::UsageHistory(std::filesystem::path codex_home) : codex_home_(std::move(codex_home)) {
    if (!codex_home_.empty()) return;
    std::wstring configured(32768, L'\0');
    DWORD configured_length = GetEnvironmentVariableW(L"CODEX_HOME", configured.data(), static_cast<DWORD>(configured.size()));
    if (configured_length > 0 && configured_length < configured.size()) {
        configured.resize(configured_length);
        codex_home_ = configured;
        return;
    }
    std::wstring profile(32768, L'\0');
    DWORD length = GetEnvironmentVariableW(L"USERPROFILE", profile.data(), static_cast<DWORD>(profile.size()));
    if (length > 0 && length < profile.size()) {
        profile.resize(length);
        codex_home_ = std::filesystem::path(profile) / L".codex";
    }
}

std::int64_t UsageHistory::refresh(UsageSnapshot& snapshot) {
    if (stop_requested_.load()) return 0;
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    discover_files();
    std::int64_t discovered_tokens = 0;
    for (auto& [key, state] : files_) {
        if (stop_requested_.load()) break;
        (void)key;
        discovered_tokens += scan_file(state);
    }
    prune();
    apply_to(snapshot);
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    return discovered_tokens;
}

std::int64_t UsageHistory::refresh_changed(
    UsageSnapshot& snapshot, const std::vector<std::filesystem::path>& relative_paths) {
    if (stop_requested_.load()) return 0;
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    std::int64_t discovered_tokens = 0;
    for (const auto& relative : relative_paths) {
        if (stop_requested_.load() || relative.empty() || relative.is_absolute() ||
            relative.extension() != L".jsonl") continue;
        bool safe = true;
        for (const auto& component : relative) {
            if (component == L"..") {
                safe = false;
                break;
            }
        }
        if (!safe) continue;

        auto part = relative.begin();
        if (part == relative.end()) continue;
        std::wstring root = part->wstring();
        std::transform(root.begin(), root.end(), root.begin(), [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
        if (root != L"sessions" && root != L"archived_sessions") continue;

        const auto path = codex_home_ / relative;
        const std::wstring key = path.filename().wstring();
        auto [position, inserted] = files_.try_emplace(key);
        if (inserted) position->second.path = path;
        else if (position->second.path != path) {
            std::error_code old_path_error;
            if (position->second.offset == 0 || !std::filesystem::exists(position->second.path, old_path_error)) {
                position->second.path = path;
            }
        }
        discovered_tokens += scan_file(position->second);
    }
    prune();
    apply_to(snapshot);
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    return discovered_tokens;
}

std::int64_t UsageHistory::refresh_live_threads(UsageSnapshot& snapshot) {
    if (stop_requested_.load()) return 0;
    const auto thread_ids = writer_thread_ids();
    if (thread_ids.empty()) return 0;

    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    std::int64_t discovered_tokens = 0;
    for (auto& [key, state] : files_) {
        if (stop_requested_.load()) break;
        const bool has_writer = std::any_of(thread_ids.begin(), thread_ids.end(), [&](const std::wstring& id) {
            return key.find(id) != std::wstring::npos;
        });
        if (has_writer) discovered_tokens += scan_file(state);
    }
    if (discovered_tokens > 0) {
        prune();
        apply_to(snapshot);
    }
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    return discovered_tokens;
}

std::vector<std::wstring> UsageHistory::writer_thread_ids() const {
    std::vector<std::wstring> ids;
    const auto root = codex_home_ / L"thread-writer-locks";
    std::error_code error;
    std::filesystem::directory_iterator iterator(root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::directory_iterator end;
    while (iterator != end) {
        if (error) {
            error.clear();
            iterator.increment(error);
            continue;
        }
        const auto& entry = *iterator;
        if (entry.is_regular_file(error) && entry.path().extension() == L".lock") {
            const std::wstring id = entry.path().stem().wstring();
            if (!id.empty() && id.front() != L'.') ids.push_back(id);
        }
        error.clear();
        iterator.increment(error);
    }
    return ids;
}

void UsageHistory::discover_files() {
    const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 31);
    const auto thread_ids = writer_thread_ids();
    for (const auto& root : {codex_home_ / L"sessions", codex_home_ / L"archived_sessions"}) {
        std::error_code error;
        if (!std::filesystem::exists(root, error)) continue;
        std::filesystem::recursive_directory_iterator iterator(root, std::filesystem::directory_options::skip_permission_denied, error);
        const std::filesystem::recursive_directory_iterator end;
        while (iterator != end) {
            if (stop_requested_.load()) return;
            if (error) {
                error.clear();
                iterator.increment(error);
                continue;
            }
            const auto& entry = *iterator;
            if (entry.is_regular_file(error) && entry.path().extension() == L".jsonl") {
                const std::wstring key = entry.path().filename().wstring();
                const auto modified = entry.last_write_time(error);
                const bool has_writer = std::any_of(thread_ids.begin(), thread_ids.end(), [&](const std::wstring& id) {
                    return key.find(id) != std::wstring::npos;
                });
                if (!error && (modified >= cutoff || has_writer)) {
                    // Session filenames are UUIDs; retaining the key across a move to
                    // archived_sessions avoids rescanning and double-counting the file.
                    auto [position, inserted] = files_.try_emplace(key);
                    if (inserted) position->second.path = entry.path();
                    else if (position->second.path != entry.path()) {
                        std::error_code old_path_error;
                        if (position->second.offset == 0 || !std::filesystem::exists(position->second.path, old_path_error)) {
                            position->second.path = entry.path();
                        }
                    }
                }
            }
            error.clear();
            iterator.increment(error);
        }
    }
}

std::int64_t UsageHistory::scan_file(FileState& state) {
    std::error_code error;
    const auto size = std::filesystem::file_size(state.path, error);
    if (error) return 0;
    if (size < state.offset) {
        state.offset = 0;
        state.last_total = 0;
    }
    if (size == state.offset) return 0;

    std::ifstream stream(state.path, std::ios::binary);
    if (!stream) return 0;
    if (state.offset > 0) stream.seekg(static_cast<std::streamoff>(state.offset));

    std::int64_t discovered_tokens = 0;
    auto consume_line = [&](std::string& line) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find("\"type\":\"event_msg\"") == std::string::npos ||
            line.find("\"payload\":{\"type\":\"token_count\"") == std::string::npos) return;
        const auto parsed = json::parse(line);
        if (!parsed || !parsed.value.is_object() || !is_token_count_event(parsed.value)) return;
        const std::int64_t total = cumulative_total(parsed.value);
        if (total < 0) return;
        std::int64_t delta = total >= state.last_total ? total - state.last_total : total;
        state.last_total = total;
        if (delta <= 0) return;
        const auto* timestamp_value = parsed.value.find("timestamp");
        const std::int64_t timestamp = timestamp_value ? parse_timestamp(timestamp_value->string_or()) : 0;
        if (timestamp <= 0) return;
        const std::int64_t hour = timestamp - (timestamp % kSecondsPerHour);
        hourly_tokens_[hour] += delta;
        discovered_tokens += delta;
        const std::string date = local_date(timestamp);
        if (!date.empty()) daily_tokens_[date] += delta;
    };

    constexpr std::size_t kMaximumMetadataLine = 512 * 1024;
    std::array<char, 64 * 1024> buffer{};
    std::string line;
    line.reserve(16 * 1024);
    bool oversized = false;
    std::uintmax_t cursor = state.offset;
    std::uintmax_t committed_offset = state.offset;
    while (stream && !stop_requested_.load()) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = stream.gcount();
        if (count <= 0) break;
        for (std::streamsize index = 0; index < count; ++index) {
            const char character = buffer[static_cast<std::size_t>(index)];
            ++cursor;
            if (character == '\n') {
                if (!oversized) consume_line(line);
                line.clear();
                oversized = false;
                committed_offset = cursor;
            } else if (!oversized) {
                if (line.size() < kMaximumMetadataLine) line.push_back(character);
                else {
                    line.clear();
                    oversized = true;
                }
            }
        }
    }
    state.offset = committed_offset;
    return discovered_tokens;
}

void UsageHistory::prune() {
    const std::int64_t now = unix_now();
    const std::int64_t hour_cutoff = now - 25 * kSecondsPerHour;
    while (!hourly_tokens_.empty() && hourly_tokens_.begin()->first < hour_cutoff) hourly_tokens_.erase(hourly_tokens_.begin());

    const std::string date_cutoff = local_date(now - 31 * kSecondsPerDay);
    while (!daily_tokens_.empty() && daily_tokens_.begin()->first < date_cutoff) daily_tokens_.erase(daily_tokens_.begin());
}

void UsageHistory::apply_to(UsageSnapshot& snapshot) const {
    snapshot.local_hourly.clear();
    for (const auto& [epoch, tokens] : hourly_tokens_) {
        snapshot.local_hourly.push_back({epoch, {}, tokens, UsageScope::Local});
    }
    snapshot.local_daily.clear();
    for (const auto& [date, tokens] : daily_tokens_) {
        snapshot.local_daily.push_back({0, date, tokens, UsageScope::Local});
    }
}

} // namespace cqt

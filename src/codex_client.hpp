#pragma once

#include "json.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace cqt {

struct RpcResult {
    bool ok = false;
    json::Value value;
    std::string error;
};

class CodexClient {
public:
    using NotificationCallback = std::function<void(std::string_view)>;

    CodexClient() = default;
    ~CodexClient();
    CodexClient(const CodexClient&) = delete;
    CodexClient& operator=(const CodexClient&) = delete;

    [[nodiscard]] bool start(const std::wstring& executable, std::string& error);
    void stop();
    [[nodiscard]] bool running() const;

    [[nodiscard]] RpcResult request(std::string method, std::optional<json::Value> params = std::nullopt,
                                    std::chrono::milliseconds timeout = std::chrono::seconds(12));
    [[nodiscard]] bool notify(std::string method, std::optional<json::Value> params = std::nullopt);
    void set_notification_callback(NotificationCallback callback);

private:
    struct PendingRequest {
        std::mutex mutex;
        std::condition_variable condition;
        bool completed = false;
        RpcResult result;
    };

    [[nodiscard]] bool write_message(const json::Value& message);
    void reader_loop();
    void dispatch_line(std::string_view line);
    void fail_all(std::string error);

    HANDLE process_ = nullptr;
    HANDLE job_ = nullptr;
    HANDLE stdin_write_ = nullptr;
    HANDLE stdout_read_ = nullptr;
    std::thread reader_;
    std::atomic_bool stopping_{false};
    std::atomic<std::int64_t> next_id_{1};
    mutable std::mutex state_mutex_;
    std::mutex write_mutex_;
    std::map<std::int64_t, std::shared_ptr<PendingRequest>> pending_;
    NotificationCallback notification_callback_;
};

} // namespace cqt

#pragma once

#include "codex_client.hpp"
#include "models.hpp"
#include "settings.hpp"
#include "usage_history.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace cqt {

class MainWindow;
class TrayIcon;

class AppController {
public:
    explicit AppController(HINSTANCE instance);
    ~AppController();
    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    [[nodiscard]] bool initialize(bool background, std::wstring& error);
    int run();
    void shutdown();

    [[nodiscard]] HINSTANCE instance() const { return instance_; }
    [[nodiscard]] Settings settings_copy() const;
    [[nodiscard]] UsageSnapshot snapshot_copy() const;
    [[nodiscard]] bool startup_enabled() const { return settings_store_.startup_enabled(); }
    [[nodiscard]] std::wstring data_directory() const { return settings_store_.directory().wstring(); }

    void show_main();
    void hide_main();
    void toggle_main();
    void request_refresh(bool include_usage = true);
    void exit_application();
    void on_snapshot_message();
    void on_taskbar_created();
    void on_tray_message(LPARAM event);
    void handle_action(std::string_view action, double value = 0.0);

    static constexpr UINT kSnapshotMessage = WM_APP + 40;
    static constexpr UINT kShowMessage = WM_APP + 41;

private:
    void worker_loop();
    void publish_snapshot(const UsageSnapshot& snapshot, bool persist);
    void save_and_apply(Settings settings, bool restart_client = false);
    void choose_codex_executable();
    void choose_custom_color(bool primary);
    [[nodiscard]] bool refresh_account(UsageSnapshot& snapshot);
    [[nodiscard]] bool refresh_quota(UsageSnapshot& snapshot);
    void refresh_usage(UsageSnapshot& snapshot);

    HINSTANCE instance_ = nullptr;
    SettingsStore settings_store_;
    mutable std::mutex model_mutex_;
    Settings settings_;
    UsageSnapshot snapshot_;
    std::unique_ptr<MainWindow> window_;
    std::unique_ptr<TrayIcon> tray_;
    CodexClient codex_;
    UsageHistory history_;
    std::thread worker_;
    std::atomic_bool stopping_{false};
    std::condition_variable worker_condition_;
    std::mutex worker_mutex_;
    bool refresh_requested_ = false;
    bool usage_requested_ = false;
    bool restart_client_ = false;
    std::chrono::steady_clock::time_point last_manual_refresh_{};
};

} // namespace cqt

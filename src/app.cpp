#include "app.hpp"

#include "tray.hpp"
#include "ui.hpp"
#include "usage_parser.hpp"

#include <Windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cwchar>

namespace cqt {
namespace {

std::string health_error(const RpcResult& result, std::string fallback) {
    return result.error.empty() ? std::move(fallback) : result.error;
}

} // namespace

AppController::AppController(HINSTANCE instance)
    : instance_(instance), history_() {}

AppController::~AppController() {
    shutdown();
}

bool AppController::initialize(bool background, std::wstring& error) {
    settings_ = settings_store_.load_settings();
    snapshot_ = settings_store_.load_cache();
    if (snapshot_.updated_at == 0) {
        snapshot_.health = AppHealth::Starting;
        snapshot_.status_detail = "Starting Codex app-server";
    }

    window_ = std::make_unique<MainWindow>();
    if (!window_->create(this, error)) return false;
    tray_ = std::make_unique<TrayIcon>();
    if (!tray_->create(this, window_->hwnd(), error)) return false;
    tray_->update(snapshot_, settings_);

    codex_.set_notification_callback([this](std::string_view method) {
        if (method == "account/rateLimits/updated" || method == "account/updated") {
            {
                std::lock_guard lock(worker_mutex_);
                refresh_requested_ = true;
                if (method == "account/updated") usage_requested_ = true;
            }
            worker_condition_.notify_one();
        }
    });

    stopping_.store(false);
    worker_ = std::thread(&AppController::worker_loop, this);
    if (!background) window_->show();
    return true;
}

int AppController::run() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

void AppController::shutdown() {
    if (stopping_.exchange(true)) return;
    worker_condition_.notify_all();
    history_.cancel();
    codex_.stop();
    if (worker_.joinable()) worker_.join();
    if (tray_) tray_->destroy();
    tray_.reset();
    window_.reset();
}

Settings AppController::settings_copy() const {
    std::lock_guard lock(model_mutex_);
    return settings_;
}

UsageSnapshot AppController::snapshot_copy() const {
    std::lock_guard lock(model_mutex_);
    return snapshot_;
}

void AppController::show_main() {
    if (window_) window_->show();
    {
        std::lock_guard lock(worker_mutex_);
        usage_requested_ = true;
    }
    worker_condition_.notify_one();
}

void AppController::hide_main() {
    if (window_) window_->hide();
}

void AppController::toggle_main() {
    if (!window_) return;
    if (window_->visible()) window_->hide();
    else show_main();
}

void AppController::request_refresh(bool include_usage) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_manual_refresh_ < std::chrono::seconds(3)) return;
    last_manual_refresh_ = now;
    {
        std::lock_guard lock(worker_mutex_);
        refresh_requested_ = true;
        usage_requested_ = usage_requested_ || include_usage;
    }
    worker_condition_.notify_one();
}

void AppController::exit_application() {
    hide_main();
    PostQuitMessage(0);
}

void AppController::on_snapshot_message() {
    const auto snapshot = snapshot_copy();
    const auto settings = settings_copy();
    if (window_) window_->invalidate();
    if (tray_) tray_->update(snapshot, settings);
}

void AppController::on_taskbar_created() {
    if (tray_) tray_->on_taskbar_created();
}

void AppController::on_tray_message(LPARAM event) {
    if (tray_) tray_->on_notify(event);
}

void AppController::handle_action(std::string_view action, double value) {
    Settings settings = settings_copy();
    bool changed = false;
    bool restart = false;

    if (action == "refresh") {
        request_refresh(true);
        return;
    }
    if (action == "exit") {
        exit_application();
        return;
    }
    if (action == "settings") {
        window_->set_page(Page::Settings);
        return;
    }
    if (action == "more") {
        window_->set_page(Page::More);
        return;
    }
    if (action == "home") {
        window_->set_page(Page::Home);
        return;
    }
    if (action == "back_settings") {
        window_->set_page(Page::Settings);
        return;
    }
    if (action == "toggle_capsule") {
        settings.capsule_enabled = !settings.capsule_enabled;
        changed = true;
    } else if (action == "toggle_glass") {
        settings.soft_glass = !settings.soft_glass;
        changed = true;
    } else if (action == "toggle_startup") {
        settings_store_.set_startup_enabled(!settings_store_.startup_enabled());
        window_->invalidate();
        return;
    } else if (action == "reset") {
        const auto saved_path = settings.codex_executable;
        settings = Settings{};
        settings.codex_executable = saved_path;
        changed = true;
    } else if (action == "choose_codex") {
        choose_codex_executable();
        return;
    } else if (action == "custom_primary") {
        choose_custom_color(true);
        return;
    } else if (action == "custom_secondary") {
        choose_custom_color(false);
        return;
    } else if (action == "open_data") {
        ShellExecuteW(nullptr, L"open", settings_store_.directory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    } else if (action.starts_with("theme")) {
        settings.theme = static_cast<ThemeMode>(std::clamp(static_cast<int>(value), 0, 2));
        changed = true;
    } else if (action.starts_with("palette")) {
        settings.palette = std::clamp(static_cast<int>(value), 0, 4);
        changed = true;
    } else if (action.starts_with("language")) {
        settings.language = static_cast<LanguageMode>(std::clamp(static_cast<int>(value), 0, 2));
        changed = true;
    } else if (action.starts_with("metric")) {
        settings.taskbar_metric = static_cast<TaskbarMetric>(std::clamp(static_cast<int>(value), 0, 2));
        changed = true;
    } else if (action.starts_with("range")) {
        settings.chart_range = static_cast<ChartRange>(std::clamp(static_cast<int>(value), 0, 2));
        changed = true;
    } else if (action == "window_scale") {
        settings.window_scale = std::clamp(value, 0.8, 1.4);
        changed = true;
    } else if (action == "font_scale") {
        settings.font_scale = std::clamp(value, 0.8, 1.4);
        changed = true;
    } else if (action == "taskbar_scale") {
        settings.taskbar_scale = std::clamp(value, 0.8, 1.4);
        changed = true;
    } else if (action == "glass_opacity") {
        settings.glass_opacity = std::clamp(value, 0.2, 1.0);
        changed = true;
    }

    if (changed) save_and_apply(std::move(settings), restart);
}

void AppController::worker_loop() {
    using clock = std::chrono::steady_clock;
    auto next_quota = clock::now();
    auto next_usage = clock::now();
    auto retry_delay = std::chrono::seconds(2);
    UsageSnapshot working = snapshot_copy();

    while (!stopping_.load()) {
        bool restart = false;
        {
            std::lock_guard lock(worker_mutex_);
            restart = restart_client_;
            restart_client_ = false;
        }
        if (restart) codex_.stop();

        if (!codex_.running()) {
            std::string error;
            const auto settings = settings_copy();
            if (!codex_.start(settings.codex_executable, error)) {
                working.health = working.updated_at > 0 ? AppHealth::Stale : AppHealth::Unavailable;
                working.status_detail = std::move(error);
                publish_snapshot(working, false);
                std::unique_lock wait_lock(worker_mutex_);
                worker_condition_.wait_for(wait_lock, retry_delay, [&] { return stopping_.load() || restart_client_ || refresh_requested_; });
                refresh_requested_ = false;
                usage_requested_ = false;
                retry_delay = std::min(retry_delay * 2, std::chrono::seconds(300));
                continue;
            }
            retry_delay = std::chrono::seconds(2);
            refresh_account(working);
            next_quota = clock::now();
            next_usage = clock::now();
        }

        bool forced_quota = false;
        bool forced_usage = false;
        {
            std::lock_guard lock(worker_mutex_);
            forced_quota = refresh_requested_;
            forced_usage = usage_requested_;
            refresh_requested_ = false;
            usage_requested_ = false;
        }

        const auto now = clock::now();
        bool changed = false;
        bool persist = false;
        if (forced_quota || now >= next_quota) {
            changed = refresh_quota(working) || changed;
            next_quota = clock::now() + std::chrono::seconds(60);
        }
        if (forced_usage || now >= next_usage) {
            refresh_account(working);
            refresh_usage(working);
            history_.refresh(working);
            changed = true;
            persist = true;
            next_usage = clock::now() + std::chrono::minutes(5);
        }
        if (changed) publish_snapshot(working, persist);

        const auto wake_at = std::min(next_quota, next_usage);
        std::unique_lock wait_lock(worker_mutex_);
        worker_condition_.wait_until(wait_lock, wake_at, [&] {
            return stopping_.load() || refresh_requested_ || usage_requested_ || restart_client_;
        });
    }
}

void AppController::publish_snapshot(const UsageSnapshot& snapshot, bool persist) {
    {
        std::lock_guard lock(model_mutex_);
        snapshot_ = snapshot;
    }
    if (persist && snapshot.updated_at > 0) settings_store_.save_cache(snapshot);
    if (window_ && window_->hwnd()) PostMessageW(window_->hwnd(), kSnapshotMessage, 0, 0);
}

void AppController::save_and_apply(Settings settings, bool restart_client) {
    {
        std::lock_guard lock(model_mutex_);
        settings_ = std::move(settings);
    }
    settings_store_.save_settings(settings_copy());
    if (window_) window_->apply_settings();
    if (tray_) tray_->update(snapshot_copy(), settings_copy());
    if (restart_client) {
        {
            std::lock_guard lock(worker_mutex_);
            restart_client_ = true;
        }
        worker_condition_.notify_one();
    }
}

void AppController::choose_codex_executable() {
    std::array<wchar_t, 32768> path{};
    Settings settings = settings_copy();
    if (!settings.codex_executable.empty()) wcsncpy_s(path.data(), path.size(), settings.codex_executable.c_str(), _TRUNCATE);
    const wchar_t filter[] = L"Codex executable (codex.exe)\0codex.exe\0Executable files (*.exe)\0*.exe\0All files (*.*)\0*.*\0\0";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_->hwnd();
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    window_->set_modal(true);
    const BOOL selected = GetOpenFileNameW(&dialog);
    window_->set_modal(false);
    if (!selected) return;
    settings.codex_executable = path.data();
    save_and_apply(std::move(settings), true);
}

void AppController::choose_custom_color(bool primary) {
    Settings settings = settings_copy();
    const std::uint32_t argb = primary ? settings.custom_primary : settings.custom_secondary;
    COLORREF color = RGB((argb >> 16) & 0xff, (argb >> 8) & 0xff, argb & 0xff);
    static std::array<COLORREF, 16> custom{};
    CHOOSECOLORW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_->hwnd();
    dialog.rgbResult = color;
    dialog.lpCustColors = custom.data();
    dialog.Flags = CC_FULLOPEN | CC_RGBINIT;
    window_->set_modal(true);
    const BOOL selected = ChooseColorW(&dialog);
    window_->set_modal(false);
    if (!selected) return;
    const std::uint32_t selected_argb = 0xff000000u | (GetRValue(dialog.rgbResult) << 16) |
                                        (GetGValue(dialog.rgbResult) << 8) | GetBValue(dialog.rgbResult);
    if (primary) settings.custom_primary = selected_argb;
    else settings.custom_secondary = selected_argb;
    settings.palette = 4;
    save_and_apply(std::move(settings), false);
}

bool AppController::refresh_account(UsageSnapshot& snapshot) {
    json::Value::Object params;
    params["refreshToken"] = false;
    const auto response = codex_.request("account/read", params);
    if (!response.ok) {
        snapshot.status_detail = health_error(response, "Unable to read Codex account");
        snapshot.health = snapshot.updated_at > 0 ? AppHealth::Stale : AppHealth::Unavailable;
        return false;
    }
    const auto status = parse_account(response.value, snapshot);
    return status.ok;
}

bool AppController::refresh_quota(UsageSnapshot& snapshot) {
    if (!snapshot.chatgpt_account) return false;
    const auto response = codex_.request("account/rateLimits/read");
    if (!response.ok) {
        snapshot.status_detail = health_error(response, "Unable to read Codex quota");
        snapshot.health = snapshot.updated_at > 0 ? AppHealth::Stale : AppHealth::Unavailable;
        return true;
    }
    const auto status = parse_rate_limits(response.value, snapshot);
    if (!status.ok) {
        snapshot.status_detail = status.error;
        snapshot.health = AppHealth::Stale;
    }
    return true;
}

void AppController::refresh_usage(UsageSnapshot& snapshot) {
    if (!snapshot.chatgpt_account) return;
    const auto response = codex_.request("account/usage/read");
    if (response.ok) parse_account_usage(response.value, snapshot);
}

} // namespace cqt

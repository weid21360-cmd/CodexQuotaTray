#pragma once

#include "models.hpp"

#include <Windows.h>
#include <shellapi.h>

#include <chrono>
#include <string>
#include <vector>

namespace cqt {

class AppController;

class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon();

    [[nodiscard]] bool create(AppController* controller, HWND owner, std::wstring& error);
    void destroy();
    void update(const UsageSnapshot& snapshot, const Settings& settings);
    void on_notify(LPARAM event);
    void on_taskbar_created();
    void reposition_capsule();

    static constexpr UINT kNotifyMessage = WM_APP + 42;

private:
    struct TokenBurst {
        std::int64_t tokens = 0;
        std::chrono::steady_clock::time_point started{};
        float horizontal_offset = 0.0f;
    };

    static LRESULT CALLBACK capsule_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK token_burst_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    static void CALLBACK foreground_event_proc(HWINEVENTHOOK hook, DWORD event, HWND window,
                                                LONG object_id, LONG child_id,
                                                DWORD event_thread, DWORD event_time);
    LRESULT handle_capsule_message(UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_token_burst_message(UINT message, WPARAM wparam, LPARAM lparam);
    void add_icon();
    void show_menu();
    void show_capsule(bool visible);
    void paint_capsule();
    void start_token_burst(std::int64_t tokens);
    void reposition_token_burst();
    void hide_token_burst(bool clear);
    void paint_token_burst();
    [[nodiscard]] HICON create_percentage_icon(int percentage, COLORREF color) const;
    [[nodiscard]] std::wstring tooltip() const;

    AppController* controller_ = nullptr;
    HWND owner_ = nullptr;
    HWND capsule_ = nullptr;
    HWND token_burst_ = nullptr;
    NOTIFYICONDATAW data_{};
    HICON dynamic_icon_ = nullptr;
    HWINEVENTHOOK foreground_hook_ = nullptr;
    ULONG_PTR gdiplus_token_ = 0;
    UsageSnapshot snapshot_;
    Settings settings_;
    bool capsule_desired_visible_ = false;
    std::chrono::steady_clock::time_point live_pulse_until_{};
    std::vector<TokenBurst> token_bursts_;
    std::uint64_t token_burst_sequence_ = 0;
};

} // namespace cqt

#pragma once

#include "models.hpp"

#include <Windows.h>
#include <shellapi.h>

#include <string>

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
    static LRESULT CALLBACK capsule_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_capsule_message(UINT message, WPARAM wparam, LPARAM lparam);
    void add_icon();
    void show_menu();
    void show_capsule(bool visible);
    void paint_capsule();
    [[nodiscard]] HICON create_percentage_icon(int percentage, COLORREF color) const;
    [[nodiscard]] std::wstring tooltip() const;

    AppController* controller_ = nullptr;
    HWND owner_ = nullptr;
    HWND capsule_ = nullptr;
    NOTIFYICONDATAW data_{};
    HICON dynamic_icon_ = nullptr;
    UsageSnapshot snapshot_;
    Settings settings_;
    bool capsule_desired_visible_ = false;
};

} // namespace cqt

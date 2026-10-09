#include "tray.hpp"

#include "app.hpp"
#include "settings.hpp"
#include "usage_parser.hpp"

#include <Windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shobjidl_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <cwchar>
#include <iterator>

namespace cqt {
namespace {

constexpr UINT kIconId = 1;
constexpr UINT kMenuOpen = 2001;
constexpr UINT kMenuRefresh = 2002;
constexpr UINT kMenuCapsule = 2003;
constexpr UINT kMenuStartup = 2004;
constexpr UINT kMenuExit = 2005;
constexpr UINT_PTR kCapsuleRestoreTimer = 1;
constexpr UINT_PTR kCapsuleLivePulseTimer = 2;
constexpr UINT_PTR kTokenBurstAnimationTimer = 1;
constexpr UINT kCapsuleEnsureZOrder = WM_APP + 43;
constexpr auto kTokenBurstLifetime = std::chrono::milliseconds(1350);

TrayIcon* g_tray_instance = nullptr;

bool use_english(const Settings& settings) {
    if (settings.language == LanguageMode::English) return true;
    if (settings.language == LanguageMode::Chinese) return false;
    return PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_CHINESE;
}

COLORREF palette_color(const Settings& settings) {
    std::uint32_t argb = 0xff917dddu;
    switch (settings.palette) {
    case 0: argb = 0xff34c7dfu; break;
    case 1: argb = 0xff917dddu; break;
    case 2: argb = 0xff52ba91u; break;
    case 3: argb = 0xffeb9b4cu; break;
    default: argb = settings.custom_primary; break;
    }
    return RGB((argb >> 16) & 0xff, (argb >> 8) & 0xff, argb & 0xff);
}

COLORREF quota_color(double remaining, const Settings& settings) {
    if (remaining < 20.0) return RGB(235, 78, 88);
    if (remaining < 50.0) return RGB(232, 166, 66);
    return palette_color(settings);
}

COLORREF status_color(const UsageSnapshot& snapshot, const Settings& settings) {
    if (snapshot.health == AppHealth::LoginRequired || snapshot.health == AppHealth::Unavailable) return RGB(235, 78, 88);
    if (snapshot.health == AppHealth::Cached || snapshot.health == AppHealth::Stale) return RGB(232, 166, 66);
    return palette_color(settings);
}

COLORREF blend_color(COLORREF a, COLORREF b, int amount) {
    amount = std::clamp(amount, 0, 255);
    const auto channel = [amount](int first, int second) {
        return (first * (255 - amount) + second * amount) / 255;
    };
    return RGB(channel(GetRValue(a), GetRValue(b)), channel(GetGValue(a), GetGValue(b)),
               channel(GetBValue(a), GetBValue(b)));
}

Gdiplus::Color gdiplus_color(COLORREF value, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(value), GetGValue(value), GetBValue(value));
}

std::wstring compact_tokens(std::int64_t value) {
    const double number = static_cast<double>(std::max<std::int64_t>(0, value));
    wchar_t buffer[24]{};
    if (number >= 1'000'000'000.0) swprintf_s(buffer, L"%.1fB", number / 1'000'000'000.0);
    else if (number >= 1'000'000.0) swprintf_s(buffer, L"%.1fM", number / 1'000'000.0);
    else if (number >= 1'000.0) swprintf_s(buffer, L"%.1fK", number / 1'000.0);
    else swprintf_s(buffer, L"%lld", static_cast<long long>(number));
    std::wstring result = buffer;
    if (const auto position = result.find(L".0"); position != std::wstring::npos) result.erase(position, 2);
    return result;
}

float ease_out_cubic(float value) {
    const float inverse = 1.0f - std::clamp(value, 0.0f, 1.0f);
    return 1.0f - inverse * inverse * inverse;
}

float ease_out_back(float value) {
    constexpr float c1 = 1.70158f;
    constexpr float c3 = c1 + 1.0f;
    const float shifted = std::clamp(value, 0.0f, 1.0f) - 1.0f;
    return 1.0f + c3 * shifted * shifted * shifted + c1 * shifted * shifted;
}

void add_rounded_rectangle(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rectangle, float radius) {
    const float diameter = std::min(radius * 2.0f, std::min(rectangle.Width, rectangle.Height));
    if (diameter <= 0.0f) {
        path.AddRectangle(rectangle);
        return;
    }
    path.AddArc(rectangle.X, rectangle.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rectangle.GetRight() - diameter, rectangle.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(rectangle.GetRight() - diameter, rectangle.GetBottom() - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rectangle.X, rectangle.GetBottom() - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

std::wstring quota_label(const QuotaWindow& quota, bool english) {
    switch (quota.kind) {
    case QuotaKind::ShortTerm: return english ? L"Short" : L"短期";
    case QuotaKind::Weekly: return english ? L"Weekly" : L"周额度";
    case QuotaKind::Monthly: return english ? L"Monthly" : L"月额度";
    default: return std::to_wstring(quota.duration_minutes) + (english ? L" min" : L" 分钟");
    }
}

HWND find_descendant(HWND parent, const wchar_t* class_name) {
    HWND direct = FindWindowExW(parent, nullptr, class_name, nullptr);
    if (direct) return direct;
    HWND child = nullptr;
    while ((child = FindWindowExW(parent, child, nullptr, nullptr)) != nullptr) {
        if (HWND found = find_descendant(child, class_name)) return found;
    }
    return nullptr;
}

} // namespace

TrayIcon::~TrayIcon() {
    destroy();
}

bool TrayIcon::create(AppController* controller, HWND owner, std::wstring& error) {
    controller_ = controller;
    owner_ = owner;
    Gdiplus::GdiplusStartupInput gdiplus_input;
    if (Gdiplus::GdiplusStartup(&gdiplus_token_, &gdiplus_input, nullptr) != Gdiplus::Ok) {
        error = L"无法初始化任务栏图形引擎。";
        return false;
    }
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = controller_->instance();
    window_class.lpfnWndProc = capsule_proc;
    window_class.lpszClassName = L"CodexQuotaTray.Capsule";
    window_class.hCursor = LoadCursorW(nullptr, IDC_HAND);
    RegisterClassExW(&window_class);
    capsule_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_LAYERED,
                               window_class.lpszClassName, L"Codex quota", WS_POPUP,
                               0, 0, 96, 38, nullptr, nullptr, controller_->instance(), this);
    if (!capsule_) {
        error = L"无法创建任务栏数字条。";
        Gdiplus::GdiplusShutdown(gdiplus_token_);
        gdiplus_token_ = 0;
        return false;
    }

    WNDCLASSEXW burst_class{};
    burst_class.cbSize = sizeof(burst_class);
    burst_class.hInstance = controller_->instance();
    burst_class.lpfnWndProc = token_burst_proc;
    burst_class.lpszClassName = L"CodexQuotaTray.TokenBurst";
    RegisterClassExW(&burst_class);
    token_burst_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST |
                                       WS_EX_LAYERED | WS_EX_TRANSPARENT,
                                   burst_class.lpszClassName, L"Token activity", WS_POPUP,
                                   0, 0, 190, 86, nullptr, nullptr, controller_->instance(), this);
    if (!token_burst_) {
        error = L"无法创建 Token 动态数字浮层。";
        DestroyWindow(capsule_);
        capsule_ = nullptr;
        Gdiplus::GdiplusShutdown(gdiplus_token_);
        gdiplus_token_ = 0;
        return false;
    }
    g_tray_instance = this;
    foreground_hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                       foreground_event_proc, 0, 0,
                                       WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    data_.cbSize = sizeof(data_);
    data_.hWnd = owner_;
    data_.uID = kIconId;
    data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data_.uCallbackMessage = kNotifyMessage;
    data_.uVersion = NOTIFYICON_VERSION_4;
    dynamic_icon_ = create_percentage_icon(-1, RGB(145, 125, 221));
    data_.hIcon = dynamic_icon_;
    wcscpy_s(data_.szTip, L"CodexQuotaTray · Connecting");
    if (!Shell_NotifyIconW(NIM_ADD, &data_)) {
        error = L"无法添加系统托盘图标。";
        if (foreground_hook_) {
            UnhookWinEvent(foreground_hook_);
            foreground_hook_ = nullptr;
        }
        if (g_tray_instance == this) g_tray_instance = nullptr;
        return false;
    }
    Shell_NotifyIconW(NIM_SETVERSION, &data_);
    return true;
}

void TrayIcon::destroy() {
    if (foreground_hook_) {
        UnhookWinEvent(foreground_hook_);
        foreground_hook_ = nullptr;
    }
    if (g_tray_instance == this) g_tray_instance = nullptr;
    if (data_.hWnd) {
        Shell_NotifyIconW(NIM_DELETE, &data_);
        data_.hWnd = nullptr;
    }
    if (dynamic_icon_) {
        DestroyIcon(dynamic_icon_);
        dynamic_icon_ = nullptr;
    }
    if (token_burst_) {
        DestroyWindow(token_burst_);
        token_burst_ = nullptr;
    }
    if (capsule_) {
        DestroyWindow(capsule_);
        capsule_ = nullptr;
    }
    if (gdiplus_token_) {
        Gdiplus::GdiplusShutdown(gdiplus_token_);
        gdiplus_token_ = 0;
    }
}

void TrayIcon::update(const UsageSnapshot& snapshot, const Settings& settings) {
    const bool new_live_activity = snapshot.session_tokens > snapshot_.session_tokens ||
                                   snapshot.live_token_updated_at > snapshot_.live_token_updated_at;
    snapshot_ = snapshot;
    settings_ = settings;
    if (new_live_activity && snapshot_.live_token_delta > 0) {
        live_pulse_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    }
    const QuotaWindow* quota = select_taskbar_quota(snapshot_, settings_.taskbar_metric);
    const int percentage = quota ? static_cast<int>(std::round(quota->remaining_percent)) : -1;
    const COLORREF icon_color = quota && snapshot_.health == AppHealth::Healthy
        ? quota_color(quota->remaining_percent, settings_) : status_color(snapshot_, settings_);
    HICON replacement = create_percentage_icon(percentage, icon_color);
    if (replacement) {
        HICON old = dynamic_icon_;
        dynamic_icon_ = replacement;
        data_.hIcon = dynamic_icon_;
        const std::wstring tip = tooltip();
        wcsncpy_s(data_.szTip, tip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &data_);
        if (old) DestroyIcon(old);
    }
    reposition_capsule();
    if (capsule_ && capsule_desired_visible_) {
        if (new_live_activity) SetTimer(capsule_, kCapsuleLivePulseTimer, 80, nullptr);
        paint_capsule();
        if (new_live_activity && snapshot_.live_token_delta > 0) {
            start_token_burst(snapshot_.live_token_delta);
        }
    }
}

void TrayIcon::on_notify(LPARAM event) {
    const UINT message = LOWORD(event);
    if (message == WM_LBUTTONUP || message == NIN_SELECT || message == NIN_KEYSELECT) {
        controller_->toggle_main();
    } else if (message == WM_RBUTTONUP || message == WM_CONTEXTMENU) {
        show_menu();
    }
}

void TrayIcon::on_taskbar_created() {
    add_icon();
    reposition_capsule();
}

void TrayIcon::reposition_capsule() {
    if (!capsule_) return;
    bool should_show = settings_.capsule_enabled;
    APPBARDATA appbar{};
    appbar.cbSize = sizeof(appbar);
    if ((SHAppBarMessage(ABM_GETSTATE, &appbar) & ABS_AUTOHIDE) != 0) should_show = false;
    QUERY_USER_NOTIFICATION_STATE state = QUNS_NOT_PRESENT;
    if (SUCCEEDED(SHQueryUserNotificationState(&state)) &&
        (state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE)) should_show = false;

    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar || !IsWindowVisible(taskbar)) should_show = false;
    HWND notification = taskbar ? find_descendant(taskbar, L"TrayNotifyWnd") : nullptr;
    RECT taskbar_rect{};
    RECT notification_rect{};
    if (!taskbar || !GetWindowRect(taskbar, &taskbar_rect)) should_show = false;
    if (!notification || !GetWindowRect(notification, &notification_rect)) should_show = false;
    if (!should_show) {
        capsule_desired_visible_ = false;
        show_capsule(false);
        hide_token_burst(true);
        return;
    }

    const float scale = static_cast<float>(settings_.taskbar_scale) * static_cast<float>(GetDpiForWindow(taskbar)) / 96.0f;
    const int width = static_cast<int>(std::round(112 * scale));
    const int height = static_cast<int>(std::round(38 * scale));
    const bool horizontal = (taskbar_rect.right - taskbar_rect.left) > (taskbar_rect.bottom - taskbar_rect.top);
    int x = 0;
    int y = 0;
    if (horizontal) {
        x = notification_rect.left - width - 4;
        y = taskbar_rect.top + ((taskbar_rect.bottom - taskbar_rect.top) - height) / 2;
        if (x < taskbar_rect.left || x + width > taskbar_rect.right) should_show = false;
    } else {
        x = taskbar_rect.left + ((taskbar_rect.right - taskbar_rect.left) - width) / 2;
        y = notification_rect.top - height - 4;
        if (y < taskbar_rect.top || y + height > taskbar_rect.bottom) should_show = false;
    }
    if (!should_show) {
        capsule_desired_visible_ = false;
        show_capsule(false);
        hide_token_burst(true);
        return;
    }
    // Tie the top-level capsule to Explorer's taskbar. This keeps it out of Show Desktop/minimize
    // transitions while still avoiding any injection or modification inside Explorer.
    if (GetWindow(capsule_, GW_OWNER) != taskbar) {
        SetWindowLongPtrW(capsule_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(taskbar));
    }
    capsule_desired_visible_ = true;
    if (!SetWindowPos(capsule_, HWND_TOPMOST, x, y, width, height,
                      SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER)) {
        capsule_desired_visible_ = false;
        show_capsule(false);
        hide_token_burst(true);
        return;
    }
    // Per-pixel alpha supplies the smooth silhouette; a hard HRGN produces visible stair-stepping.
    SetWindowRgn(capsule_, nullptr, FALSE);
    show_capsule(true);
    if (token_burst_ && GetWindow(token_burst_, GW_OWNER) != taskbar) {
        SetWindowLongPtrW(token_burst_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(taskbar));
    }
    reposition_token_burst();
    if (std::chrono::steady_clock::now() < live_pulse_until_) {
        SetTimer(capsule_, kCapsuleLivePulseTimer, 80, nullptr);
    }
    paint_capsule();
}

LRESULT CALLBACK TrayIcon::capsule_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    TrayIcon* self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<TrayIcon*>(create->lpCreateParams);
        self->capsule_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handle_capsule_message(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK TrayIcon::token_burst_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    TrayIcon* self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<TrayIcon*>(create->lpCreateParams);
        self->token_burst_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handle_token_burst_message(message, wparam, lparam)
                : DefWindowProcW(window, message, wparam, lparam);
}

void CALLBACK TrayIcon::foreground_event_proc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    if (g_tray_instance && g_tray_instance->capsule_) {
        PostMessageW(g_tray_instance->capsule_, kCapsuleEnsureZOrder, 0, 0);
    }
}

LRESULT TrayIcon::handle_capsule_message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_PAINT:
        paint_capsule();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONUP:
        controller_->toggle_main();
        return 0;
    case WM_RBUTTONUP:
        show_menu();
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SHOWWINDOW:
        // Explorer can temporarily cloak or hide adjacent top-level windows when its taskbar is
        // activated. Re-check placement shortly afterwards, but only when the capsule is desired.
        if (!wparam && capsule_desired_visible_) SetTimer(capsule_, kCapsuleRestoreTimer, 180, nullptr);
        return DefWindowProcW(capsule_, message, wparam, lparam);
    case WM_TIMER:
        if (wparam == kCapsuleRestoreTimer) {
            KillTimer(capsule_, kCapsuleRestoreTimer);
            reposition_capsule();
            return 0;
        }
        if (wparam == kCapsuleLivePulseTimer) {
            if (std::chrono::steady_clock::now() >= live_pulse_until_) {
                KillTimer(capsule_, kCapsuleLivePulseTimer);
            }
            paint_capsule();
            return 0;
        }
        return DefWindowProcW(capsule_, message, wparam, lparam);
    case kCapsuleEnsureZOrder:
        if (capsule_desired_visible_) {
            SetWindowPos(capsule_, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
            paint_capsule();
            if (token_burst_ && IsWindowVisible(token_burst_)) {
                SetWindowPos(token_burst_, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                                 SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
            }
        }
        return 0;
    default:
        return DefWindowProcW(capsule_, message, wparam, lparam);
    }
}

LRESULT TrayIcon::handle_token_burst_message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_PAINT:
        paint_token_burst();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_TIMER:
        if (wparam == kTokenBurstAnimationTimer) {
            const auto now = std::chrono::steady_clock::now();
            std::erase_if(token_bursts_, [&](const TokenBurst& burst) {
                return now - burst.started >= kTokenBurstLifetime;
            });
            if (token_bursts_.empty()) {
                hide_token_burst(false);
            } else {
                paint_token_burst();
            }
            return 0;
        }
        return DefWindowProcW(token_burst_, message, wparam, lparam);
    default:
        return DefWindowProcW(token_burst_, message, wparam, lparam);
    }
}

void TrayIcon::add_icon() {
    Shell_NotifyIconW(NIM_ADD, &data_);
    Shell_NotifyIconW(NIM_SETVERSION, &data_);
}

void TrayIcon::show_menu() {
    const bool english = use_english(settings_);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuOpen, english ? L"Open" : L"打开面板");
    AppendMenuW(menu, MF_STRING, kMenuRefresh, english ? L"Refresh" : L"立即刷新");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (settings_.capsule_enabled ? MF_CHECKED : 0), kMenuCapsule,
                english ? L"Taskbar capsule" : L"任务栏数字条");
    AppendMenuW(menu, MF_STRING | (controller_->startup_enabled() ? MF_CHECKED : 0), kMenuStartup,
                english ? L"Start with Windows" : L"随 Windows 启动");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, english ? L"Exit" : L"退出");
    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(owner_);
    const UINT selection = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                          cursor.x, cursor.y, 0, owner_, nullptr);
    PostMessageW(owner_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    switch (selection) {
    case kMenuOpen: controller_->show_main(); break;
    case kMenuRefresh: controller_->request_refresh(true); break;
    case kMenuCapsule: controller_->handle_action("toggle_capsule"); break;
    case kMenuStartup: controller_->handle_action("toggle_startup"); break;
    case kMenuExit: controller_->exit_application(); break;
    default: break;
    }
}

void TrayIcon::show_capsule(bool visible) {
    if (visible) {
        if (!IsWindowVisible(capsule_)) ShowWindow(capsule_, SW_SHOWNOACTIVATE);
        SetWindowPos(capsule_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    } else if (IsWindowVisible(capsule_)) {
        ShowWindow(capsule_, SW_HIDE);
    }
}

void TrayIcon::start_token_burst(std::int64_t tokens) {
    if (!token_burst_ || !capsule_desired_visible_ || tokens <= 0) return;
    static constexpr std::array<float, 6> offsets{-15.0f, 12.0f, -5.0f, 18.0f, -10.0f, 7.0f};
    TokenBurst burst;
    burst.tokens = tokens;
    burst.started = std::chrono::steady_clock::now();
    burst.horizontal_offset = offsets[token_burst_sequence_++ % offsets.size()];
    token_bursts_.push_back(burst);
    if (token_bursts_.size() > 4) token_bursts_.erase(token_bursts_.begin());
    reposition_token_burst();
    SetTimer(token_burst_, kTokenBurstAnimationTimer, 20, nullptr);
    paint_token_burst();
}

void TrayIcon::reposition_token_burst() {
    if (!token_burst_ || !capsule_) return;
    if (!capsule_desired_visible_ || token_bursts_.empty()) {
        if (IsWindowVisible(token_burst_)) ShowWindow(token_burst_, SW_HIDE);
        return;
    }

    RECT capsule_rect{};
    if (!GetWindowRect(capsule_, &capsule_rect)) return;
    const float scale = static_cast<float>(settings_.taskbar_scale) *
                        static_cast<float>(GetDpiForWindow(capsule_)) / 96.0f;
    const int width = static_cast<int>(std::round(190.0f * scale));
    const int height = static_cast<int>(std::round(86.0f * scale));
    int x = capsule_rect.left + ((capsule_rect.right - capsule_rect.left) - width) / 2;
    int y = capsule_rect.top - height - static_cast<int>(std::round(3.0f * scale));

    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    const HMONITOR monitor = MonitorFromRect(&capsule_rect, MONITOR_DEFAULTTONEAREST);
    if (GetMonitorInfoW(monitor, &monitor_info)) {
        x = std::clamp(x, static_cast<int>(monitor_info.rcMonitor.left) + 2,
                       static_cast<int>(monitor_info.rcMonitor.right) - width - 2);
        if (y < monitor_info.rcMonitor.top + 2) {
            y = capsule_rect.bottom + static_cast<int>(std::round(3.0f * scale));
        }
    }
    SetWindowPos(token_burst_, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
}

void TrayIcon::hide_token_burst(bool clear) {
    if (!token_burst_) return;
    KillTimer(token_burst_, kTokenBurstAnimationTimer);
    if (clear) token_bursts_.clear();
    if (IsWindowVisible(token_burst_)) ShowWindow(token_burst_, SW_HIDE);
}

void TrayIcon::paint_token_burst() {
    if (!token_burst_ || !gdiplus_token_ || token_bursts_.empty()) return;
    if (GetUpdateRect(token_burst_, nullptr, FALSE)) {
        PAINTSTRUCT paint{};
        BeginPaint(token_burst_, &paint);
        EndPaint(token_burst_, &paint);
    }

    RECT client{};
    GetClientRect(token_burst_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!memory || !bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
    const auto now = std::chrono::steady_clock::now();
    const COLORREF accent = palette_color(settings_);

    {
        Gdiplus::Bitmap surface(width, height, width * 4, PixelFormat32bppPARGB,
                                static_cast<BYTE*>(pixels));
        Gdiplus::Graphics graphics(&surface);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

        Gdiplus::FontFamily family(L"Segoe UI");
        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

        for (std::size_t index = 0; index < token_bursts_.size(); ++index) {
            const auto& burst = token_bursts_[index];
            const float elapsed = static_cast<float>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - burst.started).count());
            const float progress = std::clamp(elapsed / static_cast<float>(kTokenBurstLifetime.count()), 0.0f, 1.0f);
            const float pop = ease_out_back(std::min(1.0f, progress / 0.28f));
            const float scale = 0.38f + 0.62f * pop;
            const float rise = height * 0.26f * ease_out_cubic(progress);
            const float fade = progress < 0.58f ? 1.0f : std::max(0.0f, (1.0f - progress) / 0.42f);
            const BYTE alpha = static_cast<BYTE>(std::round(255.0f * fade));
            const float logical_scale = static_cast<float>(width) / 190.0f;
            const float font_size = 27.0f * logical_scale * scale;
            const float x_offset = burst.horizontal_offset * logical_scale;
            const float top = height * 0.39f - rise + static_cast<float>(index) * 1.5f;
            const std::wstring value = L"−" + compact_tokens(burst.tokens);

            Gdiplus::GraphicsPath path;
            const Gdiplus::RectF layout(x_offset, top, static_cast<float>(width), height * 0.48f);
            path.AddString(value.c_str(), -1, &family, Gdiplus::FontStyleBold,
                           font_size, layout, &format);

            Gdiplus::Pen shadow(Gdiplus::Color(static_cast<BYTE>(alpha * 0.78f), 10, 13, 19),
                                5.2f * logical_scale);
            shadow.SetLineJoin(Gdiplus::LineJoinRound);
            Gdiplus::Pen edge(gdiplus_color(accent, alpha), 2.1f * logical_scale);
            edge.SetLineJoin(Gdiplus::LineJoinRound);
            Gdiplus::SolidBrush face(Gdiplus::Color(alpha, 252, 253, 255));
            graphics.DrawPath(&shadow, &path);
            graphics.DrawPath(&edge, &path);
            graphics.FillPath(&face, &path);

            if (progress < 0.42f) {
                const float spark_progress = progress / 0.42f;
                const float spark_distance = (8.0f + 18.0f * ease_out_cubic(spark_progress)) * logical_scale;
                const float center_x = width * 0.5f + x_offset;
                const float center_y = top + height * 0.22f;
                const BYTE spark_alpha = static_cast<BYTE>(alpha * (1.0f - spark_progress) * 0.72f);
                Gdiplus::SolidBrush spark(gdiplus_color(accent, spark_alpha));
                for (int ray = 0; ray < 5; ++ray) {
                    const float angle = -2.75f + static_cast<float>(ray) * 0.61f;
                    const float radius = (ray % 2 == 0 ? 2.0f : 1.4f) * logical_scale;
                    const float px = center_x + std::cos(angle) * spark_distance;
                    const float py = center_y + std::sin(angle) * spark_distance * 0.58f;
                    graphics.FillEllipse(&spark, px - radius, py - radius, radius * 2.0f, radius * 2.0f);
                }
            }
        }
    }

    RECT window_rect{};
    GetWindowRect(token_burst_, &window_rect);
    POINT destination{window_rect.left, window_rect.top};
    POINT source{0, 0};
    SIZE size{width, height};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(token_burst_, screen, &destination, &size, memory, &source, 0, &blend, ULW_ALPHA);

    SelectObject(memory, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

void TrayIcon::paint_capsule() {
    if (!capsule_ || !gdiplus_token_) return;
    if (GetUpdateRect(capsule_, nullptr, FALSE)) {
        PAINTSTRUCT paint{};
        BeginPaint(capsule_, &paint);
        EndPaint(capsule_, &paint);
    }

    RECT client{};
    GetClientRect(capsule_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!memory || !bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ old_bitmap = SelectObject(memory, bitmap);

    const QuotaWindow* quota = select_taskbar_quota(snapshot_, settings_.taskbar_metric);
    const int percentage = quota ? static_cast<int>(std::round(quota->remaining_percent)) : -1;
    const COLORREF accent = quota && snapshot_.health == AppHealth::Healthy
        ? quota_color(quota->remaining_percent, settings_) : status_color(snapshot_, settings_);
    const bool live_pulse = snapshot_.live_token_delta > 0 &&
                            std::chrono::steady_clock::now() < live_pulse_until_;
    const auto pulse_milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const float pulse = live_pulse
        ? 0.55f + 0.45f * static_cast<float>(std::sin(static_cast<double>(pulse_milliseconds) / 135.0))
        : 0.0f;
    const COLORREF background = RGB(24, 30, 40);
    const COLORREF border = blend_color(RGB(96, 106, 126), accent, live_pulse ? 150 : 86);

    {
        Gdiplus::Bitmap surface(width, height, width * 4, PixelFormat32bppPARGB,
                                static_cast<BYTE*>(pixels));
        Gdiplus::Graphics graphics(&surface);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

        Gdiplus::GraphicsPath body;
        const Gdiplus::RectF body_rect(1.5f, 1.5f, static_cast<float>(width) - 3.0f,
                                      static_cast<float>(height) - 3.0f);
        add_rounded_rectangle(body, body_rect, body_rect.Height / 2.0f);
        Gdiplus::SolidBrush body_brush(gdiplus_color(background, 252));
        Gdiplus::Pen body_pen(gdiplus_color(border), 1.25f + (live_pulse ? pulse * 0.8f : 0.0f));
        graphics.FillPath(&body_brush, &body);
        graphics.DrawPath(&body_pen, &body);

        if (live_pulse) {
            Gdiplus::GraphicsPath glow;
            const Gdiplus::RectF glow_rect(3.5f, 3.5f, static_cast<float>(width) - 7.0f,
                                           static_cast<float>(height) - 7.0f);
            add_rounded_rectangle(glow, glow_rect, glow_rect.Height / 2.0f);
            Gdiplus::Pen glow_pen(gdiplus_color(accent, static_cast<BYTE>(60 + pulse * 70)), 1.2f);
            graphics.DrawPath(&glow_pen, &glow);
        }

        // Soft inner highlight separates the capsule from dark taskbars without a black halo.
        Gdiplus::GraphicsPath highlight;
        const Gdiplus::RectF highlight_rect(2.8f, 2.8f, static_cast<float>(width) - 5.6f,
                                           static_cast<float>(height) - 5.6f);
        add_rounded_rectangle(highlight, highlight_rect, highlight_rect.Height / 2.0f);
        Gdiplus::Pen highlight_pen(Gdiplus::Color(42, 255, 255, 255), 0.75f);
        graphics.DrawPath(&highlight_pen, &highlight);

        const float diameter = std::max(18.0f, static_cast<float>(height) - 12.0f);
        const Gdiplus::RectF progress_rect(7.0f, (height - diameter) / 2.0f, diameter, diameter);
        Gdiplus::Pen track_pen(Gdiplus::Color(255, 56, 64, 78), 3.0f);
        graphics.DrawEllipse(&track_pen, progress_rect);
        Gdiplus::Pen progress_pen(gdiplus_color(accent), 3.0f + (live_pulse ? pulse * 0.8f : 0.0f));
        progress_pen.SetStartCap(Gdiplus::LineCapRound);
        progress_pen.SetEndCap(Gdiplus::LineCapRound);
        if (percentage < 0) graphics.DrawEllipse(&progress_pen, progress_rect);
        else if (percentage > 0) graphics.DrawArc(&progress_pen, progress_rect, -90.0f,
                                                   std::min(359.9f, static_cast<float>(percentage) * 3.6f));

        const float core = std::max(4.0f, diameter / 5.0f) + (live_pulse ? pulse * 2.5f : 0.0f);
        Gdiplus::SolidBrush core_brush(gdiplus_color(blend_color(background, accent, 112), 245));
        graphics.FillEllipse(&core_brush, progress_rect.X + (diameter - core) / 2.0f,
                             progress_rect.Y + (diameter - core) / 2.0f, core, core);

        const float separator_x = progress_rect.GetRight() + 8.0f;
        Gdiplus::Pen separator_pen(Gdiplus::Color(190, 54, 63, 78), 1.0f);
        graphics.DrawLine(&separator_pen, separator_x, 9.0f, separator_x, static_cast<float>(height) - 9.0f);

        const std::wstring value = live_pulse
            ? L"+" + compact_tokens(snapshot_.live_token_delta)
            : (percentage >= 0 ? std::to_wstring(percentage) + L"%" : L"--");
        Gdiplus::Font value_font(L"Segoe UI", std::max(10.0f, height * 0.34f),
                                 Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font label_font(L"Segoe UI", std::max(6.2f, height * 0.17f),
                                 Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush value_brush(Gdiplus::Color(255, 248, 249, 252));
        Gdiplus::SolidBrush label_brush(gdiplus_color(accent));
        Gdiplus::StringFormat value_format;
        value_format.SetAlignment(Gdiplus::StringAlignmentCenter);
        value_format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        value_format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        Gdiplus::RectF value_layout(separator_x + 5.0f, 1.0f,
                                    width - separator_x - 12.0f, height * 0.62f);
        graphics.DrawString(value.c_str(), -1, &value_font, value_layout, &value_format, &value_brush);
        Gdiplus::RectF label_layout(separator_x + 5.0f, height * 0.53f,
                                    width - separator_x - 12.0f, height * 0.34f);
        graphics.DrawString(live_pulse ? L"TOKEN" : L"CODEX", -1, &label_font,
                            label_layout, &value_format, &label_brush);
    }

    RECT window_rect{};
    GetWindowRect(capsule_, &window_rect);
    POINT destination{window_rect.left, window_rect.top};
    POINT source{0, 0};
    SIZE size{width, height};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(capsule_, screen, &destination, &size, memory, &source, 0, &blend, ULW_ALPHA);

    SelectObject(memory, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

HICON TrayIcon::create_percentage_icon(int percentage, COLORREF accent) const {
    constexpr int size = 32;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP color_bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    std::array<BYTE, size * size / 8> mask_bits{};
    HBITMAP mask_bitmap = CreateBitmap(size, size, 1, 1, mask_bits.data());
    ReleaseDC(nullptr, screen);
    if (!color_bitmap || !mask_bitmap) {
        if (color_bitmap) DeleteObject(color_bitmap);
        if (mask_bitmap) DeleteObject(mask_bitmap);
        DeleteDC(memory);
        return nullptr;
    }
    HGDIOBJ old = SelectObject(memory, color_bitmap);
    RECT rect{0, 0, size, size};
    HBRUSH background = CreateSolidBrush(RGB(28, 34, 42));
    FillRect(memory, &rect, background);
    DeleteObject(background);
    HPEN pen = CreatePen(PS_SOLID, 3, accent);
    HGDIOBJ old_pen = SelectObject(memory, pen);
    HGDIOBJ old_brush = SelectObject(memory, GetStockObject(HOLLOW_BRUSH));
    Ellipse(memory, 2, 2, 30, 30);
    SelectObject(memory, old_pen);
    SelectObject(memory, old_brush);
    DeleteObject(pen);
    SetBkMode(memory, TRANSPARENT);
    SetTextColor(memory, RGB(250, 250, 252));
    HFONT font = CreateFontW(-10, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             FF_DONTCARE, L"Arial Narrow");
    HGDIOBJ old_font = SelectObject(memory, font);
    const std::wstring text = percentage >= 0 ? std::to_wstring(percentage) : L"--";
    DrawTextW(memory, text.c_str(), -1, &rect, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    if (pixels) {
        auto* bytes = static_cast<BYTE*>(pixels);
        for (int i = 0; i < size * size; ++i) bytes[i * 4 + 3] = 0xff;
    }
    SelectObject(memory, old_font);
    DeleteObject(font);
    SelectObject(memory, old);
    DeleteDC(memory);

    ICONINFO icon_info{};
    icon_info.fIcon = TRUE;
    icon_info.hbmColor = color_bitmap;
    icon_info.hbmMask = mask_bitmap;
    HICON icon = CreateIconIndirect(&icon_info);
    DeleteObject(color_bitmap);
    DeleteObject(mask_bitmap);
    return icon;
}

std::wstring TrayIcon::tooltip() const {
    const bool english = use_english(settings_);
    std::wstring result = L"Codex";
    if (snapshot_.quota_windows.empty()) {
        result += english ? L" · quota unavailable" : L" · 暂无额度数据";
        return result;
    }
    for (const auto& quota : snapshot_.quota_windows) {
        result += L"\n" + quota_label(quota, english) + L" " + std::to_wstring(static_cast<int>(std::round(quota.remaining_percent))) + L"%";
        if (quota.resets_at > 0) {
            result += english ? L" · reset " : L" · 重置 ";
            const std::time_t time = static_cast<std::time_t>(quota.resets_at);
            std::tm local{};
            if (localtime_s(&local, &time) == 0) {
                wchar_t buffer[32]{};
                std::wcsftime(buffer, std::size(buffer), L"%m/%d %H:%M", &local);
                result += buffer;
            } else result += L"--";
        }
    }
    if (snapshot_.session_tokens > 0) {
        result += english ? L"\nLive session " : L"\n本次实时 ";
        result += compact_tokens(snapshot_.session_tokens) + L" tokens";
    }
    if (result.size() >= std::size(data_.szTip)) result.resize(std::size(data_.szTip) - 1);
    return result;
}

} // namespace cqt

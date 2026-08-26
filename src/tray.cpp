#include "tray.hpp"

#include "app.hpp"
#include "settings.hpp"
#include "usage_parser.hpp"

#include <Windows.h>
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
        return false;
    }
    SetLayeredWindowAttributes(capsule_, 0, 245, LWA_ALPHA);

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
        return false;
    }
    Shell_NotifyIconW(NIM_SETVERSION, &data_);
    return true;
}

void TrayIcon::destroy() {
    if (data_.hWnd) {
        Shell_NotifyIconW(NIM_DELETE, &data_);
        data_.hWnd = nullptr;
    }
    if (dynamic_icon_) {
        DestroyIcon(dynamic_icon_);
        dynamic_icon_ = nullptr;
    }
    if (capsule_) {
        DestroyWindow(capsule_);
        capsule_ = nullptr;
    }
}

void TrayIcon::update(const UsageSnapshot& snapshot, const Settings& settings) {
    snapshot_ = snapshot;
    settings_ = settings;
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
    if (capsule_) InvalidateRect(capsule_, nullptr, FALSE);
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
        show_capsule(false);
        return;
    }

    const float scale = static_cast<float>(settings_.taskbar_scale) * static_cast<float>(GetDpiForWindow(taskbar)) / 96.0f;
    const int width = static_cast<int>(std::round(96 * scale));
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
        show_capsule(false);
        return;
    }
    if (!SetWindowPos(capsule_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
        show_capsule(false);
        return;
    }
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, height / 2, height / 2);
    SetWindowRgn(capsule_, region, TRUE);
    show_capsule(true);
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
    default:
        return DefWindowProcW(capsule_, message, wparam, lparam);
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
    ShowWindow(capsule_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void TrayIcon::paint_capsule() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(capsule_, &paint);
    RECT client{};
    GetClientRect(capsule_, &client);
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
    HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
    const COLORREF background = RGB(28, 34, 42);
    HBRUSH background_brush = CreateSolidBrush(background);
    FillRect(memory, &client, background_brush);
    DeleteObject(background_brush);

    const QuotaWindow* quota = select_taskbar_quota(snapshot_, settings_.taskbar_metric);
    const int percentage = quota ? static_cast<int>(std::round(quota->remaining_percent)) : -1;
    const COLORREF accent = quota && snapshot_.health == AppHealth::Healthy
        ? quota_color(quota->remaining_percent, settings_) : status_color(snapshot_, settings_);
    const int diameter = std::max(18, client.bottom - 10);
    const int circle_left = 5;
    const int circle_top = (client.bottom - diameter) / 2;
    HPEN pen = CreatePen(PS_SOLID, 3, accent);
    HGDIOBJ old_pen = SelectObject(memory, pen);
    HGDIOBJ old_brush = SelectObject(memory, GetStockObject(HOLLOW_BRUSH));
    Ellipse(memory, circle_left, circle_top, circle_left + diameter, circle_top + diameter);
    SelectObject(memory, old_pen);
    SelectObject(memory, old_brush);
    DeleteObject(pen);

    SetBkMode(memory, TRANSPARENT);
    SetTextColor(memory, RGB(248, 249, 252));
    HFONT value_font = CreateFontW(-static_cast<int>(13 * settings_.taskbar_scale), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   FF_DONTCARE, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(memory, value_font);
    RECT value_rect{diameter + 9, 2, client.right - 4, client.bottom / 2 + 6};
    const std::wstring value = percentage >= 0 ? std::to_wstring(percentage) + L"%" : L"--";
    DrawTextW(memory, value.c_str(), -1, &value_rect, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(memory, old_font);
    DeleteObject(value_font);

    SetTextColor(memory, accent);
    HFONT label_font = CreateFontW(-static_cast<int>(8 * settings_.taskbar_scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   FF_DONTCARE, L"Segoe UI");
    old_font = SelectObject(memory, label_font);
    RECT label_rect{diameter + 9, client.bottom / 2, client.right - 4, client.bottom - 1};
    DrawTextW(memory, L"CODEX", -1, &label_rect, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(memory, old_font);
    DeleteObject(label_font);

    BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(capsule_, &paint);
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
    if (result.size() >= std::size(data_.szTip)) result.resize(std::size(data_.szTip) - 1);
    return result;
}

} // namespace cqt

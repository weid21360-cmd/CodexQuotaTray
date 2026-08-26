#include "ui.hpp"

#include "app.hpp"
#include "settings.hpp"
#include "tray.hpp"

#include <Windows.h>
#include <windowsx.h>
#include <d2d1helper.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <iterator>
#include <numeric>
#include <sstream>

namespace cqt {
namespace {

constexpr float kBaseWidth = 436.0f;
constexpr float kBaseHeight = 650.0f;

D2D1_COLOR_F from_argb(std::uint32_t color, float alpha_multiplier = 1.0f) {
    return D2D1::ColorF(((color >> 16) & 0xff) / 255.0f, ((color >> 8) & 0xff) / 255.0f,
                        (color & 0xff) / 255.0f, ((color >> 24) & 0xff) / 255.0f * alpha_multiplier);
}

D2D1_COLOR_F color(float r, float g, float b, float a = 1.0f) {
    return D2D1::ColorF(r, g, b, a);
}

D2D1_COLOR_F mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return color(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
                 a.a + (b.a - a.a) * t);
}

std::pair<D2D1_COLOR_F, D2D1_COLOR_F> palette_colors(const Settings& settings) {
    switch (settings.palette) {
    case 0: return {color(0.20f, 0.78f, 0.88f), color(0.19f, 0.61f, 0.88f)};
    case 1: return {color(0.57f, 0.49f, 0.86f), color(0.73f, 0.45f, 0.76f)};
    case 2: return {color(0.32f, 0.73f, 0.57f), color(0.35f, 0.59f, 0.75f)};
    case 3: return {color(0.92f, 0.61f, 0.30f), color(0.77f, 0.44f, 0.53f)};
    default: return {from_argb(settings.custom_primary), from_argb(settings.custom_secondary)};
    }
}

std::wstring percent_text(double percent) {
    return std::to_wstring(static_cast<int>(std::round(clamp_percent(percent)))) + L"%";
}

std::wstring compact_tokens(std::int64_t value) {
    const double number = static_cast<double>(std::max<std::int64_t>(0, value));
    std::wostringstream output;
    output.setf(std::ios::fixed);
    if (number >= 1'000'000'000.0) output << std::setprecision(number >= 10'000'000'000.0 ? 0 : 1) << number / 1'000'000'000.0 << L"B";
    else if (number >= 1'000'000.0) output << std::setprecision(number >= 10'000'000.0 ? 0 : 1) << number / 1'000'000.0 << L"M";
    else if (number >= 1'000.0) output << std::setprecision(number >= 10'000.0 ? 0 : 1) << number / 1'000.0 << L"K";
    else output << static_cast<std::int64_t>(number);
    return output.str();
}

std::wstring local_time_text(std::int64_t epoch, bool date) {
    if (epoch <= 0) return L"--";
    const std::time_t time = static_cast<std::time_t>(epoch);
    std::tm local{};
    if (localtime_s(&local, &time) != 0) return L"--";
    wchar_t buffer[64]{};
    std::wcsftime(buffer, std::size(buffer), date ? L"%m/%d %H:%M" : L"%H:%M", &local);
    return buffer;
}

std::wstring remaining_time(std::int64_t resets_at, bool english) {
    const auto seconds = std::max<std::int64_t>(0, resets_at - unix_now());
    const auto days = seconds / 86400;
    const auto hours = (seconds % 86400) / 3600;
    const auto minutes = (seconds % 3600) / 60;
    if (english) {
        if (days > 0) return std::to_wstring(days) + L"d " + std::to_wstring(hours) + L"h";
        return std::to_wstring(hours) + L"h " + std::to_wstring(minutes) + L"m";
    }
    if (days > 0) return std::to_wstring(days) + L"天 " + std::to_wstring(hours) + L"小时";
    return std::to_wstring(hours) + L"小时 " + std::to_wstring(minutes) + L"分钟";
}

std::wstring plan_label(const std::string& plan) {
    if (plan.empty()) return L"ChatGPT";
    std::wstring wide = SettingsStore::utf8_to_wide(plan);
    if (!wide.empty()) wide[0] = static_cast<wchar_t>(towupper(wide[0]));
    return L"ChatGPT " + wide;
}

std::vector<std::int64_t> chart_values(const UsageSnapshot& snapshot, ChartRange range) {
    if (range == ChartRange::Hours24) {
        std::vector<std::int64_t> values(24, 0);
        const std::int64_t current_hour = unix_now() / 3600 * 3600;
        for (const auto& bucket : snapshot.local_hourly) {
            const auto offset = static_cast<int>((bucket.start_epoch - (current_hour - 23 * 3600)) / 3600);
            if (offset >= 0 && offset < 24) values[static_cast<std::size_t>(offset)] += bucket.tokens;
        }
        return values;
    }
    const std::size_t count = range == ChartRange::Days7 ? 7 : 30;
    const auto& source = snapshot.account_daily.empty() ? snapshot.local_daily : snapshot.account_daily;
    std::vector<std::int64_t> values;
    const std::size_t start = source.size() > count ? source.size() - count : 0;
    for (std::size_t i = start; i < source.size(); ++i) values.push_back(source[i].tokens);
    if (values.size() < count) values.insert(values.begin(), count - values.size(), 0);
    return values;
}

} // namespace

MainWindow::~MainWindow() {
    discard_device_resources();
    if (hwnd_) DestroyWindow(hwnd_);
}

bool MainWindow::create(AppController* controller, std::wstring& error) {
    controller_ = controller;
    taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = controller_->instance();
    window_class.lpfnWndProc = window_proc;
    window_class.lpszClassName = L"CodexQuotaTray.MainWindow";
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    window_class.hIconSm = window_class.hIcon;
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&window_class);

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_factory_.ReleaseAndGetAddressOf())) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(write_factory_.ReleaseAndGetAddressOf())))) {
        error = L"无法初始化 Direct2D/DirectWrite。";
        return false;
    }

    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED, window_class.lpszClassName, L"CodexQuotaTray",
                            WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, 436, 650, nullptr, nullptr,
                            controller_->instance(), this);
    if (!hwnd_) {
        error = L"无法创建主窗口，Windows 错误：" + std::to_wstring(GetLastError());
        return false;
    }
    dpi_ = GetDpiForWindow(hwnd_);
    apply_settings();
    return true;
}

void MainWindow::show() {
    position_near_taskbar();
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    SetTimer(hwnd_, 1, 1000, nullptr);
    invalidate();
}

void MainWindow::hide() {
    KillTimer(hwnd_, 1);
    ShowWindow(hwnd_, SW_HIDE);
    discard_device_resources();
}

bool MainWindow::visible() const {
    return hwnd_ && IsWindowVisible(hwnd_) != FALSE;
}

void MainWindow::invalidate() {
    if (hwnd_ && visible()) InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::apply_settings() {
    if (!hwnd_) return;
    const auto settings = controller_->settings_copy();
    font_scale_ = static_cast<float>(settings.font_scale);
    const float scale = static_cast<float>(settings.window_scale) * dpi_ / 96.0f;
    SetWindowPos(hwnd_, nullptr, 0, 0, static_cast<int>(std::round(kBaseWidth * scale)),
                 static_cast<int>(std::round(kBaseHeight * scale)), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    const BYTE opacity = settings.soft_glass ? static_cast<BYTE>(std::round(settings.glass_opacity * 255.0)) : 255;
    SetLayeredWindowAttributes(hwnd_, 0, opacity, LWA_ALPHA);
    const BOOL dark_mode = dark(settings) ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, 20, &dark_mode, sizeof(dark_mode));
    const int corner = 2;
    DwmSetWindowAttribute(hwnd_, 33, &corner, sizeof(corner));
    const int backdrop = settings.soft_glass ? 2 : 1;
    DwmSetWindowAttribute(hwnd_, 38, &backdrop, sizeof(backdrop));
    update_shape();
    discard_device_resources();
    if (visible()) position_near_taskbar();
    invalidate();
}

void MainWindow::set_page(Page page) {
    page_ = page;
    invalidate();
}

LRESULT MainWindow::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == taskbar_created_message_) {
        controller_->on_taskbar_created();
        return 0;
    }
    if (message == AppController::kSnapshotMessage) {
        controller_->on_snapshot_message();
        return 0;
    }
    if (message == AppController::kShowMessage) {
        show();
        return 0;
    }
    if (message == TrayIcon::kNotifyMessage) {
        controller_->on_tray_message(lparam);
        return 0;
    }

    switch (message) {
    case WM_PAINT:
        paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONUP: {
        const auto settings = controller_->settings_copy();
        const float scale = static_cast<float>(settings.window_scale) * dpi_ / 96.0f;
        dispatch_click(GET_X_LPARAM(lparam) / scale, GET_Y_LPARAM(lparam) / scale);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const auto settings = controller_->settings_copy();
        const float scale = static_cast<float>(settings.window_scale) * dpi_ / 96.0f;
        const float x = GET_X_LPARAM(lparam) / scale;
        const float y = GET_Y_LPARAM(lparam) / scale;
        const bool hand = std::any_of(hits_.begin(), hits_.end(), [=](const HitTarget& hit) {
            return x >= hit.rect.left && x <= hit.rect.right && y >= hit.rect.top && y <= hit.rect.bottom;
        });
        SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_NCHITTEST: {
        const LRESULT original = DefWindowProcW(hwnd_, message, wparam, lparam);
        if (original != HTCLIENT) return original;
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd_, &point);
        const auto settings = controller_->settings_copy();
        const float scale = static_cast<float>(settings.window_scale) * dpi_ / 96.0f;
        const float x = point.x / scale;
        const float y = point.y / scale;
        if (y < 62.0f && x > 58.0f && x < 350.0f) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE && !modal_) hide();
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) hide();
        return 0;
    case WM_TIMER:
        if (visible()) invalidate();
        return 0;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wparam);
        const RECT* proposed = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(hwnd_, nullptr, proposed->left, proposed->top, proposed->right - proposed->left,
                     proposed->bottom - proposed->top, SWP_NOZORDER | SWP_NOACTIVATE);
        discard_device_resources();
        update_shape();
        return 0;
    }
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        position_near_taskbar();
        controller_->on_snapshot_message();
        return 0;
    case WM_CLOSE:
        hide();
        return 0;
    case WM_SIZE:
        if (render_target_) render_target_->Resize(D2D1::SizeU(LOWORD(lparam), HIWORD(lparam)));
        return 0;
    default:
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }
}

LRESULT CALLBACK MainWindow::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        self->hwnd_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handle_message(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

void MainWindow::create_device_resources() {
    if (render_target_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const auto size = D2D1::SizeU(static_cast<UINT32>(std::max<LONG>(1, client.right)), static_cast<UINT32>(std::max<LONG>(1, client.bottom)));
    d2d_factory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(), D2D1::HwndRenderTargetProperties(hwnd_, size), render_target_.ReleaseAndGetAddressOf());
    if (render_target_) {
        render_target_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
        render_target_->CreateSolidColorBrush(color(1, 1, 1), brush_.ReleaseAndGetAddressOf());
    }
}

void MainWindow::discard_device_resources() {
    brush_.Reset();
    render_target_.Reset();
}

void MainWindow::create_text_formats() {}

void MainWindow::paint() {
    PAINTSTRUCT paint_structure{};
    BeginPaint(hwnd_, &paint_structure);
    create_device_resources();
    if (!render_target_ || !brush_) {
        EndPaint(hwnd_, &paint_structure);
        return;
    }

    const auto settings = controller_->settings_copy();
    const auto snapshot = controller_->snapshot_copy();
    font_scale_ = static_cast<float>(settings.font_scale);
    const bool is_dark = dark(settings);
    const auto background = is_dark ? color(0.075f, 0.092f, 0.112f) : color(0.95f, 0.96f, 0.975f);
    render_target_->BeginDraw();
    render_target_->SetTransform(D2D1::Matrix3x2F::Scale(static_cast<float>(settings.window_scale), static_cast<float>(settings.window_scale)));
    render_target_->Clear(background);
    rounded_rect({0, 0, kBaseWidth, kBaseHeight}, 18.0f, background,
                 is_dark ? color(0.15f, 0.18f, 0.22f) : color(0.78f, 0.80f, 0.84f), 1.0f);
    hits_.clear();
    if (page_ == Page::Home) draw_home(snapshot, settings);
    else if (page_ == Page::Settings) draw_settings(settings);
    else draw_more(settings);
    const HRESULT result = render_target_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) discard_device_resources();
    EndPaint(hwnd_, &paint_structure);
}

void MainWindow::draw_home(const UsageSnapshot& snapshot, const Settings& settings) {
    const bool is_dark = dark(settings);
    const bool is_english = english(settings);
    const auto [primary, secondary] = palette_colors(settings);
    const auto strong = is_dark ? color(0.96f, 0.97f, 1.0f) : color(0.08f, 0.10f, 0.14f);
    const auto muted = is_dark ? color(0.61f, 0.68f, 0.76f) : color(0.35f, 0.40f, 0.48f);
    const auto divider = is_dark ? color(0.20f, 0.24f, 0.29f) : color(0.80f, 0.83f, 0.87f);
    const auto card = is_dark ? color(0.105f, 0.125f, 0.15f) : color(0.91f, 0.93f, 0.96f);

    text(L"Codex " + tr(settings, L"用量", L"Usage"), {25, 17, 300, 48}, 20, strong, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    text(plan_label(snapshot.plan_type), {25, 49, 300, 71}, 12, muted);
    text(L"↻", {376, 17, 414, 56}, 26, muted, DWRITE_TEXT_ALIGNMENT_CENTER);
    add_hit({370, 12, 418, 62}, "refresh");
    line(24, 85, 412, 85, divider);

    std::wstring health;
    D2D1_COLOR_F health_color = color(0.24f, 0.84f, 0.61f);
    switch (snapshot.health) {
    case AppHealth::Healthy: health = tr(settings, L"✓ 服务正常", L"✓ Healthy"); break;
    case AppHealth::Cached: health = tr(settings, L"缓存数据", L"Cached"); health_color = color(0.92f, 0.67f, 0.30f); break;
    case AppHealth::Stale: health = tr(settings, L"数据延迟", L"Delayed"); health_color = color(0.92f, 0.67f, 0.30f); break;
    case AppHealth::LoginRequired: health = tr(settings, L"需要登录", L"Sign-in required"); health_color = color(0.94f, 0.38f, 0.42f); break;
    case AppHealth::Unavailable: health = tr(settings, L"服务不可用", L"Unavailable"); health_color = color(0.94f, 0.38f, 0.42f); break;
    default: health = tr(settings, L"正在连接", L"Connecting"); health_color = muted; break;
    }
    text(health, {292, 99, 408, 123}, 12, health_color, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    text(L"Codex · " + plan_label(snapshot.plan_type), {25, 97, 292, 124}, 14, strong, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_SEMI_BOLD);

    float quota_y = 132.0f;
    const std::size_t quota_count = std::min<std::size_t>(snapshot.quota_windows.size(), 2);
    if (quota_count == 0) {
        rounded_rect({24, quota_y, 412, 211}, 10, card);
        text(snapshot.status_detail.empty() ? tr(settings, L"暂无额度数据", L"Quota data unavailable") : SettingsStore::utf8_to_wide(snapshot.status_detail),
             {40, 153, 396, 194}, 13, muted, DWRITE_TEXT_ALIGNMENT_CENTER);
        quota_y = 218;
    } else {
        for (std::size_t index = 0; index < quota_count; ++index) {
            const auto& quota = snapshot.quota_windows[index];
            std::wstring label;
            if (quota.kind == QuotaKind::ShortTerm) label = tr(settings, L"▣  短期剩余", L"▣  Short-term remaining");
            else if (quota.kind == QuotaKind::Weekly) label = tr(settings, L"◷  周额度剩余", L"◷  Weekly remaining");
            else if (quota.kind == QuotaKind::Monthly) label = tr(settings, L"◫  月额度剩余", L"◫  Monthly remaining");
            else label = std::to_wstring(quota.duration_minutes) + tr(settings, L" 分钟窗口", L" minute window");
            text(label, {26, quota_y, 315, quota_y + 24}, 13, strong);
            text(percent_text(quota.remaining_percent), {330, quota_y, 410, quota_y + 24}, 15, strong, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_FONT_WEIGHT_BOLD);
            rounded_rect({25, quota_y + 32, 411, quota_y + 43}, 5.5f, divider);
            const float width = 386.0f * static_cast<float>(quota.remaining_percent / 100.0);
            rounded_rect({25, quota_y + 32, 25 + width, quota_y + 43}, 5.5f, index == 0 ? primary : secondary);
            text(tr(settings, L"剩余 ", L"Resets in ") + remaining_time(quota.resets_at, is_english), {26, quota_y + 52, 246, quota_y + 73}, 11, muted);
            text(tr(settings, L"重置于 ", L"Reset ") + local_time_text(quota.resets_at, quota.kind != QuotaKind::ShortTerm), {235, quota_y + 52, 410, quota_y + 73}, 11, muted, DWRITE_TEXT_ALIGNMENT_TRAILING);
            quota_y += 89.0f;
        }
    }

    const float chart_top = quota_count >= 2 ? 315.0f : 246.0f;
    line(24, chart_top - 17, 412, chart_top - 17, divider);
    text(tr(settings, L"Token 活动", L"Token activity"), {25, chart_top, 220, chart_top + 30}, 17, strong, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const auto values = chart_values(snapshot, settings.chart_range);
    const std::int64_t range_total = std::accumulate(values.begin(), values.end(), std::int64_t{});
    const std::int64_t display_total = snapshot.lifetime_tokens > 0 ? snapshot.lifetime_tokens : range_total;
    text(compact_tokens(display_total), {315, chart_top - 2, 410, chart_top + 31}, 19, strong, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_FONT_WEIGHT_BOLD);

    const std::array<std::wstring, 3> ranges{L"24" + tr(settings, L"小时", L"h"), L"7" + tr(settings, L"天", L"d"), L"30" + tr(settings, L"天", L"d")};
    for (int i = 0; i < 3; ++i) {
        const float x = 25.0f + i * 64.0f;
        const bool selected = static_cast<int>(settings.chart_range) == i;
        if (selected) rounded_rect({x, chart_top + 37, x + 57, chart_top + 66}, 7, mix(card, primary, 0.12f), primary, 1.0f);
        text(ranges[static_cast<std::size_t>(i)], {x, chart_top + 40, x + 57, chart_top + 63}, 11, selected ? primary : muted, DWRITE_TEXT_ALIGNMENT_CENTER);
        add_hit({x, chart_top + 34, x + 57, chart_top + 69}, "range", i);
    }
    const bool account_scope = settings.chart_range != ChartRange::Hours24 && !snapshot.account_daily.empty();
    text(account_scope ? tr(settings, L"账户数据", L"Account") : tr(settings, L"本机数据", L"Local"), {300, chart_top + 41, 410, chart_top + 63}, 10, muted, DWRITE_TEXT_ALIGNMENT_TRAILING);

    const float graph_top = chart_top + 78.0f;
    const float graph_bottom = std::min(485.0f, graph_top + 102.0f);
    const auto maximum = std::max<std::int64_t>(1, *std::max_element(values.begin(), values.end()));
    const float gap = 3.0f;
    const float available = 387.0f;
    const float bar_width = std::max(3.0f, (available - gap * static_cast<float>(values.size() - 1)) / static_cast<float>(values.size()));
    for (std::size_t i = 0; i < values.size(); ++i) {
        const float ratio = static_cast<float>(values[i]) / static_cast<float>(maximum);
        const float height = values[i] == 0 ? 2.0f : std::max(4.0f, ratio * (graph_bottom - graph_top));
        const float x = 25.0f + i * (bar_width + gap);
        rounded_rect({x, graph_bottom - height, x + bar_width, graph_bottom}, std::min(4.0f, bar_width / 2), primary);
    }
    line(24, graph_bottom + 11, 412, graph_bottom + 11, divider);

    const float settings_y = graph_bottom + 28;
    text(L"⚙", {25, settings_y - 5, 55, settings_y + 30}, 25, muted, DWRITE_TEXT_ALIGNMENT_CENTER);
    text(tr(settings, L"设置", L"Settings"), {65, settings_y - 4, 310, settings_y + 20}, 15, strong);
    text(tr(settings, L"配色、语言、数字条与更多设置", L"Colors, language, taskbar and more"), {65, settings_y + 20, 356, settings_y + 43}, 11, muted);
    text(L"›", {380, settings_y, 411, settings_y + 31}, 24, muted, DWRITE_TEXT_ALIGNMENT_CENTER);
    add_hit({20, settings_y - 12, 416, settings_y + 52}, "settings");
    line(24, settings_y + 59, 412, settings_y + 59, divider);
    text(tr(settings, L"更新 ", L"Updated ") + local_time_text(snapshot.updated_at, false), {25, settings_y + 73, 240, settings_y + 96}, 11, muted);
    text(tr(settings, L"退出", L"Exit"), {350, settings_y + 70, 412, settings_y + 98}, 13, strong, DWRITE_TEXT_ALIGNMENT_TRAILING);
    add_hit({340, settings_y + 65, 418, settings_y + 103}, "exit");
}

void MainWindow::draw_settings(const Settings& settings) {
    const bool is_dark = dark(settings);
    const auto [primary, secondary] = palette_colors(settings);
    const auto strong = is_dark ? color(0.96f, 0.97f, 1.0f) : color(0.08f, 0.10f, 0.14f);
    const auto muted = is_dark ? color(0.61f, 0.68f, 0.76f) : color(0.35f, 0.40f, 0.48f);
    const auto divider = is_dark ? color(0.20f, 0.24f, 0.29f) : color(0.80f, 0.83f, 0.87f);
    const auto card = is_dark ? color(0.12f, 0.15f, 0.18f) : color(0.90f, 0.92f, 0.95f);

    text(L"‹", {22, 15, 53, 55}, 34, muted, DWRITE_TEXT_ALIGNMENT_CENTER);
    add_hit({14, 8, 58, 64}, "home");
    text(tr(settings, L"设置", L"Settings"), {66, 17, 260, 49}, 21, strong, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_BOLD);
    text(tr(settings, L"恢复默认", L"Reset"), {330, 20, 411, 48}, 11, primary, DWRITE_TEXT_ALIGNMENT_TRAILING);
    add_hit({320, 10, 418, 58}, "reset");
    text(tr(settings, L"更改会自动保存", L"Changes save automatically"), {66, 49, 280, 70}, 11, muted);
    line(24, 85, 412, 85, divider);

    auto toggle = [&](float y, bool on, std::wstring title, std::wstring subtitle, std::string action) {
        text(title, {25, y, 300, y + 22}, 14, strong, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        text(subtitle, {25, y + 24, 320, y + 44}, 10, muted);
        rounded_rect({364, y + 4, 412, y + 30}, 13, on ? primary : divider);
        circle(on ? 399.0f : 377.0f, y + 17, 9, color(0.96f, 0.97f, 1.0f));
        add_hit({350, y - 4, 418, y + 44}, std::move(action));
        line(24, y + 55, 412, y + 55, divider);
    };
    toggle(102, settings.capsule_enabled, tr(settings, L"任务栏数字条", L"Taskbar capsule"),
           tr(settings, L"显示完整剩余百分比", L"Show the full remaining percentage"), "toggle_capsule");
    toggle(166, settings.soft_glass, tr(settings, L"柔光玻璃", L"Soft glass"),
           tr(settings, L"透明度可在更多设置调整", L"Adjust opacity in More settings"), "toggle_glass");

    text(tr(settings, L"外观", L"Appearance"), {25, 229, 116, 252}, 14, strong, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const std::array<std::wstring, 3> themes{tr(settings, L"跟随", L"System"), tr(settings, L"深色", L"Dark"), tr(settings, L"浅色", L"Light")};
    for (int i = 0; i < 3; ++i) {
        const float x = 230.0f + i * 61.0f;
        const bool selected = static_cast<int>(settings.theme) == i;
        rounded_rect({x, 222, x + 58, 250}, 7, selected ? mix(card, primary, 0.13f) : card, selected ? primary : divider, 1.0f);
        text(themes[static_cast<std::size_t>(i)], {x, 226, x + 58, 246}, 10, selected ? primary : muted, DWRITE_TEXT_ALIGNMENT_CENTER);
        add_hit({x, 218, x + 58, 254}, "theme", i);
    }

    const std::array<std::pair<std::uint32_t, std::uint32_t>, 4> swatches{{
        {0xff34c7df, 0xff319ce0}, {0xff917ddd, 0xffba73c2}, {0xff52ba91, 0xff5997bf}, {0xffeb9b4c, 0xffc56f86}
    }};
    const std::array<std::wstring, 4> names{tr(settings, L"深海青", L"Deep sea"), tr(settings, L"极光紫", L"Aurora"), tr(settings, L"森林绿", L"Forest"), tr(settings, L"暮光橙", L"Sunset")};
    for (int i = 0; i < 4; ++i) {
        const float x = i % 2 == 0 ? 24.0f : 228.0f;
        const float y = i < 2 ? 260.0f : 312.0f;
        const bool selected = settings.palette == i;
        rounded_rect({x, y, x + 184, y + 45}, 10, card, selected ? primary : divider, selected ? 1.5f : 1.0f);
        circle(x + 24, y + 22, 10, from_argb(swatches[static_cast<std::size_t>(i)].first));
        circle(x + 36, y + 22, 10, from_argb(swatches[static_cast<std::size_t>(i)].second));
        text(names[static_cast<std::size_t>(i)], {x + 55, y + 12, x + 172, y + 34}, 11, selected ? primary : strong);
        add_hit({x, y, x + 184, y + 45}, "palette", i);
    }
    const bool custom_selected = settings.palette == 4;
    rounded_rect({24, 367, 412, 421}, 10, card, custom_selected ? primary : divider, custom_selected ? 1.5f : 1.0f);
    text(tr(settings, L"自定义配色", L"Custom colors"), {40, 376, 250, 398}, 12, strong, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    text(tr(settings, L"点击色块选择主色与辅色", L"Click swatches to choose colors"), {40, 398, 280, 416}, 9, muted);
    rounded_rect({327, 376, 357, 412}, 7, from_argb(settings.custom_primary), color(1, 1, 1, 0.35f), 1);
    rounded_rect({367, 376, 397, 412}, 7, from_argb(settings.custom_secondary), color(1, 1, 1, 0.35f), 1);
    add_hit({318, 367, 362, 421}, "custom_primary");
    add_hit({360, 367, 406, 421}, "custom_secondary");

    line(24, 434, 412, 434, divider);
    text(tr(settings, L"语言", L"Language"), {25, 448, 130, 475}, 14, strong, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const std::array<std::wstring, 3> languages{tr(settings, L"跟随", L"System"), L"中文", L"English"};
    for (int i = 0; i < 3; ++i) {
        const float x = 230.0f + i * 61.0f;
        const bool selected = static_cast<int>(settings.language) == i;
        rounded_rect({x, 441, x + 58, 476}, 7, selected ? mix(card, primary, 0.13f) : card, selected ? primary : divider, 1.0f);
        text(languages[static_cast<std::size_t>(i)], {x, 449, x + 58, 470}, 10, selected ? primary : muted, DWRITE_TEXT_ALIGNMENT_CENTER);
        add_hit({x, 438, x + 58, 479}, "language", i);
    }

    line(24, 488, 412, 488, divider);
    text(tr(settings, L"更多设置", L"More settings"), {25, 502, 300, 526}, 14, strong, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    text(tr(settings, L"尺寸、额度数字、数据目录与版本", L"Sizing, quota metric, data and version"), {25, 528, 360, 549}, 10, muted);
    text(L"›", {380, 505, 412, 539}, 23, muted, DWRITE_TEXT_ALIGNMENT_CENTER);
    add_hit({20, 492, 417, 554}, "more");
    line(24, 565, 412, 565, divider);
    text(tr(settings, L"返回首页", L"Home"), {24, 582, 160, 610}, 11, muted);
    add_hit({16, 570, 180, 620}, "home");
    text(tr(settings, L"退出", L"Exit"), {350, 580, 412, 610}, 13, strong, DWRITE_TEXT_ALIGNMENT_TRAILING);
    add_hit({335, 570, 418, 620}, "exit");
}

void MainWindow::draw_more(const Settings& settings) {
    const bool is_dark = dark(settings);
    const auto [primary, secondary] = palette_colors(settings);
    (void)secondary;
    const auto strong = is_dark ? color(0.96f, 0.97f, 1.0f) : color(0.08f, 0.10f, 0.14f);
    const auto muted = is_dark ? color(0.61f, 0.68f, 0.76f) : color(0.35f, 0.40f, 0.48f);
    const auto divider = is_dark ? color(0.20f, 0.24f, 0.29f) : color(0.80f, 0.83f, 0.87f);
    const auto card = is_dark ? color(0.12f, 0.15f, 0.18f) : color(0.90f, 0.92f, 0.95f);

    text(L"‹", {22, 15, 53, 55}, 34, muted, DWRITE_TEXT_ALIGNMENT_CENTER);
    add_hit({14, 8, 58, 64}, "back_settings");
    text(tr(settings, L"更多设置", L"More settings"), {66, 17, 300, 49}, 21, strong, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_BOLD);
    text(tr(settings, L"尺寸、显示与本机集成", L"Sizing, display and local integration"), {66, 49, 350, 70}, 11, muted);
    line(24, 85, 412, 85, divider);

    auto slider = [&](float y, std::wstring label, double current, double minimum, double maximum, std::string action, bool enabled = true) {
        text(label, {25, y, 180, y + 24}, 14, enabled ? strong : muted, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        const float start = 183.0f;
        const float end = 402.0f;
        const float ratio = static_cast<float>((current - minimum) / (maximum - minimum));
        const float knob = start + std::clamp(ratio, 0.0f, 1.0f) * (end - start);
        line(start, y + 32, end, y + 32, divider, 4);
        line(start, y + 32, knob, y + 32, enabled ? primary : muted, 4);
        for (int i = 0; i < 7; ++i) {
            const float x = start + i * (end - start) / 6.0f;
            circle(x, y + 32, 3.5f, i / 6.0f <= ratio ? (enabled ? primary : muted) : divider,
                   is_dark ? color(0.07f, 0.09f, 0.11f) : color(1, 1, 1), 1);
            const double selected = minimum + (maximum - minimum) * i / 6.0;
            if (enabled) add_hit({x - 12, y + 18, x + 12, y + 46}, action, selected);
        }
        rounded_rect({knob - 24, y - 2, knob + 24, y + 24}, 7, enabled ? primary : muted);
        text(std::to_wstring(static_cast<int>(std::round(current * 100))) + L"%", {knob - 24, y + 2, knob + 24, y + 21}, 10, color(1, 1, 1), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_FONT_WEIGHT_BOLD);
        line(24, y + 57, 412, y + 57, divider);
    };

    slider(105, tr(settings, L"悬浮窗", L"Window"), settings.window_scale, 0.8, 1.4, "window_scale");
    slider(174, tr(settings, L"字体", L"Font"), settings.font_scale, 0.8, 1.4, "font_scale");
    slider(243, tr(settings, L"任务栏数字", L"Taskbar digits"), settings.taskbar_scale, 0.8, 1.4, "taskbar_scale");
    slider(312, tr(settings, L"玻璃不透明度", L"Glass opacity"), settings.glass_opacity, 0.2, 1.0, "glass_opacity", settings.soft_glass);

    text(tr(settings, L"任务栏显示额度", L"Taskbar quota"), {25, 382, 180, 408}, 13, strong, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const std::array<std::wstring, 3> metrics{tr(settings, L"短期", L"Short"), tr(settings, L"周额度", L"Weekly"), tr(settings, L"最紧张", L"Lowest")};
    for (int i = 0; i < 3; ++i) {
        const float x = 210.0f + i * 67.0f;
        const bool selected = static_cast<int>(settings.taskbar_metric) == i;
        rounded_rect({x, 378, x + 63, 410}, 7, selected ? mix(card, primary, 0.13f) : card, selected ? primary : divider, 1);
        text(metrics[static_cast<std::size_t>(i)], {x, 385, x + 63, 405}, 9, selected ? primary : muted, DWRITE_TEXT_ALIGNMENT_CENTER);
        add_hit({x, 374, x + 63, 414}, "metric", i);
    }
    line(24, 425, 412, 425, divider);

    text(tr(settings, L"Codex CLI", L"Codex CLI"), {25, 439, 140, 463}, 13, strong, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const std::wstring path = settings.codex_executable.empty() ? tr(settings, L"自动检测 codex.exe", L"Auto-detect codex.exe") : settings.codex_executable;
    text(path, {25, 466, 305, 489}, 9, muted);
    rounded_rect({316, 441, 412, 482}, 8, card, primary, 1);
    text(tr(settings, L"选择路径", L"Browse"), {316, 452, 412, 475}, 10, primary, DWRITE_TEXT_ALIGNMENT_CENTER);
    add_hit({306, 433, 418, 490}, "choose_codex");
    line(24, 500, 412, 500, divider);

    text(tr(settings, L"版本与数据", L"Version and data"), {25, 514, 200, 540}, 14, strong, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    text(L"v" CQT_VERSION L" · Windows x64", {25, 543, 255, 566}, 10, primary);
    rounded_rect({268, 518, 412, 558}, 8, card, divider, 1);
    text(tr(settings, L"打开数据目录", L"Open data folder"), {268, 529, 412, 552}, 10, strong, DWRITE_TEXT_ALIGNMENT_CENTER);
    add_hit({258, 510, 418, 565}, "open_data");
    line(24, 578, 412, 578, divider);
    text(tr(settings, L"返回设置", L"Back to settings"), {24, 592, 180, 620}, 11, muted);
    add_hit({16, 582, 190, 630}, "back_settings");
    text(tr(settings, L"退出", L"Exit"), {350, 590, 412, 620}, 13, strong, DWRITE_TEXT_ALIGNMENT_TRAILING);
    add_hit({335, 582, 418, 630}, "exit");
}

void MainWindow::position_near_taskbar() {
    if (!hwnd_) return;
    RECT window{};
    GetWindowRect(hwnd_, &window);
    const int width = window.right - window.left;
    const int height = window.bottom - window.top;
    APPBARDATA appbar{};
    appbar.cbSize = sizeof(appbar);
    RECT taskbar{};
    UINT edge = ABE_BOTTOM;
    if (SHAppBarMessage(ABM_GETTASKBARPOS, &appbar)) {
        taskbar = appbar.rc;
        edge = appbar.uEdge;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &taskbar, 0);
        taskbar.top = taskbar.bottom;
    }
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromRect(&taskbar, MONITOR_DEFAULTTOPRIMARY), &monitor);
    int x = monitor.rcWork.right - width - 8;
    int y = monitor.rcWork.bottom - height - 8;
    if (edge == ABE_TOP) y = taskbar.bottom + 8;
    else if (edge == ABE_LEFT) x = taskbar.right + 8;
    else if (edge == ABE_RIGHT) x = taskbar.left - width - 8;
    const UINT flags = SWP_NOACTIVATE | (visible() ? SWP_SHOWWINDOW : 0);
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, flags);
}

void MainWindow::update_shape() {
    if (!hwnd_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int radius = static_cast<int>(18.0f * controller_->settings_copy().window_scale * dpi_ / 96.0f);
    HRGN region = CreateRoundRectRgn(0, 0, client.right + 1, client.bottom + 1, radius * 2, radius * 2);
    SetWindowRgn(hwnd_, region, TRUE);
}

void MainWindow::dispatch_click(float x, float y) {
    for (auto iterator = hits_.rbegin(); iterator != hits_.rend(); ++iterator) {
        if (x >= iterator->rect.left && x <= iterator->rect.right && y >= iterator->rect.top && y <= iterator->rect.bottom) {
            controller_->handle_action(iterator->action, iterator->value);
            return;
        }
    }
}

void MainWindow::add_hit(D2D1_RECT_F rect, std::string action, double value) {
    hits_.push_back({rect, std::move(action), value});
}

void MainWindow::text(std::wstring_view value, D2D1_RECT_F rect, float size, D2D1_COLOR_F color_value,
                      DWRITE_TEXT_ALIGNMENT alignment, DWRITE_FONT_WEIGHT weight) {
    if (!render_target_ || !write_factory_ || value.empty()) return;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    if (FAILED(write_factory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                                 DWRITE_FONT_STRETCH_NORMAL, size * font_scale_, L"zh-CN", format.GetAddressOf()))) return;
    format->SetTextAlignment(alignment);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    brush_->SetColor(color_value);
    render_target_->DrawText(value.data(), static_cast<UINT32>(value.size()), format.Get(), rect, brush_.Get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

void MainWindow::line(float x1, float y1, float x2, float y2, D2D1_COLOR_F color_value, float width) {
    brush_->SetColor(color_value);
    render_target_->DrawLine({x1, y1}, {x2, y2}, brush_.Get(), width);
}

void MainWindow::rounded_rect(D2D1_RECT_F rect, float radius, D2D1_COLOR_F fill, D2D1_COLOR_F stroke, float stroke_width) {
    const auto rounded = D2D1::RoundedRect(rect, radius, radius);
    brush_->SetColor(fill);
    render_target_->FillRoundedRectangle(rounded, brush_.Get());
    if (stroke_width > 0.0f && stroke.a > 0.0f) {
        brush_->SetColor(stroke);
        render_target_->DrawRoundedRectangle(rounded, brush_.Get(), stroke_width);
    }
}

void MainWindow::circle(float x, float y, float radius, D2D1_COLOR_F fill, D2D1_COLOR_F stroke, float stroke_width) {
    const auto ellipse = D2D1::Ellipse({x, y}, radius, radius);
    brush_->SetColor(fill);
    render_target_->FillEllipse(ellipse, brush_.Get());
    if (stroke_width > 0.0f && stroke.a > 0.0f) {
        brush_->SetColor(stroke);
        render_target_->DrawEllipse(ellipse, brush_.Get(), stroke_width);
    }
}

bool MainWindow::english(const Settings& settings) const {
    if (settings.language == LanguageMode::English) return true;
    if (settings.language == LanguageMode::Chinese) return false;
    return PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_CHINESE;
}

bool MainWindow::dark(const Settings& settings) const {
    if (settings.theme == ThemeMode::Dark) return true;
    if (settings.theme == ThemeMode::Light) return false;
    DWORD light = 0;
    DWORD size = sizeof(light);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) == ERROR_SUCCESS) return light == 0;
    return true;
}

std::wstring MainWindow::tr(const Settings& settings, std::wstring_view chinese, std::wstring_view english_text) const {
    return std::wstring(english(settings) ? english_text : chinese);
}

} // namespace cqt

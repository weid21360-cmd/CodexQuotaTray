#pragma once

#include "models.hpp"

#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>
#include <string_view>
#include <vector>

namespace cqt {

class AppController;

enum class Page { Home, Settings, More };

class MainWindow {
public:
    MainWindow() = default;
    ~MainWindow();

    [[nodiscard]] bool create(AppController* controller, std::wstring& error);
    void show();
    void hide();
    [[nodiscard]] bool visible() const;
    [[nodiscard]] HWND hwnd() const { return hwnd_; }
    void invalidate();
    void apply_settings();
    void set_page(Page page);
    [[nodiscard]] Page page() const { return page_; }
    void set_modal(bool value) { modal_ = value; }

    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);

private:
    struct HitTarget {
        D2D1_RECT_F rect{};
        std::string action;
        double value = 0.0;
    };

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    void create_device_resources();
    void discard_device_resources();
    void create_text_formats();
    void paint();
    void draw_home(const UsageSnapshot& snapshot, const Settings& settings);
    void draw_settings(const Settings& settings);
    void draw_more(const Settings& settings);
    void position_near_taskbar();
    void update_shape();
    void dispatch_click(float x, float y);
    void add_hit(D2D1_RECT_F rect, std::string action, double value = 0.0);

    void text(std::wstring_view value, D2D1_RECT_F rect, float size, D2D1_COLOR_F color,
              DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL);
    void line(float x1, float y1, float x2, float y2, D2D1_COLOR_F color, float width = 1.0f);
    void rounded_rect(D2D1_RECT_F rect, float radius, D2D1_COLOR_F fill, D2D1_COLOR_F stroke = {}, float stroke_width = 0.0f);
    void circle(float x, float y, float radius, D2D1_COLOR_F fill, D2D1_COLOR_F stroke = {}, float stroke_width = 0.0f);

    [[nodiscard]] bool english(const Settings& settings) const;
    [[nodiscard]] bool dark(const Settings& settings) const;
    [[nodiscard]] std::wstring tr(const Settings& settings, std::wstring_view chinese, std::wstring_view english_text) const;

    AppController* controller_ = nullptr;
    HWND hwnd_ = nullptr;
    UINT taskbar_created_message_ = 0;
    UINT dpi_ = 96;
    float font_scale_ = 1.0f;
    Page page_ = Page::Home;
    bool modal_ = false;
    std::vector<HitTarget> hits_;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_factory_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> render_target_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<IDWriteFactory> write_factory_;
};

} // namespace cqt

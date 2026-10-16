#pragma once
#include "KeyboardLogic.h"
#include <fluentui/design.hpp>
#include <fluentui/system_backdrop.hpp>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <map>
#include <algorithm>
#include <memory>
#include <string_view>

namespace ftc::keyboard_editor
{
inline const auto Theme = fluent::Palette::get(fluent::ThemeMode::Light,
    fluent::AccentPalette::fromColor(fluent::Color::rgb(0x0067C0)));
inline const auto Design = fluent::DesignPalette::get(Theme);
inline const auto Controls = fluent::ControlPalette::get(Theme);
enum class Language { Chinese, English, Vietnamese };
inline const wchar_t* Text(Language language, const wchar_t* zh, const wchar_t* en, const wchar_t* vi)
{ return language == Language::Chinese ? zh : language == Language::Vietnamese ? vi : en; }
inline int Px(int dip, UINT dpi) { return MulDiv(dip, static_cast<int>(dpi), 96); }

// Private presentation code. Each executable owns its own renderer and material.
class Canvas final
{
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID2D1Factory> factory_;
    Ptr<IDWriteFactory> write_;
    Ptr<ID2D1HwndRenderTarget> target_;
    Ptr<ID2D1SolidColorBrush> brush_;
    std::map<std::pair<int, int>, Ptr<IDWriteTextFormat>> formats_;
    fluent::Host material_;
    HWND window_{};
    bool mica_{};
public:
    void Attach(HWND window)
    {
        window_ = window;
        fluent::HostOptions options;
        options.theme = fluent::ThemeMode::Light;
        options.compositionSurface = true; options.transparentBackground = true;
        if (material_.create(window, {0, 0, 1, 1}, options))
        {
            auto backdrop = std::make_unique<fluent::SystemBackdrop>(fluent::WindowEffect::Mica);
            backdrop->onChanged = [this] { Configure(); };
            material_.setRoot(std::move(backdrop));
        }
        Configure();
    }
    void Configure() noexcept
    {
        if (!window_ || !IsWindow(window_)) return;
        const auto* backdrop = dynamic_cast<const fluent::SystemBackdrop*>(material_.root());
        const BOOL dark = FALSE; const int corner = 2;
        DwmSetWindowAttribute(window_, static_cast<DWMWINDOWATTRIBUTE>(20), &dark, sizeof(dark));
        DwmSetWindowAttribute(window_, static_cast<DWMWINDOWATTRIBUTE>(33), &corner, sizeof(corner));
        const MARGINS margins = backdrop && backdrop->effective() ? MARGINS{-1, -1, -1, -1} : MARGINS{};
        mica_ = margins.cxLeftWidth == -1 && SUCCEEDED(DwmExtendFrameIntoClientArea(window_, &margins));
        InvalidateRect(window_, nullptr, FALSE);
    }
    static D2D1_COLOR_F Color(fluent::Color value) noexcept { return {value.r, value.g, value.b, value.a}; }
    bool Begin(HWND window, UINT dpi)
    {
        if (!factory_ && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory_)))) return false;
        if (!write_ && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(write_.GetAddressOf())))) return false;
        RECT client{}; GetClientRect(window, &client);
        const auto size = D2D1::SizeU(static_cast<UINT32>(std::max(1L, client.right)),
            static_cast<UINT32>(std::max(1L, client.bottom)));
        if (!target_)
        {
            if (FAILED(factory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                D2D1::HwndRenderTargetProperties(window, size), &target_))) return false;
            if (FAILED(target_->CreateSolidColorBrush(D2D1::ColorF(0), &brush_))) { target_.Reset(); return false; }
        }
        else if (FAILED(target_->Resize(size))) { brush_.Reset(); target_.Reset(); return false; }
        target_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
        target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        target_->BeginDraw();
        target_->Clear(mica_ ? D2D1::ColorF(0, 0.0F) : Color(Design.window));
        return true;
    }
    void End(HWND window)
    {
        if (FAILED(target_->EndDraw())) { brush_.Reset(); target_.Reset(); InvalidateRect(window, nullptr, FALSE); }
    }
    void Fill(D2D1_RECT_F rect, fluent::Color color, float radius = 0)
    {
        brush_->SetColor(Color(color));
        target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.Get());
    }
    void Stroke(D2D1_RECT_F rect, fluent::Color color, float radius = 6, float width = 1)
    {
        brush_->SetColor(Color(color));
        target_->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.Get(), width);
    }
    void Label(std::wstring_view text, D2D1_RECT_F rect, float size = 14, int weight = 400,
        fluent::Color color = Design.text, bool centered = false, bool wrap = false)
    {
        const auto key = std::pair{static_cast<int>(size * 10), weight};
        auto& format = formats_[key];
        if (!format)
        {
            if (FAILED(write_->CreateTextFormat(L"Segoe UI", nullptr, static_cast<DWRITE_FONT_WEIGHT>(weight),
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &format))) return;
            Ptr<IDWriteInlineObject> ellipsis;
            if (SUCCEEDED(write_->CreateEllipsisTrimmingSign(format.Get(), &ellipsis)))
            {
                const DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                format->SetTrimming(&trim, ellipsis.Get());
            }
        }
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetTextAlignment(centered ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        brush_->SetColor(Color(color));
        target_->DrawTextW(text.data(), static_cast<UINT32>(text.size()), format.Get(), rect,
            brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
};
struct KeyGeometry final
{
    float scale{1}, left{}, top{};
    D2D1_RECT_F viewport{};
    D2D1_RECT_F Rect(const LayoutKey& key) const noexcept
    { return {left + key.x * scale, top + key.y * scale,
        left + (key.x + key.width) * scale, top + (key.y + key.height) * scale}; }
};
KeyGeometry FitKeys(const KeyboardLayout& layout, D2D1_RECT_F area);
void DrawKeys(Canvas& canvas, const KeyboardLayout& layout, const std::vector<KeyStatus>& status,
    const KeyGeometry& geometry, int selected = -1);
void DrawButton(const DRAWITEMSTRUCT& item, UINT dpi, HFONT font, bool accent = false);
void PrepareButtonSurface(HWND button, UINT dpi);
void ShowPrepared(HWND window);
}

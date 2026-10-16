#include "KeyboardUi.h"
#include <algorithm>
#include <string>

namespace ftc::modules::keyboard_test
{
KeyGeometry FitKeys(const KeyboardLayout& layout, const D2D1_RECT_F area)
{
    int width = std::max(100, layout.width - 12), height = std::max(100, layout.height - 32);
    for (const auto& key : layout.keys)
    { width = std::max(width, key.x + key.width + 10); height = std::max(height, key.y + key.height + 10); }
    const float scale = std::max(0.01F, std::min({(area.right - area.left) / width,
        (area.bottom - area.top) / height, 1.5F}));
    return {scale, area.left + ((area.right - area.left) - width * scale) / 2,
        area.top + ((area.bottom - area.top) - height * scale) / 2, area};
}
void DrawKeys(Canvas& canvas, const KeyboardLayout& layout, const std::vector<KeyStatus>& status,
    const KeyGeometry& geometry, const int selected)
{
    for (std::size_t i = 0; i < layout.keys.size(); ++i)
    {
        const auto rect = geometry.Rect(layout.keys[i]);
        const KeyStatus state = i < status.size() ? status[i] : KeyStatus::Waiting;
        auto fill = fluent::Color::rgb(0xFAFAFA), ink = Design.text;
        if (state == KeyStatus::Passed) { fill = fluent::Color::rgb(0xDFF6DD); ink = fluent::Color::rgb(0x0F7B0F); }
        else if (state == KeyStatus::Pressed) { fill = fluent::Color::rgb(0xE5F1FB); ink = fluent::Color::rgb(0x0067C0); }
        else if (state == KeyStatus::Retry) { fill = fluent::Color::rgb(0xFFF4CE); ink = fluent::Color::rgb(0x8A4B00); }
        canvas.Fill(rect, fill, 5);
        canvas.Stroke(rect, static_cast<int>(i) == selected ? fluent::Color::rgb(0x0067C0) :
            fluent::Color::rgb(0xDEDEDE), 5, static_cast<int>(i) == selected ? 2.0F : 1.0F);
        canvas.Label(layout.keys[i].label, {rect.left + 3, rect.top + 2, rect.right - 3, rect.bottom - 2},
            std::clamp(13.0F * geometry.scale, 8.0F, 18.0F), 400, ink, true, true);
    }
}
void PrepareButtonSurface(const HWND button, const UINT dpi)
{
    if (!button) return;
    // GDI buttons need an opaque surface above the extended Mica frame.
    const LONG_PTR style = GetWindowLongPtrW(button, GWL_EXSTYLE);
    if (!(style & WS_EX_LAYERED)) SetWindowLongPtrW(button, GWL_EXSTYLE, style | WS_EX_LAYERED);
    SetLayeredWindowAttributes(button, 0, 255, LWA_ALPHA);
    RECT rect{}; GetClientRect(button, &rect);
    const HRGN region = CreateRoundRectRgn(0, 0, rect.right + 1, rect.bottom + 1, Px(8, dpi), Px(8, dpi));
    if (region && !SetWindowRgn(button, region, TRUE)) DeleteObject(region);
}
void DrawButton(const DRAWITEMSTRUCT& item, const UINT dpi, const HFONT font, const bool accent)
{
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const COLORREF color = disabled ? RGB(243,243,243) : accent ? (pressed ? RGB(0,85,158) : RGB(0,103,192)) :
        pressed ? RGB(232,232,232) : RGB(250,250,250);
    const HBRUSH brush = CreateSolidBrush(color);
    const HPEN pen = CreatePen(PS_SOLID, 1, accent ? color : RGB(222,222,222));
    const auto oldBrush = SelectObject(item.hDC, brush), oldPen = SelectObject(item.hDC, pen);
    RoundRect(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom, Px(8,dpi), Px(8,dpi));
    SelectObject(item.hDC, oldBrush); SelectObject(item.hDC, oldPen); DeleteObject(brush); DeleteObject(pen);
    wchar_t text[256]{}; GetWindowTextW(item.hwndItem, text, 256);
    const auto oldFont = SelectObject(item.hDC, font);
    SetBkMode(item.hDC, TRANSPARENT); SetTextColor(item.hDC, disabled ? RGB(154,154,154) : accent ? RGB(255,255,255) : RGB(36,36,36));
    RECT rect = item.rcItem; DrawTextW(item.hDC, text, -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
    if (item.itemState & ODS_FOCUS) { InflateRect(&rect, -Px(4,dpi), -Px(4,dpi)); DrawFocusRect(item.hDC, &rect); }
    SelectObject(item.hDC, oldFont);
}
namespace
{
constexpr UINT_PTR entrance = 0x46544B42;
constexpr wchar_t prop[] = L"FTC.Keyboard.Entrance";
void EndEntrance(HWND window)
{
    KillTimer(window, entrance); RemovePropW(window, prop);
    SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA);
    SetWindowLongPtrW(window, GWL_EXSTYLE, GetWindowLongPtrW(window, GWL_EXSTYLE) & ~static_cast<LONG_PTR>(WS_EX_LAYERED));
    EnumChildWindows(window,[](HWND child,LPARAM)->BOOL {
        if (GetWindowLongPtrW(child,GWL_EXSTYLE)&WS_EX_LAYERED)
        {
            SetLayeredWindowAttributes(child,0,255,LWA_ALPHA);
            RedrawWindow(child,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE);
        }
        return TRUE;
    },0);
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
}
void CALLBACK Entrance(HWND window, UINT, UINT_PTR, DWORD)
{
    const auto start = reinterpret_cast<ULONGLONG>(GetPropW(window, prop));
    const auto now = GetTickCount64();
    if (!start) return;
    if (start == 1 && !SetPropW(window, prop, reinterpret_cast<HANDLE>(now))) { EndEntrance(window); return; }
    const auto elapsed = start == 1 ? 0 : now - start;
    if (elapsed >= 160 || IsIconic(window)) { EndEntrance(window); return; }
    const double remainder = 1.0 - static_cast<double>(elapsed) / 160.0;
    if (!SetLayeredWindowAttributes(window, 0, static_cast<BYTE>(255 - 254 * remainder * remainder * remainder), LWA_ALPHA))
        EndEntrance(window);
}
}
void ShowPrepared(HWND window)
{
    const auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    SetWindowLongPtrW(window, GWL_EXSTYLE, style | WS_EX_LAYERED);
    const bool prepared = SetLayeredWindowAttributes(window, 0, 0, LWA_ALPHA) != FALSE;
    if (!prepared) SetWindowLongPtrW(window, GWL_EXSTYLE, style);
    ShowWindow(window, SW_SHOW);
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    if (!prepared) return;
    BOOL animations = FALSE; HIGHCONTRASTW contrast{sizeof(contrast)};
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0) && animations &&
        !(contrast.dwFlags & HCF_HIGHCONTRASTON) && SetPropW(window, prop, reinterpret_cast<HANDLE>(1)) &&
        SetTimer(window, entrance, 15, Entrance)) return;
    EndEntrance(window);
}
}

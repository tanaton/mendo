#include "settings_dialog_impl.h"
#include "d2d_util.h"
#include "theme.h"
#include "ui_constants.h"
#include "utility.h"
#include "win32_util.h"
#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <utility>

using Microsoft::WRL::ComPtr;
using namespace settings_dialog_constants;

std::wstring FormatByteSize(uint64_t bytes)
{
    if (bytes < 1024) {
        return std::format(L"{} B", bytes);
    }
    constexpr const wchar_t* kUnits[] = { L"KB", L"MB", L"GB", L"TB" };
    double value = static_cast<double>(bytes) / 1024.0;
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(kUnits)) {
        value /= 1024.0;
        ++unit;
    }
    // 桁が少ないときだけ小数を出す (「12.3 MB」「512 KB」)。
    return value < 100.0 ? std::format(L"{:.1f} {}", value, kUnits[unit]) : std::format(L"{:.0f} {}", value, kUnits[unit]);
}

SettingsDialog::SettingsDialog() : impl_(std::make_unique<Impl>())
{}
SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::Init(ID2D1Factory* d2d_factory, IDWriteFactory* dwrite_factory)
{
    impl_->d2d_factory = d2d_factory;
    impl_->dwrite_factory = dwrite_factory;
}

void SettingsDialog::Impl::CreateTextFormats()
{
    const wchar_t* const locale = i18n::S().lang_locale;
    const wchar_t* const family = theme->font_family.c_str();
    const float size = theme->pane_font_size;
    constexpr auto CENTER = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
    fmt_title = CreateTextFormat(dwrite_factory, { .family = family, .weight = DWRITE_FONT_WEIGHT_SEMI_BOLD, .size = size * TITLE_FONT_SCALE, .locale = locale, .para_align = CENTER, .no_wrap = true });
    fmt_text = CreateTextFormat(dwrite_factory, { .family = family, .size = size, .locale = locale, .para_align = CENTER, .no_wrap = true });
    fmt_text_center = CreateTextFormat(dwrite_factory, { .family = family, .size = size, .locale = locale, .text_align = DWRITE_TEXT_ALIGNMENT_CENTER, .para_align = CENTER, .no_wrap = true });
    fmt_icon = CreateTextFormat(dwrite_factory, { .family = theme->icon_font.c_str(), .size = ICON_FONT_SIZE, .text_align = DWRITE_TEXT_ALIGNMENT_CENTER, .para_align = CENTER, .no_wrap = true });
    for (size_t i = 0; i < std::size(i18n::kAllStrings); ++i) {
        fmt_language_names[i] = CreateTextFormat(dwrite_factory, { .family = family, .size = size, .locale = i18n::kAllStrings[i]->lang_locale, .para_align = CENTER, .no_wrap = true });
    }
}

float SettingsDialog::Impl::TextWidth(std::wstring_view text, IDWriteTextFormat* fmt) const
{
    ComPtr<IDWriteTextLayout> text_layout;
    if (!fmt || FAILED(dwrite_factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), fmt, 10000.0f, 100.0f, &text_layout))) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    text_layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

void SettingsDialog::Impl::Rebuild()
{
    const auto& ls = i18n::S();
    system_language_text = std::format(L"{} ({})", ls.settings_language_system, i18n::SystemStrings().lang_name);
    cache_size_text = FormatByteSize(cache_bytes);
    CreateTextFormats();
    ComputeLayout();
}

void SettingsDialog::Impl::ComputeLayout()
{
    const auto& ls = i18n::S();
    const float button_w = std::max(BUTTON_MIN_WIDTH, TextWidth(ls.settings_clear_cache, fmt_text.Get()) + 2 * BUTTON_PAD_X);

    float content_w = TextWidth(ls.settings_title, fmt_title.Get());
    content_w = std::max(content_w, TextWidth(ls.settings_language, fmt_text.Get()));
    content_w = std::max(content_w, CHECK_WIDTH + TextWidth(system_language_text, fmt_text.Get()));
    for (size_t i = 0; i < std::size(i18n::kAllStrings); ++i) {
        content_w = std::max(content_w, CHECK_WIDTH + TextWidth(i18n::kAllStrings[i]->lang_name, fmt_language_names[i].Get()));
    }
    content_w = std::max(content_w, TextWidth(ls.settings_diagram_cache, fmt_text.Get()));
    content_w = std::max(content_w, TextWidth(cache_size_text, fmt_text.Get()) + BUTTON_GAP + button_w);

    Layout l;
    const float w = std::max(MIN_WIDTH, std::ceil(content_w + 2 * PAD_X));
    l.width = w;

    float y = TITLE_HEIGHT;
    l.language_heading = { PAD_X, y, w - PAD_X, y + HEADING_HEIGHT };
    y += HEADING_HEIGHT;
    for (auto& row : l.languages) {
        row = { 0.0f, y, w, y + ROW_HEIGHT };
        y += ROW_HEIGHT;
    }
    y += SECTION_GAP;
    l.separator_y = y;
    y += SECTION_GAP;
    l.cache_heading = { PAD_X, y, w - PAD_X, y + HEADING_HEIGHT };
    y += HEADING_HEIGHT;
    l.clear = { w - PAD_X - button_w, y, w - PAD_X, y + ROW_HEIGHT };
    l.cache_size = { PAD_X, y, l.clear.left - BUTTON_GAP, y + ROW_HEIGHT };
    y += ROW_HEIGHT + BOTTOM_PAD;
    l.height = y;
    layout = l;
}

SettingsDialog::Impl::Hit SettingsDialog::Impl::HitTest(float x, float y) const noexcept
{
    for (size_t row = 0; row < layout.languages.size(); ++row) {
        if (PointInRect(x, y, layout.languages[row])) {
            return { HitKind::Language, row };
        }
    }
    if (CanClearCache() && PointInRect(x, y, layout.clear)) {
        return { HitKind::ClearCache };
    }
    return {};
}

void SettingsDialog::Impl::Activate(const Hit& hit)
{
    switch (hit.kind) {
    case HitKind::None:
        return;
    case HitKind::Language: {
        const i18n::Strings* const chosen = LanguageOfRow(hit.row);
        if (chosen == i18n::ChosenLanguage() || !on_language) {
            return;
        }
        on_language(chosen);
        Rebuild();
        break;
    }
    case HitKind::ClearCache:
        on_clear_cache();
        cache_bytes = 0;
        cache_size_text = FormatByteSize(cache_bytes);
        break;
    }
    // テストはウィンドウを作らずに呼ぶ。
    if (!hwnd) {
        return;
    }
    // 言語で幅が変わるので、今の中心のまま大きさを合わせる。
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    PlaceWindow(rc);
    // 組み直しで行が動いても、押せなくなったボタンの上でも、止まったままのカーソルで引き直す。
    RefreshHover();
    InvalidateRect(hwnd, nullptr, FALSE);
}

void SettingsDialog::Impl::RefreshHover()
{
    // キャプチャしているので窓の外の動きも届く。外では何にも当たらずホバーが消える。
    POINT pt{};
    Hit hit{};
    if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)) {
        const auto [x, y] = PixelToDip(pt.x, pt.y, dpi_scale);
        hit = HitTest(x, y);
    }
    if (hit != hovered) {
        hovered = hit;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

namespace {

constexpr wchar_t kSettingsDialogClass[] = L"mendoSettingsDialog";

} // namespace

bool SettingsDialog::Impl::RegisterWindowClass()
{
    static const bool registered_ok = RegisterPopupWindowClass(kSettingsDialogClass, WndProc);
    return registered_ok;
}

LRESULT CALLBACK SettingsDialog::Impl::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* self = BindWindowInstance<SettingsDialog::Impl>(hwnd, msg, lParam);
    if (!self) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (msg == WM_NCCREATE) {
        self->hwnd = hwnd;
    }
    return self->HandleMessage(msg, wParam, lParam);
}

bool SettingsDialog::Impl::CreateDialogWindow()
{
    if (!RegisterWindowClass()) {
        return false;
    }
    // 大きさと位置は PlaceWindow で決める。ウィンドウ名は読み上げ用。
    hwnd = CreateWindowExW(
        0, kSettingsDialogClass, std::wstring(i18n::S().settings_title).c_str(),
        WS_POPUP,
        0, 0, 1, 1,
        owner, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd) {
        return false;
    }
    RECT owner_rc{};
    GetWindowRect(owner, &owner_rc);
    PlaceWindow(owner_rc);
    if (!CreateDeviceResources()) {
        DestroyDialogWindow();
        return false;
    }
    return true;
}

void SettingsDialog::Impl::DestroyDialogWindow() noexcept
{
    // hwnd と rt は WM_NCDESTROY で手放す。
    if (hwnd) {
        DestroyWindow(hwnd);
    }
}

void SettingsDialog::Impl::PlaceWindow(const RECT& around)
{
    const int pixel_w = DipToPixelCeil(layout.width, dpi_scale);
    const int pixel_h = DipToPixelCeil(layout.height, dpi_scale);
    const POINT pos = ClampToWorkArea((around.left + around.right - pixel_w) / 2, (around.top + around.bottom - pixel_h) / 2, pixel_w, pixel_h);
    SetWindowPos(hwnd, nullptr, pos.x, pos.y, pixel_w, pixel_h, SWP_NOZORDER | SWP_NOACTIVATE);

    if (rt) {
        rt->SetDpi(dpi_scale * DEFAULT_DPI, dpi_scale * DEFAULT_DPI);
        if (FAILED(rt->Resize(D2D1_SIZE_U{ static_cast<UINT32>(pixel_w), static_cast<UINT32>(pixel_h) }))) {
            rt.Reset();
        }
    }
    ApplyRoundedRegion(hwnd, pixel_w, pixel_h, CORNER, dpi_scale, /*redraw=*/true);
}

void SettingsDialog::Show(HWND owner_hwnd, const SettingsDialogParams& params)
{
    auto& s = *impl_;
    if (!s.d2d_factory || !s.dwrite_factory || !params.theme || params.dpi_scale <= 0.0f) {
        return;
    }

    s.owner = owner_hwnd;
    s.theme = params.theme;
    s.dpi_scale = params.dpi_scale;
    s.cache_bytes = params.diagram_cache_bytes;
    s.on_language = params.on_language;
    s.on_clear_cache = params.on_clear_cache;
    s.done = false;
    s.hovered = {};
    // theme とコールバックは呼び出し側の借用。開くたびにウィンドウを作り、閉じたら壊す (まれにしか開かない)。
    auto guard = ScopeGuard([&s] {
        s.DestroyDialogWindow();
        s.theme = nullptr;
        s.on_language = nullptr;
        s.on_clear_cache = nullptr;
    });

    s.Rebuild();
    if (!s.CreateDialogWindow()) {
        return;
    }
    // 右クリックメニューと同じく、キャプチャして範囲外のクリックを受け取り、閉じるのに使う (本体へは渡さない)。
    ShowWindow(s.hwnd, SW_SHOW);
    SetForegroundWindow(s.hwnd);
    SetFocus(s.hwnd);
    SetCapture(s.hwnd);
    // 開いた位置にカーソルがあれば、動かす前からホバーを出す。
    s.RefreshHover();
    RunPopupModalLoop(s.done);
    if (GetCapture() == s.hwnd) {
        ReleaseCapture();
    }
    // 壊すときに前面が他のアプリへ移らないよう、まだ前面にいればオーナーへ返す。
    if (s.hwnd && GetForegroundWindow() == s.hwnd) {
        SetForegroundWindow(owner_hwnd);
    }
}

LRESULT SettingsDialog::Impl::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        Paint();
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE:
        RefreshHover();
        return 0;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN: {
        // 右クリックメニューと同じく押した時点で反応する。タイトルバーのボタンは押した時点で開くので、
        // 離す側 (WM_LBUTTONUP) はダイアログを開いた後にキャプチャ経由で届く。それを押したことにしない。
        const auto [x, y] = DipPointFromLParam(lParam, dpi_scale);
        if (!PointInRect(x, y, DipRect{ 0.0f, 0.0f, layout.width, layout.height })) {
            done = true;
        }
        else if (msg == WM_LBUTTONDOWN) {
            Activate(HitTest(x, y));
        }
        return 0;
    }

    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        // 開いたままでも、ポインタの下の本文やペインをスクロールする (座標はスクリーン座標のまま渡せる)。
        // 渡さないとホイールは本体のキューに溜まらず消えるが、渡すまでの間に溜まると閉じた後に一気に動く。
        SendMessageW(owner, msg, wParam, lParam);
        return 0;

    case WM_CAPTURECHANGED:
        // 右クリックメニューと同じく、キャプチャを他に取られたら閉じる (取られたままでは範囲外のクリックが届かない)。
        if (reinterpret_cast<HWND>(lParam) != hwnd) {
            done = true;
        }
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            done = true;
        }
        return 0;

    case WM_ACTIVATEAPP:
        if (wParam == FALSE) {
            done = true;
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE || wParam == VK_RETURN) {
            done = true;
        }
        return 0;

    case WM_SYSKEYDOWN:
        if (wParam == VK_F4) {
            done = true;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_CLOSE:
        done = true;
        return 0;

    case WM_DPICHANGED: {
        dpi_scale = DpiScaleFrom(static_cast<float>(HIWORD(wParam)));
        PlaceWindow(*reinterpret_cast<const RECT*>(lParam));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_NCDESTROY: {
        // オーナーと一緒に壊されたときもループを抜ける。
        const HWND destroyed = std::exchange(hwnd, nullptr);
        rt.Reset();
        done = true;
        return DefWindowProcW(destroyed, msg, wParam, lParam);
    }

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool SettingsDialog::Impl::CreateDeviceResources()
{
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) {
        return false;
    }
    const D2D1_SIZE_U size{ static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom) };
    // 右クリックメニューと同じく、数百 px ならソフトウェアで足りる。
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE);
    props.dpiX = dpi_scale * DEFAULT_DPI;
    props.dpiY = dpi_scale * DEFAULT_DPI;
    if (FAILED(d2d_factory->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd, size), &rt))) {
        return false;
    }

    auto sub = theme->text_color;
    sub.a = HEADING_ALPHA;
    // 背後の本文と同じような明るさになるので、区切り線より濃い文字色で縁を描く。
    auto frame = theme->text_color;
    frame.a = FRAME_ALPHA;
    auto gray = theme->text_color;
    gray.a = DISABLED_UI_ALPHA;
    // ComPtr は operator& を持つので std::addressof で指す。
    const std::pair<ComPtr<ID2D1SolidColorBrush>*, D2D1_COLOR_F> brushes[] = {
        { std::addressof(brush_frame),  frame                        },
        { std::addressof(brush_border), theme->splitter_color        },
        { std::addressof(brush_text),   theme->text_color            },
        { std::addressof(brush_sub),    sub                          },
        { std::addressof(brush_gray),   gray                         },
        { std::addressof(brush_hover),  theme->pane_item_hover_color },
        { std::addressof(brush_check),  theme->link_color            },
    };
    for (const auto& [brush, color] : brushes) {
        CreateSolidColorBrushOrFallback(rt.Get(), color, *brush);
    }
    return true;
}

void SettingsDialog::Impl::DrawLabel(std::wstring_view text, IDWriteTextFormat* fmt, const DipRect& rect, ID2D1Brush* brush)
{
    if (fmt && !text.empty()) {
        rt->DrawText(text.data(), static_cast<UINT32>(text.size()), fmt, ToD2DRect(rect), brush);
    }
}

void SettingsDialog::Impl::DrawHover(const DipRect& rect)
{
    const D2D1_ROUNDED_RECT rr{ ToD2DRect(rect), HOVER_CORNER, HOVER_CORNER };
    rt->FillRoundedRectangle(rr, brush_hover.Get());
}

void SettingsDialog::Impl::Paint()
{
    // デバイスロスト後 (EndDraw が D2DERR_RECREATE_TARGET で rt を破棄) はここで再生成する。
    if (!rt && !CreateDeviceResources()) {
        return;
    }
    const auto& ls = i18n::S();
    const float w = layout.width;

    rt->BeginDraw();
    rt->Clear(theme->pane_bg_color);
    const D2D1_RECT_F frame_rect{ FRAME_WIDTH * 0.5f, FRAME_WIDTH * 0.5f, w - FRAME_WIDTH * 0.5f, layout.height - FRAME_WIDTH * 0.5f };
    rt->DrawRoundedRectangle({ frame_rect, CORNER, CORNER }, brush_frame.Get(), FRAME_WIDTH);

    DrawLabel(ls.settings_title, fmt_title.Get(), { PAD_X, 0.0f, w - PAD_X, TITLE_HEIGHT }, brush_text.Get());

    DrawLabel(ls.settings_language, fmt_text.Get(), layout.language_heading, brush_sub.Get());
    const i18n::Strings* const chosen = i18n::ChosenLanguage();
    for (size_t row = 0; row < layout.languages.size(); ++row) {
        const DipRect& rect = layout.languages[row];
        if (hovered == Hit{ HitKind::Language, row }) {
            DrawHover({ HOVER_MARGIN_X, rect.top + 1.0f, w - HOVER_MARGIN_X, rect.bottom - 1.0f });
        }
        const i18n::Strings* const language = LanguageOfRow(row);
        if (language == chosen) {
            DrawLabel(GLYPH_CHECKMARK, fmt_icon.Get(), { PAD_X - 4.0f, rect.top, PAD_X + CHECK_WIDTH - 8.0f, rect.bottom }, brush_check.Get());
        }
        const DipRect text_rect{ PAD_X + CHECK_WIDTH, rect.top, w - PAD_X, rect.bottom };
        if (language) {
            DrawLabel(language->lang_name, fmt_language_names[row - 1].Get(), text_rect, brush_text.Get());
        }
        else {
            DrawLabel(system_language_text, fmt_text.Get(), text_rect, brush_text.Get());
        }
    }

    rt->DrawLine({ PAD_X, layout.separator_y }, { w - PAD_X, layout.separator_y }, brush_border.Get(), 1.0f);

    DrawLabel(ls.settings_diagram_cache, fmt_text.Get(), layout.cache_heading, brush_sub.Get());
    DrawLabel(cache_size_text, fmt_text.Get(), layout.cache_size, brush_text.Get());
    // 押せないボタンには HitTest が当たらないので、ホバーは押せるときだけ出る。
    if (hovered.kind == HitKind::ClearCache) {
        DrawHover(layout.clear);
    }
    const D2D1_RECT_F button_border{ layout.clear.left + 0.5f, layout.clear.top + 0.5f, layout.clear.right - 0.5f, layout.clear.bottom - 0.5f };
    rt->DrawRoundedRectangle({ button_border, HOVER_CORNER, HOVER_CORNER }, brush_border.Get(), 1.0f);
    DrawLabel(ls.settings_clear_cache, fmt_text_center.Get(), layout.clear, CanClearCache() ? brush_text.Get() : brush_gray.Get());

    if (rt->EndDraw() == D2DERR_RECREATE_TARGET) {
        rt.Reset();
        // 次の WM_PAINT で rt を再生成させる。
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

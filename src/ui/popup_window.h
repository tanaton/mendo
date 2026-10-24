#pragma once
// 自前描画のポップアップ (右クリックメニュー・設定ダイアログ) の土台。ポップアップの Impl だけがインクルードする。
#include "d2d_util.h"
#include "i18n.h"
#include "theme.h"
#include "ui_constants.h"
#include "ui_types.h"
#include "win32_util.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <windows.h>
#include <algorithm>
#include <string_view>
#include <utility>

inline constexpr float POPUP_CORNER = 8.0f;
// 縁は背後の本文と同じような明るさになるので、区切り線より濃い文字色で描く。
inline constexpr float POPUP_FRAME_WIDTH = 2.0f;
inline constexpr float POPUP_FRAME_ALPHA = 0.45f;
// 見出しの文字。押せない文字 (DISABLED_UI_ALPHA) より濃くする。
inline constexpr float POPUP_SUB_TEXT_ALPHA = 0.7f;
inline constexpr float POPUP_HOVER_CORNER = 4.0f;
inline constexpr wchar_t POPUP_GLYPH_CHECKMARK[] = L"\xE73E";

// 確定仕様: ポップアップはどれも同じ振る舞いにし、新しいポップアップは中身だけを書けば済むようにする。
// - Impl は開くたびに作り、閉じたら捨てる (ウィンドウ・描画先・ブラシ・書式を開いている間だけ持つ)。
// - オーナーの上に出して前面・フォーカス・キャプチャを取り、閉じたら前面をオーナーへ返す。
// - 開閉をオーナーへ WM_ENTERMENULOOP / WM_EXITMENULOOP で知らせる (本体はその間ホバーとツールチップを出さない)。
// - 範囲外の押下 (左・右・中)、キャプチャを取られる、非アクティブになる、Esc / Enter / Alt+F4 で閉じる。
// - ホイールはオーナーに渡し、開いたままポインタの下の本文やペインをスクロールできる。
// - 開いた瞬間からカーソルの下にホバーを出す。押した後も引き直して描き直す。DPI が変わったら大きさを合わせ直す。
// - 背景・縁・角丸・色・UI の文言の書式は共通。
//
// Derived は次を持つ。
//   static constexpr wchar_t kClassName[];       ウィンドウクラス名
//   void PaintContent();                         縁の内側を描く (BeginDraw / EndDraw は土台が行う)
//   bool UpdateHover(float x, float y);          ホバーを引き直し、変わったら true
//   void OnPress(UINT msg, float x, float y);    範囲内の押下 (WM_LBUTTONDOWN / WM_RBUTTONDOWN / WM_MBUTTONDOWN)
template <class Derived>
struct PopupWindow {
    // rt から作るので、rt と一緒に捨てる。
    struct Brushes {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> frame;
        // 区切り線とボタンの枠。
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> border;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> sub;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> gray;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hover;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> check;
    };

    HWND hwnd = nullptr;
    HWND owner = nullptr;
    // 立ったらモーダルループを抜けて閉じる。
    bool done = false;
    float dpi_scale = 1.0f;
    // DIP。Derived がレイアウトで決める。
    float width = 0.0f;
    float height = 0.0f;

    ID2D1Factory* d2d_factory = nullptr;
    IDWriteFactory* dwrite_factory = nullptr;
    // 呼び出し側から借りる。
    const Theme* theme = nullptr;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> rt;
    Brushes brushes;
    // UI の文言は表示中の言語のロケールで組む (OS と違う言語を選んでも漢字の字形をその言語に合わせる)。
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_text;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_text_center;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_icon;

    // 開く前に呼ぶ。描けない組み合わせなら false。
    bool Setup(const PopupEnv& env) noexcept
    {
        d2d_factory = env.d2d_factory;
        dwrite_factory = env.dwrite_factory;
        theme = env.theme;
        dpi_scale = env.dpi_scale;
        return d2d_factory && dwrite_factory && theme && dpi_scale > 0.0f;
    }

    // 文字は等倍のサイドペインの項目と同じ大きさにする。言語を切り替えたら呼び直す (アイコンはロケールに依らないので作り直さない)。
    void CreateBaseTextFormats(float icon_size)
    {
        const wchar_t* const family = theme->font_family.c_str();
        const wchar_t* const locale = i18n::S().lang_locale;
        constexpr auto CENTER = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
        fmt_text = CreateTextFormat(dwrite_factory, { .family = family, .size = theme->pane_font_size, .locale = locale, .para_align = CENTER, .no_wrap = true });
        fmt_text_center = CreateTextFormat(dwrite_factory, { .family = family, .size = theme->pane_font_size, .locale = locale, .text_align = DWRITE_TEXT_ALIGNMENT_CENTER, .para_align = CENTER, .no_wrap = true });
        if (!fmt_icon) {
            fmt_icon = CreateTextFormat(dwrite_factory, { .family = theme->icon_font.c_str(), .size = icon_size, .text_align = DWRITE_TEXT_ALIGNMENT_CENTER, .para_align = CENTER, .no_wrap = true });
        }
    }

    // 1 行に組んだときの幅。ポップアップの幅を文言に合わせるのに使う。
    float Measure(std::wstring_view text, IDWriteTextFormat* fmt) const noexcept
    {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (!fmt || FAILED(dwrite_factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), fmt, 10000.0f, 100.0f, &layout))) {
            return 0.0f;
        }
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        return metrics.width;
    }

    void DrawLabel(std::wstring_view text, IDWriteTextFormat* fmt, const DipRect& rect, ID2D1Brush* brush)
    {
        if (fmt && !text.empty()) {
            rt->DrawText(text.data(), static_cast<UINT32>(text.size()), fmt, ToD2DRect(rect), brush);
        }
    }

    void FillHover(const DipRect& rect)
    {
        rt->FillRoundedRectangle({ ToD2DRect(rect), POPUP_HOVER_CORNER, POPUP_HOVER_CORNER }, brushes.hover.Get());
    }

    void DrawCheck(const DipRect& rect)
    {
        DrawLabel(POPUP_GLYPH_CHECKMARK, fmt_icon.Get(), rect, brushes.check.Get());
    }

    // ウィンドウを作って pos (左上、スクリーン座標) に置き、閉じるまで戻らない。title は読み上げ用のウィンドウ名で、
    // i18n の NUL 終端のリテラルを渡す。
    // 入力は前面・フォーカス・キャプチャでポップアップに来るので、本体宛ての描画・タイマー・通知はそのまま配る
    // (開いている間も図の描画や読み直しが進み、閉じた後にまとめて動かない)。
    // Dispatch は副作用の一覧を呼び出しごとに持つので、ポップアップを開いた Dispatch の途中で割り込まれても壊れない。
    void RunModal(HWND owner_hwnd, std::wstring_view title, POINT pos)
    {
        static const bool registered = RegisterWindowClass();
        if (!registered) {
            return;
        }
        owner = owner_hwnd;
        done = false;
        const RECT r = BoundsAt(pos);
        hwnd = CreateWindowExW(0, Derived::kClassName, title.data(), WS_POPUP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                               owner, nullptr, GetModuleHandleW(nullptr), static_cast<Derived*>(this));
        if (!hwnd) {
            return;
        }
        ApplyRegion(r.right - r.left, r.bottom - r.top);
        if (CreateDeviceResources()) {
            RefreshHover();
            // システムメニューと同じ通知にする。wParam は TrackPopupMenu かどうかで、自前描画なので FALSE。
            SendMessageW(owner, WM_ENTERMENULOOP, FALSE, 0);
            RunLoop();
            SendMessageW(owner, WM_EXITMENULOOP, FALSE, 0);
        }
        // hwnd・rt・ブラシは WM_NCDESTROY で手放す。
        if (hwnd) {
            DestroyWindow(hwnd);
        }
    }

    // 今の width / height にして pos (左上) に置く。
    void Place(POINT pos)
    {
        const RECT r = BoundsAt(pos);
        const int pixel_w = r.right - r.left;
        const int pixel_h = r.bottom - r.top;
        SetWindowPos(hwnd, nullptr, r.left, r.top, pixel_w, pixel_h, SWP_NOZORDER | SWP_NOACTIVATE);
        if (rt) {
            rt->SetDpi(dpi_scale * DEFAULT_DPI, dpi_scale * DEFAULT_DPI);
            if (FAILED(rt->Resize(D2D1_SIZE_U{ static_cast<UINT32>(pixel_w), static_cast<UINT32>(pixel_h) }))) {
                rt.Reset();
            }
        }
        ApplyRegion(pixel_w, pixel_h);
    }

    // around (スクリーン座標) の中央に置くときの左上。
    POINT CenteredIn(const RECT& around) const noexcept
    {
        const int pixel_w = DipToPixelCeil(width, dpi_scale);
        const int pixel_h = DipToPixelCeil(height, dpi_scale);
        return { (around.left + around.right - pixel_w) / 2, (around.top + around.bottom - pixel_h) / 2 };
    }

    // カーソルの位置からホバーを引き直す。キャプチャしているので窓の外の動きも届き、外では何にも当たらずに消える。
    void RefreshHover()
    {
        POINT pt{};
        if (!GetCursorPos(&pt) || !ScreenToClient(hwnd, &pt)) {
            return;
        }
        const auto [x, y] = PixelToDip(pt.x, pt.y, dpi_scale);
        if (derived().UpdateHover(x, y)) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }

    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
    {
        // マウスのメッセージの lParam はクライアント座標。キャプチャ中は窓の外で負にもなる。
        const auto client_dip = [&] { return PixelToDip(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)), dpi_scale); };
        switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            Paint();
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_MOUSEMOVE: {
            const auto [x, y] = client_dip();
            if (derived().UpdateHover(x, y)) {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN: {
            // 押した時点で反応する。タイトルバーのボタンは押した時点で開くので、
            // 離す側 (WM_LBUTTONUP) は開いた後にキャプチャ経由で届く。それを押したことにしない。
            const auto [x, y] = client_dip();
            if (!PointInRect(x, y, DipRect{ 0.0f, 0.0f, width, height })) {
                done = true;
                return 0;
            }
            derived().OnPress(msg, x, y);
            // 押して中身が変わっても (行が動く・押せなくなる)、止まったままのカーソルで引き直して描き直す。
            // テストはウィンドウを作らずに呼ぶ。
            if (!done && hwnd) {
                RefreshHover();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            // 座標はスクリーン座標のまま渡せる。渡さないとホイールは本体のキューに溜まらず消える。
            SendMessageW(owner, msg, wParam, lParam);
            return 0;

        case WM_CAPTURECHANGED:
            // 取られたままでは範囲外のクリックが届かない。
            if (reinterpret_cast<HWND>(lParam) != hwnd) {
                done = true;
            }
            return 0;

        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) {
                done = true;
                return 0;
            }
            break;

        case WM_ACTIVATEAPP:
            if (wParam == FALSE) {
                done = true;
            }
            return 0;

        case WM_KEYDOWN:
            // 選んだ時点で反映済みなので、Enter も閉じるだけ。
            if (wParam == VK_ESCAPE || wParam == VK_RETURN) {
                done = true;
            }
            return 0;

        case WM_SYSKEYDOWN:
            if (wParam == VK_F4) {
                done = true;
                return 0;
            }
            break;

        case WM_CLOSE:
            done = true;
            return 0;

        case WM_DPICHANGED: {
            dpi_scale = DpiScaleFrom(static_cast<float>(HIWORD(wParam)));
            const RECT& suggested = *reinterpret_cast<const RECT*>(lParam);
            Place({ suggested.left, suggested.top });
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_NCDESTROY: {
            // オーナーと一緒に壊されたときもループを抜ける。
            const HWND destroyed = std::exchange(hwnd, nullptr);
            rt.Reset();
            brushes = {};
            done = true;
            return DefWindowProcW(destroyed, msg, wParam, lParam);
        }

        default:
            break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

private:
    Derived& derived() noexcept
    {
        return static_cast<Derived&>(*this);
    }

    static bool RegisterWindowClass() noexcept
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = Derived::kClassName;
        return RegisterClassExW(&wc) != 0;
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        auto* self = BindWindowInstance<Derived>(hwnd, msg, lParam);
        if (!self) {
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        if (msg == WM_NCCREATE) {
            self->hwnd = hwnd;
        }
        return self->HandleMessage(msg, wParam, lParam);
    }

    // pos (左上) に width / height で置くときの外形。作業領域からはみ出すなら収める。
    RECT BoundsAt(POINT pos) const noexcept
    {
        const int pixel_w = DipToPixelCeil(width, dpi_scale);
        const int pixel_h = DipToPixelCeil(height, dpi_scale);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromPoint(pos, MONITOR_DEFAULTTONEAREST), &mi);
        // 作業領域より大きいときは左上端を優先する (std::clamp は lo > hi で使えない)。
        const int x = std::max(std::min(static_cast<int>(pos.x), static_cast<int>(mi.rcWork.right) - pixel_w), static_cast<int>(mi.rcWork.left));
        const int y = std::max(std::min(static_cast<int>(pos.y), static_cast<int>(mi.rcWork.bottom) - pixel_h), static_cast<int>(mi.rcWork.top));
        return { x, y, x + pixel_w, y + pixel_h };
    }

    void ApplyRegion(int pixel_w, int pixel_h) noexcept
    {
        // SetWindowRgn は成功時のみ rgn の所有権を OS に移譲する。失敗時は呼び出し側で DeleteObject。
        const int corner_px = DipToPixel(POPUP_CORNER, dpi_scale);
        const HRGN rgn = CreateRoundRectRgn(0, 0, pixel_w + 1, pixel_h + 1, corner_px, corner_px);
        if (rgn && !SetWindowRgn(hwnd, rgn, IsWindowVisible(hwnd))) {
            DeleteObject(rgn);
        }
    }

    void RunLoop() noexcept
    {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        SetCapture(hwnd);

        MSG msg{};
        while (!done) {
            if (!PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                WaitMessage();
                continue;
            }
            if (msg.message == WM_QUIT) {
                // メインループにも終了を届ける。
                PostQuitMessage(static_cast<int>(msg.wParam));
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        // 壊すときに前面が他のアプリへ移らないよう、まだ前面にいればオーナーへ返す (他のアプリへ移って閉じたときは奪わない)。
        if (hwnd && GetForegroundWindow() == hwnd) {
            SetForegroundWindow(owner);
        }
    }

    // 数百 px なので、D3D デバイスの生成を伴うハードウェアは使わない。
    bool CreateDeviceResources()
    {
        RECT rc{};
        if (!GetClientRect(hwnd, &rc)) {
            return false;
        }
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE);
        props.dpiX = dpi_scale * DEFAULT_DPI;
        props.dpiY = dpi_scale * DEFAULT_DPI;
        const D2D1_SIZE_U size{ static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom) };
        if (FAILED(d2d_factory->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd, size), &rt))) {
            return false;
        }
        const auto with_alpha = [](D2D1_COLOR_F c, float a) noexcept {
            c.a = a;
            return c;
        };
        CreateSolidColorBrushOrFallback(rt.Get(), with_alpha(theme->text_color, POPUP_FRAME_ALPHA), brushes.frame);
        CreateSolidColorBrushOrFallback(rt.Get(), theme->splitter_color, brushes.border);
        CreateSolidColorBrushOrFallback(rt.Get(), theme->text_color, brushes.text);
        CreateSolidColorBrushOrFallback(rt.Get(), with_alpha(theme->text_color, POPUP_SUB_TEXT_ALPHA), brushes.sub);
        CreateSolidColorBrushOrFallback(rt.Get(), with_alpha(theme->text_color, DISABLED_UI_ALPHA), brushes.gray);
        CreateSolidColorBrushOrFallback(rt.Get(), theme->pane_item_hover_color, brushes.hover);
        CreateSolidColorBrushOrFallback(rt.Get(), theme->link_color, brushes.check);
        return true;
    }

    void Paint()
    {
        // デバイスロスト後 (EndDraw が D2DERR_RECREATE_TARGET で rt を破棄) はここで再生成する。
        if (!rt && !CreateDeviceResources()) {
            return;
        }
        rt->BeginDraw();
        rt->Clear(theme->pane_bg_color);
        constexpr float half = POPUP_FRAME_WIDTH * 0.5f;
        rt->DrawRoundedRectangle({ { half, half, width - half, height - half }, POPUP_CORNER, POPUP_CORNER }, brushes.frame.Get(), POPUP_FRAME_WIDTH);
        derived().PaintContent();
        if (rt->EndDraw() == D2DERR_RECREATE_TARGET) {
            rt.Reset();
            // 次の WM_PAINT で作り直させる。
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }
};

#pragma once
#include "d2d_util.h"
#include "image_loader.h"
#include "mermaid_util.h"
#include "tooltip.h"
#include "worker_latch.h"
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

struct ID2D1DeviceContext5;
class MermaidRenderer;
class TaskScheduler;

namespace zoom_viewer {

// 全体表示の倍率。本文より拡大する窓なので、小さな図や画像は等倍のまま置く。窓を余白より小さく潰したら最小の倍率で止める。
float FitZoom(float view_width, float view_height, float natural_width, float natural_height) noexcept;
// dir (+1 / -1) の向きの次の倍率。全体表示の倍率も止まる位置にし、そのすぐ隣の値は 1 回押しても見た目が変わらないので除く。
// 端なら nullopt。
std::optional<float> NextZoom(float zoom, float fit_zoom, int dir) noexcept;

} // namespace zoom_viewer

// ZoomViewer が拡大表示の窓に頼む操作。本物の窓は開くと前面を取るので、テストは窓を作らない実装を差し込む。
class IZoomWindow {
public:
    virtual ~IZoomWindow() = default;
    // 本体と一緒に壊されていない。
    virtual bool IsOpen() const noexcept = 0;
    // 図か画像を出している (読み込み中でも、画像を外して読み直しを待つ間でもない)。
    virtual bool HasContent() const noexcept = 0;
    virtual void BringToFront() const noexcept = 0;
    virtual void SetTitle(std::wstring_view title) = 0;
    // ボタンのツールチップ・読み込み中の文言を、表示中の言語にする (タイトルは SetTitle)。
    virtual void ApplyLanguage() = 0;
    virtual void SetDarkMode(bool dark) = 0;
    // 別の図か画像を開いた。
    virtual void ShowLoading() = 0;
    // 同じ種類の中身が出ていれば、位置とボタンで変えた倍率を保つ (保存・テーマの切り替え・画像の更新)。全体表示のままなら
    // 新しい大きさに合わせ直す。
    // commands は本文が同じデバイスで記録した drawing のコマンドリストで、記録し直さずに使う (文字の組みが重い)。
    virtual void SetDrawing(std::shared_ptr<const mermaid_util::DiagramDrawing> drawing, Microsoft::WRL::ComPtr<ID2D1CommandList> commands) = 0;
    // bitmap は本体の描画デバイスで作ったもの。pixel_width / pixel_height は元の画像の画素数で、大きな画像は bitmap を縮めてある。
    // 100% は本文と同じく、画像の 1 px を物理 1 px で描く。
    virtual void SetImage(Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap, UINT pixel_width, UINT pixel_height) = 0;
    // 出している画像の GPU のメモリを先に返す (読み直した画像を載せる前)。位置と倍率は保ち、次の SetImage まで画像を描かない。
    virtual void ReleaseImage() = 0;
    // 本体の描画デバイスを作り直した。次の描画から新しいデバイスで描く。画像は旧デバイスのものなので外し、次の SetImage を待つ。
    virtual void SetDevice(ID3D11Device* d3d, ID2D1Device* device) = 0;
};

// 拡大表示の窓。図の描画命令か画像を倍率を変えて描き、右上のボタン・ホイール (スクロール)・ドラッグ・キーで操作する。
// PopupWindow はキャプチャを取り、ホイールを本体へ渡し、非アクティブで閉じるので、見ながら操作するこの窓には使えない。
class ZoomWindow final : public IZoomWindow {
public:
    // owner の中央に、owner の 9 割の大きさで「読み込み中」として開く。on_close は ×・Esc で呼ぶ (呼ばれた側がこれを壊す)。
    // 開けなければ null。d3d と device は本体の描画デバイスで、この窓のスワップチェーンを d3d に作り device で描く。
    // 別のデバイス (既定の HwndRenderTarget) で描いていた頃は、開くたびにデバイスの作成で最初の描画が遅れ、ドライバのデバイスの
    // 分の commit が閉じた後も残った。数式の形 (MathDrawing::Shape) はファクトリごとに作るので、本体と同じファクトリで
    // 描くと本体と交互に作り直さない。本体のデバイスで作った画像のビットマップも、そのまま描ける。
    static std::unique_ptr<IZoomWindow> Open(HWND owner, ID3D11Device* d3d, ID2D1Device* device, IDWriteFactory* dw, IWICImagingFactory* wic,
                                             bool dark, std::wstring_view title, std::move_only_function<void()> on_close);
    // 窓も壊す。
    ~ZoomWindow() override;
    ZoomWindow(const ZoomWindow&) = delete;
    ZoomWindow& operator=(const ZoomWindow&) = delete;

    bool IsOpen() const noexcept override
    {
        return hwnd_ != nullptr;
    }
    // 画像を外している間 (デバイスの作り直し・読み直し) は false で、読み込み中の文言を出す。
    bool HasContent() const noexcept override
    {
        const auto* image = std::get_if<ImageContent>(&content_);
        return image ? image->bitmap != nullptr : std::holds_alternative<DiagramContent>(content_);
    }
    void BringToFront() const noexcept override;
    void SetTitle(std::wstring_view title) override;
    void ApplyLanguage() override;
    void SetDarkMode(bool dark) override;
    void ShowLoading() override;
    void SetDrawing(std::shared_ptr<const mermaid_util::DiagramDrawing> drawing, Microsoft::WRL::ComPtr<ID2D1CommandList> commands) override;
    void SetImage(Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap, UINT pixel_width, UINT pixel_height) override;
    void ReleaseImage() override;
    void SetDevice(ID3D11Device* d3d, ID2D1Device* device) override;

private:
    static constexpr UINT_PTR kTooltipTimer = 2;
    // 並びは右上のバーの左から。
    enum class Button : uint8_t {
        None,
        ZoomOut,
        ZoomIn,
        Fit
    };
    struct DiagramContent {
        std::shared_ptr<const mermaid_util::DiagramDrawing> drawing;
        // 描画先と同じデバイスで記録したもの。描画先を作り直したら捨て、次の描画で drawing から記録し直す。
        Microsoft::WRL::ComPtr<ID2D1CommandList> commands;
    };
    struct ImageContent {
        // 本体のデバイスで作ったもの。デバイスが変わったら外し、ZoomViewer が読み直して渡すまで何も描かない。
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        UINT pixel_width = 0;
        UINT pixel_height = 0;
        // 縮小表示で、bitmap を画面に出す画素の数へ前もって高品質に縮めたもの (ScaledImage)。高品質の縮小は描くたびに
        // かけ直すので、スクロールやドラッグのたびに縮めないよう、メモリを使って持つ。
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> scaled;
    };

    ZoomWindow(ID3D11Device* d3d, ID2D1Device* device, IDWriteFactory* dw, IWICImagingFactory* wic, std::move_only_function<void()> on_close);
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    // 扱ったメッセージは true を返し、result に戻り値を入れる。
    bool HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam, LRESULT& result);

    bool EnsureTarget();
    // 描画先とスワップチェーンを捨てる (デバイスロストとデバイスの差し替え)。次の描画で作り直す。
    void ReleaseTarget() noexcept;
    void Paint();
    // 中身を倍率 1 で左上を原点に描く (変換は呼び出し側が掛ける)。
    void PaintContent(ID2D1DeviceContext* rt);
    // 画像を画面に出す画素の数が bitmap より少ない (縮小する) とき、その大きさへ縮めたもの。倍率が変わったら作り直し、
    // 縮小しない倍率になったら捨てる。窓の大きさを変えている間は作らない。作らなければ null。
    ID2D1Bitmap* ScaledImage(ImageContent& image);
    // 中身の大きさが決まっている。画像を外している間も、位置と倍率を保つために true のまま。
    bool HasSize() const noexcept
    {
        return !std::holds_alternative<std::monostate>(content_);
    }
    void PaintBar();
    // 倍率 1 の中身の大きさ (DIP)。画像は今の DPI で物理 1 px が画像の 1 px になる大きさ。
    D2D1_SIZE_F NaturalSize() const noexcept;
    // 窓と中身の大きさから全体表示の倍率と、スクロールの範囲を決め直す。
    void Layout();
    void UpdateScrollBars();
    // 中身の左上を置く位置 (DIP)。中身が窓より小さい向きは中央に置く。
    D2D1_POINT_2F Origin() const noexcept;
    void ScrollTo(float x, float y);
    void ScrollBy(float dx, float dy)
    {
        ScrollTo(scroll_x_ + dx, scroll_y_ + dy);
    }
    // スクロールバーと同じ送り (SB_LINEUP など) で動かす。キーもこれを使う。
    void Scroll(int bar, WORD request);
    void Step(int dir);
    void Fit();
    float Zoom() const noexcept
    {
        return manual_zoom_.value_or(fit_zoom_);
    }
    // 窓の中央に見えている点を動かさずに倍率を変える。
    void SetZoom(float zoom);
    // 中身を差し替える。同じ種類なら位置とボタンで変えた倍率を保ち、違えば全体表示にする。
    template <class Content>
    void Replace(Content content)
    {
        const bool keep = std::holds_alternative<Content>(content_);
        content_ = std::move(content);
        if (keep) {
            Layout();
        }
        else {
            Fit();
        }
    }
    Button HitButton(float x, float y) const noexcept;
    D2D1_RECT_F ButtonRect(Button button) const noexcept;
    D2D1_RECT_F BarRect() const noexcept;
    bool IsEnabled(Button button) const noexcept;
    // 押していない間に、ポインタの下のボタンを button にする。
    void HoverAt(Button button);
    // 塗るボタン (ポインタの下か、押している)。
    Button HighlightedButton() const noexcept;
    void Invalidate() const noexcept
    {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    float Scale() const noexcept;
    D2D1_POINT_2F ToDip(LPARAM lparam) const noexcept;

    // 窓を作り終えるまでと、本体と一緒に壊された後は null。
    HWND hwnd_ = nullptr;
    std::move_only_function<void()> on_close_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dw_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
    Microsoft::WRL::ComPtr<ID3D11Device> d3d_;
    Microsoft::WRL::ComPtr<ID2D1Device> device_;
    WindowSwapTarget target_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    // 図の記録と、画像の縮小版の描画に使う DPI 96 の DeviceContext。描画先と同じデバイスから作り、描画先を作り直したら捨てる。
    Microsoft::WRL::ComPtr<ID2D1DeviceContext5> record_dc_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_label_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_symbol_;

    // 読み込み中は monostate。
    std::variant<std::monostate, DiagramContent, ImageContent> content_;
    bool dark_ = false;
    Tooltip tooltip_;

    // 大きさと位置は DIP。
    float view_width_ = 0.0f;
    float view_height_ = 0.0f;
    float fit_zoom_ = 1.0f;
    // ボタンやキーで変えた倍率。無ければ全体表示で、窓の大きさに追従する。
    std::optional<float> manual_zoom_;
    // 中身を余白込みで並べた面の、窓の左上が指す位置。
    float scroll_x_ = 0.0f;
    float scroll_y_ = 0.0f;
    // スクロールバーの出し入れで WM_SIZE から Layout に戻ってくる間は、外側の Layout に任せる。
    bool laying_out_ = false;
    // 窓の移動か大きさの変更の最中 (WM_ENTERSIZEMOVE〜WM_EXITSIZEMOVE)。全体表示の倍率が 1 px ごとに変わるので、
    // 画像の縮小版は終えてから作る。
    bool sizing_ = false;

    // ポインタの段階。押している間 (Pressing / Dragging) はキャプチャを持つ。
    struct PointerIdle {};
    // 押した後も同じボタンの上ならツールチップを出し直さない (HoverAt は同じボタンでは何もしない)。
    struct PointerOver {
        Button button = Button::None;
    };
    // 離したときに同じボタンの上なら押したことにする。
    struct PointerPressing {
        Button button = Button::None;
    };
    // 中身をつかんで動かしている。last は直前のポインタの位置で、動いた分だけスクロールする
    // (ドラッグ中にホイール・キー・倍率の変更・保存で動いた位置を、次の移動で戻さない)。
    struct PointerDragging {
        D2D1_POINT_2F last{};
    };
    std::variant<PointerIdle, PointerOver, PointerPressing, PointerDragging> pointer_;
    bool PointerDown() const noexcept
    {
        return std::holds_alternative<PointerPressing>(pointer_) || std::holds_alternative<PointerDragging>(pointer_);
    }
    bool tracking_leave_ = false;
};

// 拡大表示の窓 1 枚と、その中身 (図か画像) の取り寄せ。窓は図と画像で使い回し、別のものを開くと中身を差し替える (図の窓を
// 1 枚にしたのと揃える)。図は本文と同じ結果 (MermaidRenderer) を出す。画像は本文のビットマップが本文の幅に
// 縮めたもので、見えている範囲の分しか持たないので、開いたときに原寸で読み直し、閉じたら捨てる。必要な部分だけ読む
// ID2D1ImageSourceFromWic は、縮小表示で全部を読まないための作りが重いので、原寸のビットマップ 1 枚を選んだ。
// パブリックメソッドはすべて UI スレッドから呼ぶ。
class ZoomViewer {
public:
    // 窓を開く。開けなければ null。本番は ZoomWindow::Open で、テストは窓を作らない実装を返す。
    using WindowFactory = std::unique_ptr<IZoomWindow> (*)(HWND owner, ID3D11Device* d3d, ID2D1Device* device, IDWriteFactory* dw,
                                                           IWICImagingFactory* wic, bool dark, std::wstring_view title,
                                                           std::move_only_function<void()> on_close);
    // ID3D11Device をヘッダに持ち込まないよう、作る・壊すは zoom_viewer.cpp で行う。
    ZoomViewer();
    ~ZoomViewer();
    ZoomViewer(const ZoomViewer&) = delete;
    ZoomViewer& operator=(const ZoomViewer&) = delete;

    // App に頼むこと。窓に投げるメッセージの番号は App が決め、ここでは知らない。wake のほかは空にしない。
    struct Hooks {
        // 待っている図を頼み直させる (本文の図と同じ回の中で RequestDiagram を呼ばせる)。UI スレッドで呼ぶ。
        std::move_only_function<void()> on_update;
        // 読み終えた画像が溜まり始めたときに worker で呼ぶ。UI スレッドに OnImageDecoded を呼ばせるだけにする。空なら呼ばない。
        WorkerWake wake;
        // 開けなかったことを文言で知らせる。UI スレッドで呼ぶ。
        std::move_only_function<void(std::wstring_view)> on_failed;
    };
    // hwnd は本体の窓で、開く窓の持ち主にする。
    void Init(HWND hwnd, IDWriteFactory* dw, IWICImagingFactory* wic, TaskScheduler& scheduler, WindowFactory open_window, Hooks hooks);
    // 本体の描画先とそのデバイス。窓はこのデバイスで描き、画像のビットマップは rt で作る。作り直しの再試行中は null。
    void SetRenderTarget(ID2D1RenderTarget* rt, ID3D11Device* d3d, ID2D1Device* device);
    // 窓を閉じ、読んでいる画像を待つ。終了前に呼ぶ。
    void Shutdown();

    bool IsOpen() const noexcept
    {
        return window_ && window_->IsOpen();
    }
    // 図を開く。開いていれば中身を差し替え、同じ図なら前面に出すだけ。code (UTF-8) は内部でコピーする。
    // node は呼び出し側の図の目印で、窓と一緒に消える (DiagramNode)。
    void OpenDiagram(std::string_view code, int node, bool dark_mode);
    // 画像 (絶対パス) を開く。開いていれば中身を差し替え、同じ画像なら前面に出すだけ。
    void OpenImage(std::wstring path, bool dark_mode);
    // 窓を壊し、読んでいる画像の結果を捨てる。頼んだ図の描画は止めず、結果は本文と同じキャッシュに残る。
    void Close();
    // 開いていれば、倍率と位置を保って新しいテーマにする (図は新しいテーマの図に差し替える)。
    void SetDarkMode(bool dark_mode);
    void ApplyLanguage();

    // 開いている図のコード。図を出していなければ nullopt (図を空にして保存すると、開いていてもコードは空になる)。
    std::optional<std::string_view> DiagramCode() const noexcept;
    // 開いている図の目印。図を出していなければ -1。読み直しで図を見失ったら SetDiagramNode(-1) にする。
    int DiagramNode() const noexcept;
    void SetDiagramNode(int node) noexcept;
    // 保存で図が変わったら、位置とボタンで変えた倍率を保って差し替える。描けなければ今の図を残す。図を出していない・同じコードなら何もしない。
    void UpdateDiagramCode(std::string_view code);
    // 待っている図を mermaid に頼み、結果が出ていれば窓に渡す。待っていなければ何もしない。本文の図と同じ所から、本文の幅と
    // テーマで呼ぶ (頼んでよいかの判断と、本文と同じ要求になる幅を 1 か所で決める)。
    void RequestDiagram(MermaidRenderer& mermaid, float max_width, bool dark_mode);

    // Hooks::wake で頼まれた。
    void OnImageDecoded();
    // 本文中の画像のファイルが更新された。開いている画像なら、位置と倍率を保って読み直す (読めなければ前の絵を残す)。
    void OnImageFilesChanged(std::span<const std::wstring> paths);

private:
    struct DiagramSubject {
        // 最後に要求した図。描けなかったコードも入るので、出ている絵のコードとは限らない。
        std::string code;
        int node = -1;
        // code の図の結果をまだ窓に渡していない。
        bool pending = false;
    };
    struct ImageSubject {
        std::wstring path;
    };
    using Subject = std::variant<std::monostate, DiagramSubject, ImageSubject>;
    struct DecodeResult {
        // 頼んだときの generation_。違えば閉じたか別のものを開いたか、読み直しを頼み直した。
        uint32_t generation = 0;
        FullSizeImage image;
    };

    // 開いていて、出しているものが T なら返す。
    template <class T, class Self>
    auto* Shown(this Self& self) noexcept
    {
        return self.IsOpen() ? std::get_if<T>(&self.subject_) : nullptr;
    }
    // 中身を subject に替え、窓を読み込み中にする (閉じていれば開く)。開けなければ知らせて false。
    bool Show(Subject subject, bool dark_mode);
    void Fail();
    // 結果が使えなかった。出ている絵があれば残し (書き換えの途中で読んだ・描けなかったことが多い)、無ければ閉じて知らせる。
    void FailIfEmpty();
    std::wstring Title() const;
    // 図の結果を待つ印を付け、Hooks::on_update で頼ませる (印だけだと窓が読み込み中のまま残る)。
    void AwaitDiagram();
    void StartDecode();
    // 読み終えた画像を GPU に載せて窓に渡す。描画先が無ければ捨てる (戻ったときに SetRenderTarget が読み直す)。
    void Upload(FullSizeImage image);

    HWND hwnd_ = nullptr;
    Microsoft::WRL::ComPtr<IDWriteFactory> dw_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
    TaskScheduler* scheduler_ = nullptr;
    WindowFactory open_window_ = nullptr;
    Hooks hooks_;

    ID2D1RenderTarget* rt_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> d3d_;
    Microsoft::WRL::ComPtr<ID2D1Device> device_;

    std::unique_ptr<IZoomWindow> window_;
    // 窓に出しているもの。閉じていれば monostate。
    Subject subject_;
    // 中身を替える・閉じる・読み直しを頼むたびに進め、それより前に頼んだ画像の読み込みを捨てる (worker も読む)。
    std::atomic<uint32_t> generation_{ 0 };
    PostedInbox<DecodeResult> decoded_;
    WorkerLatch latch_;
};

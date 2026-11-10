#pragma once
#include "mermaid_renderer_interface.h"
#include "win_handle.h"
#include <windows.h>
#include <wincodec.h>
#include <functional>
#include <memory>
#include <string_view>

class Document;
namespace mermaid_util {
struct DiagramDrawing;
}

// MD ペインのオーバーレイボタンから発火する 3 つのクリップボード操作を集約する。
//   - コードブロックのテキストコピー
//   - Mermaid ダイアグラムの PNG 保存 (ファイルダイアログ経由)
//   - Mermaid ダイアグラムのクリップボードコピー (image/svg+xml + 画像 (CF_DIB))
// 通知 (toast) は App 側のコールバックに委譲し、 ClipboardManager は effect を直接触らない。
class ClipboardManager {
public:
    using ToastCallback = std::function<void(std::wstring_view)>;

    // show_toast は呼び出し側で必ず設定する想定。Init を呼び忘れた場合は
    // ガード不要にするためデフォルトは no-op。wic はアプリ共有のファクトリ (非所有)。
    void Init(HWND hwnd, IMermaidRenderer* mermaid_renderer, IWICImagingFactory* wic, ToastCallback show_toast) noexcept;

    void CopyCodeBlock(const Document& doc, int node_index, bool dark) const;
    // drawing は図の描画命令 (DiagramEntry::drawing)。null なら何もしない。md_width は本文の幅 (DIP) で、収まらない図は
    // 本文と同じくその幅まで縮めた大きさの画像にする。画像は保存先を選んだ後に描く (取り消したら描かない)。
    void SaveDiagramAsPng(std::shared_ptr<const mermaid_util::DiagramDrawing> drawing, float md_width) const;
    // drawing から描いた画像を CF_DIB に、SVG を image/svg+xml に書く。画像は md_width まで縮め、SVG は縮めない大きさ。
    void CopyDiagramToClipboard(std::shared_ptr<const mermaid_util::DiagramDrawing> drawing, float md_width) const;

private:
    // CF_DIB 用 GlobalMem を作る。失敗/空は空を返す。
    UniqueGlobalMem BuildDib(IWICBitmapSource* bitmap) const;

    HWND hwnd_ = nullptr;
    IMermaidRenderer* mermaid_renderer_ = nullptr;
    IWICImagingFactory* wic_ = nullptr; // アプリ共有ファクトリ (非所有)
    ToastCallback show_toast_ = [](std::wstring_view) static {};
};

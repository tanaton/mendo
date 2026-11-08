#pragma once
#include "transposition_cache.h"
#include "mermaid_renderer_interface.h"
#include "win_handle.h"
#include <windows.h>
#include <wincodec.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

class Document;
namespace mermaid_util {
struct DiagramDrawing;
}

// MD ペインのオーバーレイボタンから発火する 3 つのクリップボード操作を集約する。
//   - コードブロックのテキストコピー
//   - Mermaid ダイアグラムの PNG 保存 (ファイルダイアログ経由)
//   - Mermaid ダイアグラムのクリップボードコピー (image/svg+xml + 画像 (CF_DIB))
//       SVG は非同期取得のためキャッシュ付き。
// SVG キャッシュは閲覧 1 ファイルで数十～100 entries 想定。 上限 128 件。
// 通知 (toast) は App 側のコールバックに委譲し、 ClipboardManager は effect を直接触らない。
class ClipboardManager {
public:
    using ToastCallback = std::function<void(std::wstring_view)>;

    // show_toast は呼び出し側で必ず設定する想定。Init を呼び忘れた場合は
    // ガード不要にするためデフォルトは no-op。wic はアプリ共有のファクトリ (非所有)。
    void Init(HWND hwnd, IMermaidRenderer* mermaid_renderer, IWICImagingFactory* wic, ToastCallback show_toast) noexcept;

    void CopyCodeBlock(const Document& doc, int node_index, bool dark) const;
    // drawing は図の描画命令 (DiagramEntry::drawing)。null なら何もしない。画像は保存先を選んだ後に描く (取り消したら描かない)。
    void SaveDiagramAsPng(const Document& doc, int node_index, std::shared_ptr<const mermaid_util::DiagramDrawing> drawing) const;
    // drawing から描いた画像を CF_DIB に、SVG を image/svg+xml に書く。
    void CopyDiagramToClipboard(const Document& doc, int node_index, std::shared_ptr<const mermaid_util::DiagramDrawing> drawing, float md_width,
                                bool dark);

private:
    // CF_DIB 用 GlobalMem を作る。失敗/空は空を返す。
    UniqueGlobalMem BuildDib(IWICBitmapSource* bitmap) const;
    // コピー成否に応じたトーストを出す (CopyDiagramToClipboard の各経路共通)。
    void EmitCopyResult(bool ok) const;

    HWND hwnd_ = nullptr;
    IMermaidRenderer* mermaid_renderer_ = nullptr;
    IWICImagingFactory* wic_ = nullptr; // アプリ共有ファクトリ (非所有)
    ToastCallback show_toast_ = [](std::wstring_view) static {};

    static constexpr size_t MAX_SVG_CACHE_ENTRIES = 128;
    TranspositionCache<uint64_t, std::string, MAX_SVG_CACHE_ENTRIES> svg_cache_;
    // SVG を待っているコピーの図 (NodeDiagramHash)。届いた結果は、この図を待っているときだけ書く (待つ間に別のコピーが
    // 書き込むか、置き換えた)。同じ図を待っている間に押し直しても頼み直さない。同じ図の結果は同じなので、どの要求の結果でもよい。
    std::optional<uint64_t> pending_copy_;
};

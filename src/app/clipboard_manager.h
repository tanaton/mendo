#pragma once
#include "transposition_cache.h"
#include "mermaid_renderer_interface.h"
#include "utility.h"
#include "win_handle.h"
#include <windows.h>
#include <wincodec.h>
#include <cstdint>
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
    TranspositionCache<uint64_t, std::wstring, MAX_SVG_CACHE_ENTRIES> svg_cache_;
    bool copy_in_flight_ = false;
    // 書き込みを行う(または開始する)コピーごとに増やす世代。保留中の非同期 SVG コールバックが
    // 後発コピーを上書きしないよう、コールバックは捕捉した世代と現在値が一致する時のみ書き込む。
    uint64_t copy_generation_ = 0;
};

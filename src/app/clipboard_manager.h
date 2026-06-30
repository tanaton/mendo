#pragma once
#include "lru_cache.h"
#include "mermaid_renderer_interface.h"
#include "win_handle.h"
#include <windows.h>
#include <wincodec.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <memory_resource>
#include <string_view>
#include <vector>

class Document;
class MermaidFileCache;

// MD ペインのオーバーレイボタンから発火する 3 つのクリップボード操作を集約する。
//   - コードブロックのテキストコピー
//   - Mermaid 等ダイアグラムの PNG 保存 (ファイルダイアログ経由)
//   - Mermaid 等ダイアグラムのクリップボードコピー
//       Mermaid: image/svg+xml + 画像(CF_DIB) / LaTeX: 画像(CF_DIB) のみ。
//       SVG は非同期取得のため LRU キャッシュ付き。
// SVG キャッシュは閲覧 1 ファイルで数十～100 entries 想定。 上限 128 で LRU。
// 通知 (toast) は App 側のコールバックに委譲し、 ClipboardManager は i18n も
// effect も直接触らない。
class ClipboardManager {
public:
    using ToastCallback = std::function<void(std::wstring_view)>;

    // show_toast は呼び出し側で必ず設定する想定。Init を呼び忘れた場合は
    // ガード不要にするためデフォルトは no-op。wic はアプリ共有のファクトリ (非所有)。
    void Init(HWND hwnd, MermaidFileCache* file_cache, IMermaidRenderer* mermaid_renderer,
              IWICImagingFactory* wic, ToastCallback show_toast) noexcept;

    void CopyCodeBlock(const Document& doc, int node_index, bool dark) const;
    void SaveDiagramAsPng(const Document& doc, int node_index, float md_width, bool dark);
    // png は表示中ビットマップと同寿命の元 PNG (DiagramEntry::png)。コピーボタンの表示条件と
    // 一致するため、非同期/退避され得る file_cache に依存せず確実に画像を載せられる。
    void CopyDiagramToClipboard(const Document& doc, int node_index,
                                std::shared_ptr<const std::pmr::vector<uint8_t>> png,
                                float md_width, bool dark);

private:
    using PngBytes = std::shared_ptr<const std::pmr::vector<uint8_t>>;
    // PNG をデコードして CF_DIB 用 GlobalMem を作る。デコードはコピー時のみ実行。失敗/空は空を返す。
    UniqueGlobalMem BuildDib(const PngBytes& png) const;
    // コピー成否に応じたトーストを出す (CopyDiagramToClipboard の各経路共通)。
    void EmitCopyResult(bool ok) const;

    HWND hwnd_ = nullptr;
    MermaidFileCache* file_cache_ = nullptr;
    IMermaidRenderer* mermaid_renderer_ = nullptr;
    IWICImagingFactory* wic_ = nullptr; // アプリ共有ファクトリ (非所有)
    ToastCallback show_toast_ = [](std::wstring_view) {};

    static constexpr size_t MAX_SVG_CACHE_ENTRIES = 128;
    LruCache<uint64_t, std::pmr::wstring, MAX_SVG_CACHE_ENTRIES> svg_cache_;
    bool copy_in_flight_ = false;
    // 書き込みを行う(または開始する)コピーごとに増やす世代。保留中の非同期 SVG コールバックが
    // 後発コピーを上書きしないよう、コールバックは捕捉した世代と現在値が一致する時のみ書き込む。
    uint64_t copy_generation_ = 0;
};

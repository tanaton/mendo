#pragma once
#include "lru_cache.h"
#include "mermaid_renderer_interface.h"
#include <windows.h>
#include <functional>
#include <memory_resource>
#include <string_view>

class Document;
class MermaidFileCache;

// MD ペインのオーバーレイボタンから発火する 3 つのクリップボード操作を集約する。
//   - コードブロックのテキストコピー
//   - Mermaid 等ダイアグラムの PNG 保存 (ファイルダイアログ経由)
//   - Mermaid 等ダイアグラムの SVG コピー (非同期、LRU キャッシュ付き)
// SVG キャッシュは閲覧 1 ファイルで数十～100 entries 想定。 上限 128 で LRU。
// 通知 (toast) は App 側のコールバックに委譲し、 ClipboardManager は i18n も
// effect も直接触らない。
class ClipboardManager {
public:
    using ToastCallback = std::function<void(std::wstring_view)>;

    // show_toast は呼び出し側で必ず設定する想定。Init を呼び忘れた場合は
    // ガード不要にするためデフォルトは no-op。
    void Init(HWND hwnd, MermaidFileCache* file_cache, IMermaidRenderer* mermaid_renderer, ToastCallback show_toast) noexcept;

    void CopyCodeBlock(const Document& doc, int node_index, bool dark) const;
    void SaveDiagramAsPng(const Document& doc, int node_index, float md_width, bool dark);
    void CopyDiagramAsSvg(const Document& doc, int node_index, float md_width, bool dark);

private:
    HWND hwnd_ = nullptr;
    MermaidFileCache* file_cache_ = nullptr;
    IMermaidRenderer* mermaid_renderer_ = nullptr;
    ToastCallback show_toast_ = [](std::wstring_view) {};

    static constexpr size_t MAX_SVG_CACHE_ENTRIES = 128;
    LruCache<uint64_t, std::pmr::wstring, MAX_SVG_CACHE_ENTRIES> svg_cache_;
    bool svg_copy_in_flight_ = false;
};

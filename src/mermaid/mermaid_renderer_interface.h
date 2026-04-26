#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include <functional>
#include <string>
#include <string_view>
#include <memory_resource>

// MermaidRenderer のうち ResourceManager から使う最小 API を抽出した interface。
// mermaid.h は <WebView2.h> を transitively 引き込むため、mendo_core 入りの
// モジュールは WebView2 依存を避けるべく本 interface を介する。
class IMermaidRenderer {
public:
    using Callback = std::move_only_function<void()>;
    // SVG 取得結果コールバック。空文字列なら失敗。
    using SvgCallback = std::move_only_function<void(std::pmr::wstring svg)>;
    virtual ~IMermaidRenderer() = default;

    virtual void RequestRender(Node& node, NodeLayoutEntry& layout_entry,
        DiagramEntry& diagram_entry, float max_width, bool dark_mode,
        Callback on_complete) = 0;
    // SVG をクリップボード用に取得する。PNG レンダリングと同じワーカー枠を共有する。
    // code は内部でコピーされるため呼び出し後に解放してよい。
    virtual void RequestSvg(std::wstring_view code, bool dark_mode, SvgCallback callback) = 0;
    virtual void CancelPending() = 0;
    virtual void ClearCache() = 0;
};

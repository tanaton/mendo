#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include <functional>
#include <string>
#include <string_view>
#include <memory_resource>

// mermaid.h は <WebView2.h> を transitively 引き込むため、mendo_core 入りの
// モジュールは WebView2 依存を避けるべく本 interface を介する。
class IMermaidRenderer {
public:
    using Callback = std::move_only_function<void()>;
    // SVG 取得結果コールバック。
    //   svg が非空      : 成功
    //   svg が空, cancelled=false : mermaid のレンダリング失敗
    //   svg が空, cancelled=true  : CancelPending 経由（テーマ変更等のキャンセル）
    using SvgCallback = std::move_only_function<void(std::pmr::wstring svg, bool cancelled)>;
    virtual ~IMermaidRenderer() = default;

    virtual void RequestRender(Node& node, NodeLayoutEntry& layout_entry,
                               DiagramEntry& diagram_entry, float max_width, bool dark_mode,
                               Callback on_complete) = 0;
    // PNG レンダリングと同じワーカー枠を共有する。
    // code は内部でコピーされるため呼び出し後に解放してよい。
    // max_width は CSS ビューポート幅（DIP）。表示中の図と同じ折返し結果を得るため、PNG と同じ値を渡す。
    virtual void RequestSvg(std::wstring_view code, float max_width, bool dark_mode, SvgCallback callback) = 0;
    virtual void CancelPending() = 0;
    virtual void ClearCache() = 0;
};

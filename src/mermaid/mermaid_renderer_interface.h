#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include <functional>

// MermaidRenderer のうち ResourceManager から使う最小 API を抽出した interface。
// mermaid.h は <WebView2.h> を transitively 引き込むため、mendo_core 入りの
// モジュールは WebView2 依存を避けるべく本 interface を介する。
class IMermaidRenderer {
public:
    using Callback = std::move_only_function<void()>;
    virtual ~IMermaidRenderer() = default;

    // Mermaid コードブロックのレンダリングを要求する。完了時、
    // diagram_entry.bitmap/width/height と layout_entry.height/layout_dirty が
    // 設定され、on_complete が UI スレッドで呼び出される。
    virtual void RequestRender(Node& node, NodeLayoutEntry& layout_entry,
        DiagramEntry& diagram_entry, float max_width, bool dark_mode,
        Callback on_complete) = 0;

    // 保留中のリクエストをすべてキャンセルし、処理中のリクエストを無効化する。
    virtual void CancelPending() = 0;

    // キャッシュされたビットマップをすべてクリアする。
    virtual void ClearCache() = 0;
};

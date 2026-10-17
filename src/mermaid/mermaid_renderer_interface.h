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
    // SVG 取得結果コールバック。
    //   svg が非空      : 成功
    //   svg が空, cancelled=false : mermaid のレンダリング失敗
    //   svg が空, cancelled=true  : 終了時に破棄された
    using SvgCallback = std::move_only_function<void(std::pmr::wstring svg, bool cancelled)>;
    virtual ~IMermaidRenderer() = default;

    // 結果 (ビットマップかエラー) が出ていれば diagram_entry に書いて true を返す。まだなら描画を始めて false
    // (始めていれば何もしない)。結果が出たら初期化時に渡した通知で知らせるので、呼び出し側は要求し直す。
    // 要求と結果はコードのハッシュで引くので、ノードや diagram_entry を持ち続けない。
    // ノードの高さは呼び出し側が寸法から決める。
    virtual bool RequestRender(const Node& node, DiagramEntry& diagram_entry, float max_width, bool dark_mode) = 0;
    // PNG レンダリングと同じワーカー枠を共有する。
    // code (UTF-8) は内部でコピーされるため呼び出し後に解放してよい。
    // max_width は CSS ビューポート幅（DIP）。表示中の図と同じ折返し結果を得るため、PNG と同じ値を渡す。
    virtual void RequestSvg(std::string_view code, float max_width, bool dark_mode, SvgCallback callback) = 0;
    // 描画待ちの表示用要求だけを捨てる (描画中と SVG 要求は維持)。大きく移動した後や幅・テーマの変更後に、
    // 不要になった図が先に描かれて可視の図が待たされるのを防ぐ。捨てた図は再要求される。
    virtual void DropQueued()
    {}
};

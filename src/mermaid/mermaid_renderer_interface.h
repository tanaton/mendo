#pragma once
#include <functional>
#include <string>
#include <string_view>

struct DiagramEntry;
struct Node;

// MermaidRenderer は exe の埋め込みリソースを読むので mendo_core に入れない。mendo_core 入りのモジュールは本 interface を介する。
class IMermaidRenderer {
public:
    // SVG 取得結果コールバック。
    //   svg が非空      : 成功
    //   svg が空, cancelled=false : 描けなかった
    //   svg が空, cancelled=true  : 終了時に破棄された
    // svg は MB 級になるので写さずに渡す。呼び出しの間だけ有効。
    using SvgCallback = std::move_only_function<void(std::wstring_view svg, bool cancelled)>;
    virtual ~IMermaidRenderer() = default;

    // 結果 (描画命令かエラー) が出ていれば diagram_entry に書いて true を返す。まだなら描画を始めて false
    // (始めていれば何もしない)。結果が出たら初期化時に渡した通知で知らせるので、呼び出し側は要求し直す。
    // 要求と結果はコードのハッシュで引くので、ノードや diagram_entry を持ち続けない。
    // ノードの高さは呼び出し側が寸法から決める。
    virtual bool RequestRender(const Node& node, DiagramEntry& diagram_entry, float max_width, bool dark_mode) = 0;
    // クリップボード用の SVG。本文の図と同じく bg のワーカーで描き、本文の幅には縮めない。
    // code (UTF-8) は内部でコピーされるため呼び出し後に解放してよい。
    // max_width は CSS ビューポート幅（DIP）。表示中の図と同じ折返し結果を得るため、本文と同じ値を渡す。
    virtual void RequestSvg(std::string_view code, float max_width, bool dark_mode, SvgCallback callback) = 0;
    // 描画待ちの表示用要求だけを捨てる (描画中と、SVG・インライン数式の要求は維持)。大きく移動した後や幅・テーマの変更後に、
    // 不要になった図が先に描かれて可視の図が待たされるのを防ぐ。捨てた図は再要求される。
    virtual void DropQueued()
    {}
};

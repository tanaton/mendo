#pragma once
#include <wincodec.h>
#include <wrl/client.h>
#include <functional>
#include <string>
#include <string_view>

struct DiagramEntry;
struct Node;
namespace mermaid_util {
struct DiagramDrawing;
}

// ResourceManager と ClipboardManager は本 interface を介し、テストでは pele を動かさない mock を渡す。
class IMermaidRenderer {
public:
    // SVG (UTF-8) の取得結果。空なら描けなかった。終了で取り消した要求では呼ばない。
    // svg は MB 級になるので写さずに渡す。呼び出しの間だけ有効。
    using SvgCallback = std::move_only_function<void(std::string_view svg)>;
    virtual ~IMermaidRenderer() = default;

    // 結果 (描画命令かエラー) が出ていれば diagram_entry に書いて true を返す。まだなら描画を始めて false
    // (始めていれば何もしない)。結果が出たら初期化時に渡した通知で知らせるので、呼び出し側は要求し直す。
    // 要求と結果はコードのハッシュで引くので、ノードや diagram_entry を持ち続けない。
    // ノードの高さは呼び出し側が寸法から決める。
    virtual bool RequestRender(const Node& node, DiagramEntry& diagram_entry, float max_width, bool dark_mode) = 0;
    // クリップボード用の SVG。本文の図と同じく bg のワーカーで描き、本文の幅には縮めない。
    // code (UTF-8) は内部でコピーされるため呼び出し後に解放してよい。
    // max_width は本文の幅 (DIP)。表示中の図と同じ折返し結果を得るため、本文と同じ値を渡す。
    virtual void RequestSvg(std::string_view code, float max_width, bool dark_mode, SvgCallback callback) = 0;
    // 描画待ちと、投入済みでまだ始まっていない表示用の要求 (ディスクの読み込みを含む) を捨てる (描画中と、SVG・インライン数式・
    // 拡大表示の要求は維持)。大きく移動した後や幅・テーマの変更後に、不要になった図が先に描かれて可視の図が待たされるのを防ぐ。
    // 捨てた図は再要求される。
    virtual void DropQueued()
    {}
    // 図の保存・コピーに使う画像を描く。描けなければ null。
    virtual Microsoft::WRL::ComPtr<IWICBitmap> DiagramBitmap(const mermaid_util::DiagramDrawing& /*drawing*/)
    {
        return nullptr;
    }
};

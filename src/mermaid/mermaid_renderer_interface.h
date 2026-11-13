#pragma once
#include <wincodec.h>
#include <wrl/client.h>

struct DiagramEntry;
struct ID2D1CommandList;
struct Node;
namespace mermaid_util {
struct DiagramDrawing;
}

// ResourceManager と ClipboardManager は本 interface を介し、テストでは pele を動かさない mock を渡す。
class IMermaidRenderer {
public:
    virtual ~IMermaidRenderer() = default;

    // 結果 (描画命令かエラー) が出ていれば diagram_entry に書いて true を返す。まだなら描画を始めて false
    // (始めていれば何もしない)。結果が出たら初期化時に渡した通知で知らせるので、呼び出し側は要求し直す。
    // 要求と結果はコードのハッシュで引くので、ノードや diagram_entry を持ち続けない。
    // ノードの高さは呼び出し側が寸法から決める。
    virtual bool RequestRender(const Node& node, DiagramEntry& diagram_entry, float max_width, bool dark_mode) = 0;
    // 描画待ちと、投入済みでまだ始まっていない表示用の要求 (ディスクの読み込みを含む) を捨てる (描画中と、インライン数式の
    // 要求は維持)。大きく移動した後や幅・テーマの変更後に、不要になった図が先に描かれて可視の図が待たされるのを防ぐ。
    // 捨てた図は再要求される。
    virtual void DropQueued()
    {}
    // 画面外へ出た図のコマンドリストを、覚えている結果からも外す。次の RequestRender で描画命令から記録し直す。
    virtual void ReleaseCommands(ID2D1CommandList* /*commands*/)
    {}
    // 拡大表示が待っている図を本文と同じ幅・テーマで頼み、結果が出ていれば窓に渡す。待っていなければ何もしない。
    // 本文の図と同じ所から呼ぶ (頼んでよいかの判断と、本文と同じ要求になる幅を 1 か所で決める)。
    virtual void RequestViewerDrawing(float /*max_width*/, bool /*dark_mode*/)
    {}
    // 図の保存・コピーに使う画像を描く。max_width (本文の幅) に収まらない図は、本文と同じくその幅まで縮めた大きさで描く。
    // 描けなければ null。
    virtual Microsoft::WRL::ComPtr<IWICBitmap> DiagramBitmap(const mermaid_util::DiagramDrawing& /*drawing*/, float /*max_width*/)
    {
        return nullptr;
    }
};

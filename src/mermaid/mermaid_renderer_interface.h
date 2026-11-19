#pragma once
#include "mermaid_util.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <optional>

struct DiagramEntry;
struct Node;

// ResourceManager と ClipboardManager は本 interface を介し、テストでは pele を動かさない mock を渡す。
class IMermaidRenderer {
public:
    virtual ~IMermaidRenderer() = default;

    // 結果 (描画命令かエラー) が出ていれば diagram_entry に書いて true、まだなら描画を始めて false (始めていれば何もしない)。
    // 結果が出たら初期化時に渡した通知で知らせるので、呼び出し側は要求し直す。要求と結果はコードのハッシュで引くので、
    // ノードや diagram_entry を持ち続けない。ノードの高さは呼び出し側が寸法から決める。
    virtual bool RequestRender(const Node& node, DiagramEntry& diagram_entry, float max_width, bool dark_mode) = 0;
    // 前に描いた図の縮めない大きさ (CSS px)。文書を開くときと読み直すときに、描く前に図の高さを決めるのに使う。
    // 覚えていなければ nullopt。
    virtual std::optional<mermaid_util::DiagramSize> KnownSize(const Node& /*node*/, float /*max_width*/, bool /*dark_mode*/) const
    {
        return std::nullopt;
    }
    // 描画待ちと、投入済みでまだ始まっていない表示用の要求 (キャッシュの直列化した形の読み込みを含む) を捨て、描いている途中の表示用の描画も
    // 止める (インライン数式の要求は維持)。大きく移動した後や幅・テーマの変更後に、不要な図を先に描いて可視の図を待たせない。
    // 捨てた図は再要求される。
    virtual void DropQueued()
    {}
    // 見えている図から順に RequestRender し直す回の始まり。待ちの表示用の要求を、この回に頼まれた順へ並べ直す (先読みで
    // 先に頼んだ図の後ろで、見えている図を待たせない)。
    virtual void BeginRequestPass()
    {}
    // 画面外へ出た図のコマンドリストと描画命令を、diagram_entry と覚えている結果の両方から外す (片方だけ外してもメモリは
    // 返らない)。寸法と直列化した形は残し、次の RequestRender でそこから読み直す。仮表示中の図は外さない。
    virtual void ReleaseDrawing(DiagramEntry& diagram_entry) = 0;
    // 拡大表示が待っている図を本文と同じ幅・テーマで頼み、結果が出ていれば窓に渡す。待っていなければ何もしない。
    // 本文の図と同じ所から呼ぶ (頼んでよいかの判断と、本文と同じ要求になる幅を 1 か所で決める)。
    virtual void RequestViewerDrawing(float /*max_width*/, bool /*dark_mode*/)
    {}
    // 保存・コピーの画像を描く。max_width (本文の幅) に収まらない図は本文と同じくその幅まで縮めて描く。描けなければ null。
    virtual Microsoft::WRL::ComPtr<IWICBitmap> DiagramBitmap(const mermaid_util::DiagramDrawing& /*drawing*/, float /*max_width*/)
    {
        return nullptr;
    }
};

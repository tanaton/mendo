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

    // 結果 (記録した図かエラー) が出ていれば diagram_entry に書いて true、まだなら文字の組みか描画を始めて false
    // (始めていれば何もしない)。
    // 結果が出たら初期化時に渡した通知で知らせるので、呼び出し側は要求し直す。要求と結果はコードのハッシュで引くので、
    // ノードや diagram_entry を持ち続けない。ノードの高さは呼び出し側が寸法から決める。
    virtual bool RequestRender(const Node& node, DiagramEntry& diagram_entry, float max_width, bool dark_mode) = 0;
    // 前に描いた図の縮めない大きさ (CSS px)。文書を開くときと読み直すときに、描く前に図の高さを決めるのに使う。
    // 覚えていなければ nullopt。実装し忘れると開くたびに図の高さが跳ねるので、既定の実装を置かない。
    virtual std::optional<mermaid_util::DiagramSize> KnownSize(const Node& node, float max_width, bool dark_mode) const = 0;
    // 描画待ちと、投入済みでまだ始まっていない表示用の要求 (戻った図の文字の組みを含む) を捨て、描いている途中の表示用の描画も
    // 止める (インライン数式の要求は維持)。大きく移動した後や幅・テーマの変更後に、不要な図を先に描いて可視の図を待たせない。
    // 捨てた図は再要求される。
    virtual void DropQueued()
    {}
    // 見えている図から順に RequestRender し直す回の始まり。待ちの表示用の要求を、この回に頼まれた順へ並べ直す (先読みで
    // 先に頼んだ図の後ろで、見えている図を待たせない)。
    virtual void BeginRequestPass()
    {}
    // どの図 (DiagramEntry) も拡大表示も持たなくなった図のコマンドリストを、覚えている結果からも外す (図から外しただけでは
    // メモリが返らない)。描画命令は上限の中で残し、次の RequestRender で記録し直す。要求の回の終わり (図が届いた結果を
    // 受け取って前の結果を離した後) に呼ぶ。実装し忘れると画面外の図のメモリが返らないまま気付けないので、既定の実装を置かない。
    virtual void ReleaseIdleDrawings() = 0;
    // 保存・コピーの画像を描く。max_width (本文の幅) に収まらない図は本文と同じくその幅まで縮めて描く。描けなければ null。
    virtual Microsoft::WRL::ComPtr<IWICBitmap> DiagramBitmap(const mermaid_util::DiagramDrawing& /*drawing*/, float /*max_width*/)
    {
        return nullptr;
    }
};

#pragma once
#include "document_types.h"
#include "inline_math.h"
#include "layout_cache.h"
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// インライン数式をまとめて描いた結果。
struct InlineMathBatchResult {
    struct Item {
        uint64_t key = 0;
        // 描けなかった数式 (KaTeX のエラー) は空。
        InlineMathMetrics metrics;
        D2D1_RECT_F src{};  // page の中の位置 (ピクセル)
        D2D1_RECT_F dest{}; // 数式の箱の左上を原点とする描画先 (DIP)
    };
    // 数式の形をアルファに持つ A8 のビットマップ。まとめて描けなかったときは null で、items の metrics はすべて空。
    Microsoft::WRL::ComPtr<ID2D1Bitmap> page;
    // 要求と同じ順。
    std::vector<Item> items;
    // 描く前に取り消した (終了・文書の差し替え)。数式のエラーではないので、後で頼み直してよい。
    bool cancelled = false;
};

// mermaid.h は <WebView2.h> を transitively 引き込むため、mendo_core 入りの
// モジュールは WebView2 依存を避けるべく本 interface を介する。
class IMermaidRenderer {
public:
    // SVG 取得結果コールバック。
    //   svg が非空      : 成功
    //   svg が空, cancelled=false : mermaid のレンダリング失敗
    //   svg が空, cancelled=true  : 終了時に破棄された
    using SvgCallback = std::move_only_function<void(std::wstring svg, bool cancelled)>;
    // 描けたとき・失敗したとき・取り消したときのどれでも 1 回だけ呼ぶ。
    using MathBatchCallback = std::move_only_function<void(InlineMathBatchResult result)>;
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
    // インライン数式を 1 ページに並べて描き、1 回のキャプチャで型を取る。図と同じワーカー枠を使う。
    // 1 つずつ描くとキャプチャの待ちで 1 個 66ms かかるが、まとめれば 1 個 0.5ms ほどで済む。
    // items は kMaxMathPerBatch 個までにする。
    virtual void RequestMathBatch(std::vector<InlineMathItem> items, MathBatchCallback callback) = 0;
    // キャプチャは面積に比例して時間とメモリを使い、2000 個を 1 枚にすると WebView2 の commit が
    // 一時的に 250MB 増える (200 個なら 31MB、1 個あたりの時間はほぼ同じ)。
    static constexpr size_t kMaxMathPerBatch = 200;
    // 描画待ちの表示用要求だけを捨てる (描画中と SVG 要求は維持)。大きく移動した後や幅・テーマの変更後に、
    // 不要になった図が先に描かれて可視の図が待たされるのを防ぐ。捨てた図は再要求される。
    virtual void DropQueued()
    {}
};

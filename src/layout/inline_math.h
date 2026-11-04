#pragma once
#include "text_types.h"
#include "utility.h"
#include <dwrite.h>
#include <wrl/client.h>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

struct Node;
struct NodeTableData;
struct Theme;

// インライン数式 1 つの寸法 (DIP)。baseline は箱の上端から本文のベースラインまで。
// 幅が 0 のものは描けなかった数式で、原文のまま表示する。
struct InlineMathMetrics {
    float width = 0.0f;
    float height = 0.0f;
    float baseline = 0.0f;

    constexpr bool ok() const noexcept
    {
        return width > 0.0f;
    }
};

// 描く数式 1 つ。font_size は数式を置く場所の文字の大きさ (DIP)。
struct InlineMathItem {
    uint64_t key = 0;
    std::string tex;
    float font_size = 0.0f;
};

// 数式の run (TextRun::math) から描く TeX を取り出す。
// 長い本文を区切ったときに途中で分かれた $$..$$ のように、前後の区切りがそろっていない run は nullopt で、原文のまま組む。
std::optional<std::string_view> InlineMathTex(std::string_view text, const TextRun& run);

// 数式は文字の大きさごとに描くので、キーに大きさも混ぜる。
uint64_t InlineMathKey(std::string_view tex, float font_size) noexcept;
// 数式を置く場所の文字の大きさ。計画と計測はこれでキーを揃える。見出しは見出しの大きさ、表の見出し行は h4、ほかは本文。
// コードの大きさは当たらない (数式はインラインコードと重ならず、コードブロックに数式は無い)。
float MathFontSize(const Theme& theme, const Node& node) noexcept;
float MathFontSize(const Theme& theme, const NodeTableData& table, size_t row) noexcept;

// 1 回のまとめ描き (bg のワーカーの 1 つの仕事) に載せる数式の数の上限。KaTeX は 1 個 平均 0.2ms ほどなので、
// 上限でも 1 つの仕事が 40ms ほどで終わり、ほかの図や画像のデコードを長く待たせない。
inline constexpr size_t kMaxInlineMathPerChunk = 200;

struct InlineMathChunk {
    // 組の数式のキーの並びから決まる。計画を作り直す前に頼んだ組の結果を見分けるのに使う。
    uint64_t key = 0;
    std::vector<InlineMathItem> items;
};

// 文書の数式を、同じ内容なら毎回同じ組に分ける。描画 (bg のワーカーの 1 つの仕事) はこの組を単位にする。
// 文書の順に重複を除いて並べ、区切りを数式のキーで決めるので、編集で数式が増減しても変わるのはその前後の組だけ
// (ディスクキャッシュに組を置いていたときの決め方。今も組の大きさをそろえるのに使う)。
class InlineMathPlan {
public:
    void Build(std::span<const Node> nodes, const Theme& theme);
    std::span<const InlineMathChunk> Chunks() const noexcept
    {
        return chunks_;
    }
    // key の数式を含む組の番号。計画に無ければ nullopt。
    std::optional<size_t> ChunkOf(uint64_t key) const noexcept;
    // 組を描くために取り出す。結果を待っている組は nullptr。描けた組はまた取り出せる。
    const InlineMathChunk* TakeChunk(size_t chunk) noexcept;
    // TakeChunk した組の結果が届いた。計画を作り直す前に頼んだ組 (番号と key が合わない) は何もしない。
    void FinishChunk(size_t chunk, uint64_t key) noexcept;

private:
    enum class State : uint8_t { Idle, Requested };

    // 計画を作り直す前に頼んだ組は、番号と key が合わない。
    bool IsCurrent(size_t chunk, uint64_t key) const noexcept
    {
        return chunk < chunks_.size() && chunks_[chunk].key == key;
    }

    std::vector<InlineMathChunk> chunks_;
    // 開く・読み直す・ズームのたびに数式の数だけ足すので、要素ごとに確保しない (数式 12000 個で 1598 → 1076µs)。
    FlatHashMap<uint32_t> chunk_of_;
    std::vector<State> states_;
};

// 数式の寸法表。計測ワーカーが引き、まだ無ければ描画を頼む。描画の結果は UI スレッドが書く。
class InlineMathTable {
public:
    // 頼んだ数式が溜まり始めたときに、頼んだスレッド (計測ワーカー) で呼ぶ。UI スレッドへ知らせるだけにする。
    void SetWake(std::move_only_function<void()> wake);

    // 寸法が分かっていれば返す。無ければ描画を頼み (同じキーは結果が出るまで 1 回だけ)、nullopt。
    std::optional<InlineMathMetrics> FindOrRequest(uint64_t key);
    std::vector<uint64_t> TakeRequests();
    // 組ごと描くとき、まだ計測で見つかっていない数式も頼んだことにして、計測から二重に頼ませない。
    void MarkRequested(std::span<const InlineMathItem> items);

    // 戻り値: key が表にある。Clear の前に頼んだ結果が後から届いたら false。
    bool Resolve(uint64_t key, InlineMathMetrics metrics);
    // 待っている数式を頼まなかったことにする。同じ数式がまた測られたら頼み直す。
    void ForgetRequest(uint64_t key);
    // 分かっている寸法。FindOrRequest と違い、無くても頼まない。
    std::optional<InlineMathMetrics> Find(uint64_t key);
    // 頼んで結果を待っている数式がある。
    bool HasUnresolved();
    // すべて忘れる。表を引いて組んだレイアウトも捨てて、組み直させること。
    void Clear();

private:
    std::mutex mutex_;
    // nullopt の間は描画を頼んで待っている。計測は DirectWrite が律速で、FlatHashMap にしても時間は変わらない。
    std::unordered_map<uint64_t, std::optional<InlineMathMetrics>> entries_;
    // entries_ のうち metrics の無いものの数。
    size_t unresolved_ = 0;
    std::vector<uint64_t> requests_;
    std::move_only_function<void()> wake_;
};

// $...$ の範囲に IDWriteTextLayout::SetInlineObject で掛ける。描くのは IDWriteTextRenderer::DrawInlineObject で、
// InlineMathObjectOf で引いた描画命令を本文の色で描く。scale は寸法と描画命令に掛ける倍率 (幅に収まらないブロック数式を縮める)。
Microsoft::WRL::ComPtr<IDWriteInlineObject> MakeInlineMathObject(uint64_t key, const InlineMathMetrics& metrics, float scale);
struct InlineMathObjectRef {
    uint64_t key = 0;
    float scale = 1.0f;
};
// MakeInlineMathObject で作ったものなら key と倍率。書式の省略記号 (CreateEllipsisTrimmingSign) などは nullopt。
std::optional<InlineMathObjectRef> InlineMathObjectOf(IDWriteInlineObject* object) noexcept;

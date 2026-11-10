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

// 1 回のまとめ描き (bg のワーカーの 1 つの仕事) に載せる数式の数。KaTeX は 1 個 平均 0.2ms ほどなので、
// 1 つの仕事が 13ms ほどで終わり、ほかの図や画像のデコードを長く待たせない。
inline constexpr size_t kInlineMathPerChunk = 64;

struct InlineMathChunk {
    std::vector<InlineMathItem> items;
    // 描画を頼んで結果を待っている (TakeChunk から FinishChunk まで)。
    bool requested = false;
};

// 文書の数式を組に分ける。描画 (bg のワーカーの 1 つの仕事) はこの組を単位にする。
// 文書の順に重複を除いて並べ、kInlineMathPerChunk 個ずつ順に区切る。
class InlineMathPlan {
public:
    // math_nodes はインライン数式を持つノードの番号 (文書の順。Document::GetMathNodeIndices)。
    void Build(std::span<const Node> nodes, std::span<const size_t> math_nodes, const Theme& theme);
    std::span<const InlineMathChunk> Chunks() const noexcept
    {
        return chunks_;
    }
    // key の数式を含む組の番号。計画に無ければ nullopt。
    std::optional<size_t> ChunkOf(uint64_t key) const noexcept;
    // Build のたびに変わる。頼んだ組の結果が今の計画のものかを見分ける。
    uint64_t Generation() const noexcept
    {
        return generation_;
    }
    // 組を描くために取り出す。結果を待っている組は nullptr。描けた組はまた取り出せる。
    const InlineMathChunk* TakeChunk(size_t chunk) noexcept;
    // TakeChunk した組の結果が届いた。generation は頼んだときの Generation()。作り直す前の計画で頼んだ組は何もしない。
    void FinishChunk(size_t chunk, uint64_t generation) noexcept;

private:
    std::vector<InlineMathChunk> chunks_;
    uint64_t generation_ = 0;
    // 開く・読み直す・ズームのたびに数式の数だけ足すので、要素ごとに確保しない (数式 12000 個で 1598 → 1076µs)。
    FlatHashMap<uint32_t> chunk_of_;
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
// InlineMathObjectOf で引いた描画命令を本文の色で描く (色を指定した部分はその色)。scale は寸法と描画命令に掛ける倍率 (幅に収まらないブロック数式を縮める)。
Microsoft::WRL::ComPtr<IDWriteInlineObject> MakeInlineMathObject(uint64_t key, const InlineMathMetrics& metrics, float scale);
struct InlineMathObjectRef {
    uint64_t key = 0;
    float scale = 1.0f;
};
// MakeInlineMathObject で作ったものなら key と倍率。書式の省略記号 (CreateEllipsisTrimmingSign) などは nullopt。
std::optional<InlineMathObjectRef> InlineMathObjectOf(IDWriteInlineObject* object) noexcept;

// ---- 画像 ----

// 本文中の画像 (TextRun::kImage)。GitHub と同じく行の中に置き、下端を本文のベースラインに揃え (vertical-align: baseline)、
// 行より広い画像は行の幅に縮める (max-width: 100%)。

// 表示寸法 (DIP)。
struct InlineImageSize {
    float width = 0.0f;
    float height = 0.0f;

    constexpr bool operator==(const InlineImageSize&) const noexcept = default;
};

enum class InlineImageState : uint8_t {
    // 寸法をまだ知らない。何も描かない幅 0 の箱で組み、寸法が分かったら組み直す。
    Loading,
    Ready,
    // 読めない。Chrome と同じく壊れた画像のアイコンと alt を並べる。alt が空なら何も出さない。
    Unavailable,
};

struct InlineImageEntry {
    InlineImageState state = InlineImageState::Loading;
    InlineImageSize size;

    constexpr bool operator==(const InlineImageEntry&) const noexcept = default;
};

// 文書の中の記述どおりの src で決まる。画像の場所は文書のフォルダで決まるので、文書を替えたら表を空にする。
uint64_t InlineImageKey(std::string_view src) noexcept;

// 画像の寸法表。計測ワーカーが引き、UI スレッド (ResourceManager) が読み込みの結果を書く。
class InlineImageTable {
public:
    // 無ければ Loading。
    InlineImageEntry Find(uint64_t key);
    // 戻り値: 前の値 (無ければ Loading)。
    InlineImageEntry Exchange(uint64_t key, const InlineImageEntry& entry);
    void Clear();

private:
    std::mutex mutex_;
    std::unordered_map<uint64_t, InlineImageEntry> entries_;
};

// 画像の範囲に IDWriteTextLayout::SetInlineObject で掛ける。描くのは IDWriteTextRenderer::DrawInlineObject で、
// InlineImageObjectOf で引いた key のビットマップを箱いっぱいに描く。
Microsoft::WRL::ComPtr<IDWriteInlineObject> MakeInlineImageObject(uint64_t key, InlineImageSize size);
// 読めない画像のアイコンの左端から alt の左端まで (アイコンと隙間)。
constexpr float BrokenImageAltLeft(float icon_size) noexcept
{
    return icon_size * 1.25f;
}
// 読めない画像。壊れた画像のアイコン (一辺 icon_size) の右に alt を組んだ layout を並べ、alt のベースラインを本文に揃える。
Microsoft::WRL::ComPtr<IDWriteInlineObject> MakeBrokenImageObject(uint64_t key, Microsoft::WRL::ComPtr<IDWriteTextLayout> alt, float icon_size);
struct InlineImageObjectRef {
    uint64_t key = 0;
    // 箱の大きさ。読めない画像はアイコンと alt を並べた大きさ。
    InlineImageSize size;
    // 読めない画像の alt。null なら画像を描く。
    IDWriteTextLayout* alt = nullptr;
    // 読めない画像のアイコンの大きさと位置、alt の位置 (どれも箱の左上から)。
    float icon_size = 0.0f;
    float icon_top = 0.0f;
    float alt_left = 0.0f;
    float alt_top = 0.0f;
};
// MakeInlineImageObject / MakeBrokenImageObject で作ったものなら key と寸法。ほかのインラインオブジェクトは nullopt。
std::optional<InlineImageObjectRef> InlineImageObjectOf(IDWriteInlineObject* object) noexcept;

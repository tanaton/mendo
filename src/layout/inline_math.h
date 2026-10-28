#pragma once
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

// 数式は文字の大きさごとに描くので、キーに大きさも混ぜる。
uint64_t InlineMathKey(std::string_view tex, float font_size) noexcept;

// 1 回のまとめ描き (1 ページ・1 回のキャプチャ) に載せる数式の数の上限。キャプチャは面積に比例して時間とメモリを使い、
// 2000 個を 1 枚にすると WebView2 の commit が一時的に 250MB 増える (200 個なら 31MB、1 個あたりの時間はほぼ同じ)。
inline constexpr size_t kMaxInlineMathPerChunk = 200;

struct InlineMathChunk {
    // ディスクキャッシュのキー。組の数式のキーの並びから決まる。
    uint64_t key = 0;
    std::vector<InlineMathItem> items;
};

// 文書の数式を、同じ内容なら毎回同じ組に分ける。描画とディスクキャッシュはこの組を単位にする。
// 計測で見つかった順に束ねると開くたびに組が変わり、キャッシュに当たらない。
// 文書の順に重複を除いて並べ、区切りを数式のキーで決めるので、編集で数式が増減しても変わるのはその前後の組だけ。
class InlineMathPlan {
public:
    // 文字の大きさは計測と同じ規則 (見出しは見出しの大きさ、表の見出し行は h4、ほかは本文) で決める。
    void Build(std::span<const Node> nodes, const Theme& theme);
    std::span<const InlineMathChunk> Chunks() const noexcept
    {
        return chunks_;
    }
    // key の数式を含む組の番号。計画に無い (計画と違う大きさで組んだ) ときは nullopt。
    std::optional<size_t> ChunkOf(uint64_t key) const noexcept;

private:
    std::vector<InlineMathChunk> chunks_;
    std::unordered_map<uint64_t, uint32_t> chunk_of_;
};

// 数式の寸法表。計測ワーカーが引き、まだ無ければ描画を頼む。描画の結果は UI スレッドが書く。
class InlineMathTable {
public:
    // 頼んだ数式が溜まり始めたときに、頼んだスレッド (計測ワーカー) で呼ぶ。UI スレッドへ知らせるだけにする。
    void SetWake(std::move_only_function<void()> wake);

    // 寸法が分かっていれば返す。無ければ描画を頼み (同じキーは結果が出るまで 1 回だけ)、nullopt。
    std::optional<InlineMathMetrics> FindOrRequest(uint64_t key, std::string_view tex, float font_size);
    std::vector<InlineMathItem> TakeRequests();
    // 組ごと描くとき、まだ計測で見つかっていない数式も頼んだことにして、計測から二重に頼ませない。
    void MarkRequested(std::span<const InlineMathItem> items);

    // 戻り値: key が表にある。Clear の前に頼んだ結果が後から届いたら false。
    bool Resolve(uint64_t key, InlineMathMetrics metrics);
    // 描かれずに終わった (取り消し・失敗)。次に計測したときに頼み直させる。
    void Forget(uint64_t key);
    // 描いた型を失った (デバイスロスト) ので、寸法は残したまま描けた数式をすべて頼み直す。
    void RequestAllAgain();
    // すべて忘れる。表を引いて組んだレイアウトも捨てて、組み直させること。
    void Clear();

private:
    struct Entry {
        InlineMathMetrics metrics;
        std::string tex;
        float font_size = 0.0f;
        // false の間は描画を頼んで待っている。
        bool resolved = false;
    };

    void PushRequest(uint64_t key, const Entry& e);

    std::mutex mutex_;
    std::unordered_map<uint64_t, Entry> entries_;
    std::vector<InlineMathItem> requests_;
    std::move_only_function<void()> wake_;
};

// $...$ の範囲に IDWriteTextLayout::SetInlineObject で掛ける。描くのは IDWriteTextRenderer::DrawInlineObject で、
// InlineMathObjectKey で引いた型を本文の色で塗る。
Microsoft::WRL::ComPtr<IDWriteInlineObject> MakeInlineMathObject(uint64_t key, const InlineMathMetrics& metrics);
// 文書のレイアウトに掛けるインラインオブジェクトは MakeInlineMathObject で作ったものだけ。
uint64_t InlineMathObjectKey(IDWriteInlineObject* object) noexcept;

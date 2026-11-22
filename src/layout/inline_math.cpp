#include "inline_math.h"
#include "document_types.h"
#include "document_utils.h"
#include "theme.h"
#include "utility.h"
#include <wrl/implements.h>
#include <algorithm>
#include <cmath>
#include <iterator>

std::optional<std::string_view> InlineMathTex(std::string_view text, const TextRun& run)
{
    const uint32_t delim = MathDelimiterLength(text, run);
    if (delim == 0) {
        return std::nullopt;
    }
    return text.substr(run.start + delim, run.length - delim * 2);
}

uint64_t InlineMathKey(std::string_view tex, float font_size) noexcept
{
    // 1/8 DIP 単位に丸め、浮動小数の誤差で同じ大きさが別のキーにならないようにする。
    const auto size_term = static_cast<uint64_t>(std::lround(font_size * 8.0f));
    return RapidHash(tex) ^ (size_term * kGoldenRatio64);
}

float MathFontSize(const Theme& theme, const Node& node) noexcept
{
    return node.type == NodeType::Heading ? theme.GetHeadingSize(node.heading_level) : theme.font_size_body;
}

float MathFontSize(const Theme& theme, const NodeTableData& table, size_t row) noexcept
{
    return table.IsHeaderRow(row) ? theme.GetHeadingSize(Theme::kTableHeaderLevel) : theme.font_size_body;
}

float MathLayoutSize(const Theme& theme, float text_size) noexcept
{
    return text_size / theme.zoom;
}

namespace {

// text の数式の run ごとに、TeX と計測 (DWriteMeasurer) と同じキーで visit(key, tex) を呼ぶ。visit が true を返したら
// そこで止めて true を返す。計画と届いた組の絞り込みで、キーの求め方をここにそろえる。
template <class Visit>
bool VisitMathRuns(std::string_view text, std::span<const TextRun> runs, float layout_size, Visit&& visit)
{
    for (const auto& r : runs) {
        const auto tex = r.math() ? InlineMathTex(text, r) : std::nullopt;
        if (tex && visit(InlineMathKey(*tex, layout_size), *tex)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool HasInlineMathKey(const Node& node, const Theme& theme, std::span<const uint64_t> sorted_keys)
{
    return VisitMathRuns(node.GetText(), node.runs(), MathLayoutSize(theme, MathFontSize(theme, node)),
                         [&](uint64_t key, std::string_view) { return std::ranges::binary_search(sorted_keys, key); });
}

InlineMathPlan::InlineMathPlan(std::span<const Node> nodes, std::span<const size_t> math_nodes, const Theme& theme)
{
    InlineMathChunk current;
    const auto close = [&] {
        if (current.empty()) {
            return;
        }
        // 大きさちょうどの組に写す (current の確保は次の組に使い回す)。伸ばしながら積んだものを持つと余りが残る。
        chunks_.emplace_back(std::make_move_iterator(current.begin()), std::make_move_iterator(current.end()));
        current.clear();
    };
    const auto add_runs = [&](std::string_view text, std::span<const TextRun> runs, float font_size) {
        VisitMathRuns(text, runs, font_size, [&](uint64_t key, std::string_view tex) {
            const auto [chunk, added] = chunk_of_.TryEmplace(key);
            if (added) {
                *chunk = static_cast<uint32_t>(chunks_.size());
                current.push_back({ key, std::string{ tex }, font_size });
                if (current.size() >= (chunks_.empty() ? kInlineMathFirstChunk : kInlineMathPerChunk)) {
                    close();
                }
            }
            return false;
        });
    };
    for (const size_t i : math_nodes) {
        const Node& node = nodes[i];
        if (const auto* tbl = node.table_data()) {
            for (size_t r = 0; r < tbl->row_count; ++r) {
                const float size = MathLayoutSize(theme, MathFontSize(theme, *tbl, r));
                for (size_t c = 0; c < tbl->col_count; ++c) {
                    add_runs(tbl->GetCellText(r, c), tbl->GetCellRuns(r, c), size);
                }
            }
            continue;
        }
        add_runs(node.GetText(), node.runs(), MathLayoutSize(theme, MathFontSize(theme, node)));
    }
    close();
}

std::optional<size_t> InlineMathPlan::ChunkOf(uint64_t key) const noexcept
{
    const uint32_t* const chunk = chunk_of_.Find(key);
    return chunk ? std::optional<size_t>{ *chunk } : std::nullopt;
}

std::vector<InlineMathItem> InlineMathTable::TakeUnsent(std::span<const InlineMathItem> items)
{
    std::vector<InlineMathItem> unsent;
    unsent.reserve(items.size());
    const std::lock_guard lock(mutex_);
    for (const auto& item : items) {
        const auto [it, added] = entries_.try_emplace(item.key);
        unresolved_ += added;
        if (it->second.state == State::Wanted) {
            it->second.state = State::Sent;
            unsent.push_back(item);
        }
    }
    return unsent;
}

std::vector<InlineMathChunk> InlineMathTable::TakeUnsentChunks(const InlineMathPlan& plan, std::span<const uint64_t> keys)
{
    const auto chunks = plan.Chunks();
    std::vector<InlineMathChunk> taken;
    // 表は 1 回に何千個も頼むので、頼まれた数式ごとに組をたどらず、同じ組は 1 回だけたどる。
    std::vector<bool> visited(chunks.size());
    for (const uint64_t key : keys) {
        const auto chunk = plan.ChunkOf(key);
        // 計画に無いのは、リロードの前に測って今の文書には無くなった数式だけ。描かずに待ちから外す。
        if (!chunk) {
            ForgetRequest(key);
            continue;
        }
        if (visited[*chunk]) {
            continue;
        }
        visited[*chunk] = true;
        // 組の残りの数式もまとめて描く (見えていない数式も、次にスクロールしたときには描けている)。
        if (auto unsent = TakeUnsent(chunks[*chunk]); !unsent.empty()) {
            taken.push_back(std::move(unsent));
        }
    }
    return taken;
}

void InlineMathTable::SetWake(WorkerWake wake)
{
    const std::lock_guard lock(mutex_);
    wake_ = std::move(wake);
}

std::optional<InlineMathMetrics> InlineMathTable::FindOrRequest(uint64_t key)
{
    const std::lock_guard lock(mutex_);
    const auto [it, added] = entries_.try_emplace(key);
    if (added) {
        ++unresolved_;
        const bool was_empty = requests_.empty();
        requests_.push_back(key);
        if (was_empty && wake_) {
            wake_();
        }
    }
    const Entry& entry = it->second;
    return entry.state == State::Done ? std::optional{ entry.metrics } : std::nullopt;
}

std::vector<uint64_t> InlineMathTable::TakeRequests()
{
    const std::lock_guard lock(mutex_);
    return std::exchange(requests_, {});
}

bool InlineMathTable::Resolve(uint64_t key, InlineMathMetrics metrics)
{
    const std::lock_guard lock(mutex_);
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        return false;
    }
    if (it->second.state != State::Done) {
        --unresolved_;
    }
    it->second = { .metrics = metrics, .state = State::Done };
    return true;
}

void InlineMathTable::ForgetRequest(uint64_t key)
{
    const std::lock_guard lock(mutex_);
    const auto it = entries_.find(key);
    if (it != entries_.end() && it->second.state != State::Done) {
        entries_.erase(it);
        --unresolved_;
    }
}

bool InlineMathTable::HasUnresolved()
{
    const std::lock_guard lock(mutex_);
    return unresolved_ > 0;
}

void InlineMathTable::Clear()
{
    const std::lock_guard lock(mutex_);
    entries_.clear();
    unresolved_ = 0;
    requests_.clear();
}

namespace {

// 描画に届いたインラインオブジェクトが数式か画像かを QueryInterface で見分ける。
MIDL_INTERFACE("5b3f8c1e-9d47-4a26-8e1b-7c2d4f60a913")
IInlineMathRef : public IUnknown
{
    virtual InlineMathObjectRef STDMETHODCALLTYPE Ref() const noexcept = 0;
};
MIDL_INTERFACE("0f8d2a64-37c1-4b9e-a5d3-6e21c9b47f08")
IInlineImageRef : public IUnknown
{
    virtual InlineImageObjectRef STDMETHODCALLTYPE Ref() const noexcept = 0;
};

// 寸法と ref を持つだけで描かない。計測ワーカーで作って UI スレッドで描くので、参照カウントはスレッド安全なものを使う。
// 読めない画像は alt の layout を持ち、描く側に渡す。
template <class IRef, class RefT>
class RefInlineObject final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDWriteInlineObject, IRef> {
public:
    RefInlineObject(const RefT& ref, const DWRITE_INLINE_OBJECT_METRICS& metrics, Microsoft::WRL::ComPtr<IDWriteTextLayout> alt) noexcept
        : ref_(ref), metrics_(metrics), alt_(std::move(alt))
    {
    }

    RefT STDMETHODCALLTYPE Ref() const noexcept override
    {
        return ref_;
    }

    HRESULT STDMETHODCALLTYPE Draw(void*, IDWriteTextRenderer*, FLOAT, FLOAT, BOOL, BOOL, IUnknown*) noexcept override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS* metrics) noexcept override
    {
        *metrics = metrics_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS* overhangs) noexcept override
    {
        *overhangs = {};
        return S_OK;
    }
    // 前後で折り返せるかは隣の文字に任せる。数式は 1 つの語のように扱い、画像はブラウザの img と同じく隣が空白なら折り返せる。
    HRESULT STDMETHODCALLTYPE GetBreakConditions(DWRITE_BREAK_CONDITION* before, DWRITE_BREAK_CONDITION* after) noexcept override
    {
        *before = DWRITE_BREAK_CONDITION_NEUTRAL;
        *after = DWRITE_BREAK_CONDITION_NEUTRAL;
        return S_OK;
    }

private:
    RefT ref_;
    DWRITE_INLINE_OBJECT_METRICS metrics_;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> alt_;
};

using InlineMathObject = RefInlineObject<IInlineMathRef, InlineMathObjectRef>;
using InlineImageObject = RefInlineObject<IInlineImageRef, InlineImageObjectRef>;

template <class IRef>
auto RefOf(IDWriteInlineObject* object) noexcept -> std::optional<decltype(std::declval<const IRef&>().Ref())>
{
    Microsoft::WRL::ComPtr<IRef> ref;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&ref)))) {
        return std::nullopt;
    }
    return ref->Ref();
}

} // namespace

Microsoft::WRL::ComPtr<IDWriteInlineObject> MakeInlineMathObject(uint64_t key, const InlineMathMetrics& metrics, float scale)
{
    return Microsoft::WRL::Make<InlineMathObject>(
        InlineMathObjectRef{ key, scale },
        DWRITE_INLINE_OBJECT_METRICS{ metrics.width * scale, metrics.height * scale, metrics.baseline * scale, FALSE }, nullptr);
}

std::optional<InlineMathObjectRef> InlineMathObjectOf(IDWriteInlineObject* object) noexcept
{
    return RefOf<IInlineMathRef>(object);
}

// ---- 画像 ----

uint64_t InlineImageKey(std::string_view src) noexcept
{
    return RapidHash(src);
}

InlineImageEntry InlineImageTable::Find(uint64_t key)
{
    const std::lock_guard lock(mutex_);
    const auto it = entries_.find(key);
    return it != entries_.end() ? it->second : InlineImageEntry{};
}

InlineImageEntry InlineImageTable::Exchange(uint64_t key, const InlineImageEntry& entry)
{
    const std::lock_guard lock(mutex_);
    return std::exchange(entries_[key], entry);
}

void InlineImageTable::Clear()
{
    const std::lock_guard lock(mutex_);
    entries_.clear();
}

Microsoft::WRL::ComPtr<IDWriteInlineObject> MakeInlineImageObject(uint64_t key, InlineImageSize size)
{
    // 下端をベースラインに置く。
    return Microsoft::WRL::Make<InlineImageObject>(InlineImageObjectRef{ .key = key, .size = size },
                                                   DWRITE_INLINE_OBJECT_METRICS{ size.width, size.height, size.height, FALSE }, nullptr);
}

Microsoft::WRL::ComPtr<IDWriteInlineObject> MakeBrokenImageObject(uint64_t key, Microsoft::WRL::ComPtr<IDWriteTextLayout> alt, float icon_size)
{
    DWRITE_TEXT_METRICS m{};
    alt->GetMetrics(&m);
    std::vector<DWRITE_LINE_METRICS> lines(std::max<UINT32>(m.lineCount, 1));
    UINT32 line_count = 0;
    const float alt_baseline = SUCCEEDED(alt->GetLineMetrics(lines.data(), static_cast<UINT32>(lines.size()), &line_count)) && line_count > 0
                                   ? lines[0].baseline
                                   : m.height;
    // アイコンは下端を alt の 1 行目のベースラインに置く。アイコンが alt の上端より高ければ箱をその分伸ばす。
    const float baseline = std::max(alt_baseline, icon_size);
    InlineImageObjectRef ref{
        .key = key,
        .alt = alt.Get(),
        .icon_size = icon_size,
        .icon_top = baseline - icon_size,
        .alt_left = BrokenImageAltLeft(icon_size),
        .alt_top = baseline - alt_baseline,
    };
    ref.size = { ref.alt_left + m.width, std::max(ref.alt_top + m.height, baseline) };
    return Microsoft::WRL::Make<InlineImageObject>(ref, DWRITE_INLINE_OBJECT_METRICS{ ref.size.width, ref.size.height, baseline, FALSE }, std::move(alt));
}

std::optional<InlineImageObjectRef> InlineImageObjectOf(IDWriteInlineObject* object) noexcept
{
    return RefOf<IInlineImageRef>(object);
}

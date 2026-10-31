#include "inline_math.h"
#include "document_types.h"
#include "document_utils.h"
#include "theme.h"
#include "utility.h"
#include <wrl/implements.h>
#include <algorithm>
#include <cmath>

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
    return Fnv1a64(tex) ^ (size_term * kGoldenRatio64);
}

float MathFontSize(const Theme& theme, const Node& node) noexcept
{
    return node.type == NodeType::Heading ? theme.GetHeadingSize(node.heading_level) : theme.font_size_body;
}

float MathFontSize(const Theme& theme, const NodeTableData& table, size_t row) noexcept
{
    return table.IsHeaderRow(row) ? theme.GetHeadingSize(Theme::kTableHeaderLevel) : theme.font_size_body;
}

void InlineMathPlan::Build(std::span<const Node> nodes, const Theme& theme)
{
    // 平均してこの数ごとに区切る。上限 (kMaxInlineMathPerChunk) で区切ると、数式の増減でその後ろの区切りも
    // 次にキーで区切るまでずれるので、上限に届くことが少ないよう (約 4%) 平均を上限の 1/3 ほどにする。
    constexpr uint64_t kAverageChunk = 64;
    // 描き方を変えたら値も変え、ディスクキャッシュに残った古い描き方のページを引かせない。
    constexpr uint64_t kChunkKeySalt = 0xA54FF53A5F1D36F1ULL;

    chunks_.clear();
    chunk_of_.clear();
    InlineMathChunk current;
    const auto close = [&] {
        if (current.items.empty()) {
            return;
        }
        uint64_t key = kChunkKeySalt;
        for (const auto& item : current.items) {
            key = (key ^ item.key) * kFnv1a64Prime;
        }
        current.key = key;
        chunks_.push_back(std::move(current));
        current = {};
    };
    const auto add_runs = [&](std::string_view text, std::span<const TextRun> runs, float font_size) {
        for (const auto& r : runs) {
            if (!r.math()) {
                continue;
            }
            const auto tex = InlineMathTex(text, r);
            if (!tex) {
                continue;
            }
            const uint64_t key = InlineMathKey(*tex, font_size);
            if (!chunk_of_.try_emplace(key, static_cast<uint32_t>(chunks_.size())).second) {
                continue;
            }
            current.items.push_back({ key, std::string{ *tex }, font_size });
            // 前後の数式に依らず区切るので、数式を足しても消しても区切りはその数式の前後でしか変わらない。
            // FNV-1a は似た短い TeX ("1101x" と "1102x" など) で偏るので、かき混ぜてから決める (splitmix64 の仕上げ)。
            uint64_t mixed = key;
            mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ULL;
            mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBULL;
            mixed ^= mixed >> 31;
            if (mixed % kAverageChunk == 0 || current.items.size() >= kMaxInlineMathPerChunk) {
                close();
            }
        }
    };
    for (const auto& node : nodes) {
        if (const auto* tbl = node.table_data()) {
            // 文書を開くたび・ズームのたびに通るので、数式の無い巨大な表でセルを回らない。
            if (std::ranges::none_of(tbl->all_runs, &TextRun::math)) {
                continue;
            }
            for (size_t r = 0; r < tbl->row_count; ++r) {
                const float size = MathFontSize(theme, *tbl, r);
                for (size_t c = 0; c < tbl->col_count; ++c) {
                    add_runs(tbl->GetCellText(r, c), tbl->GetCellRuns(r, c), size);
                }
            }
            continue;
        }
        add_runs(node.GetText(), node.runs(), MathFontSize(theme, node));
    }
    close();
    states_.assign(chunks_.size(), State::Idle);
}

const InlineMathChunk* InlineMathPlan::TakeChunk(size_t chunk, bool wanted) noexcept
{
    auto& state = states_[chunk];
    if (state == State::Idle) {
        state = wanted ? State::Wanted : State::Requested;
        return &chunks_[chunk];
    }
    if (wanted && state == State::Requested) {
        state = State::Wanted;
    }
    return nullptr;
}

bool InlineMathPlan::IsWanted(size_t chunk, uint64_t key) const noexcept
{
    return IsCurrent(chunk, key) && states_[chunk] == State::Wanted;
}

void InlineMathPlan::FinishChunk(size_t chunk, uint64_t key, bool failed) noexcept
{
    if (IsCurrent(chunk, key)) {
        states_[chunk] = failed ? State::Failed : State::Idle;
    }
}

std::optional<size_t> InlineMathPlan::ChunkOf(uint64_t key) const noexcept
{
    const auto it = chunk_of_.find(key);
    return it != chunk_of_.end() ? std::optional<size_t>{ it->second } : std::nullopt;
}

void InlineMathTable::MarkRequested(std::span<const InlineMathItem> items)
{
    const std::lock_guard lock(mutex_);
    for (const auto& item : items) {
        unresolved_ += entries_.try_emplace(item.key).second;
    }
}

void InlineMathTable::SetWake(std::move_only_function<void()> wake)
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
    return it->second;
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
    if (!it->second) {
        --unresolved_;
    }
    it->second = metrics;
    return true;
}

void InlineMathTable::ForgetRequest(uint64_t key)
{
    const std::lock_guard lock(mutex_);
    const auto it = entries_.find(key);
    if (it != entries_.end() && !it->second) {
        entries_.erase(it);
        --unresolved_;
    }
}

std::optional<InlineMathMetrics> InlineMathTable::Find(uint64_t key)
{
    const std::lock_guard lock(mutex_);
    const auto it = entries_.find(key);
    return it != entries_.end() ? it->second : std::nullopt;
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

// 描画に届いたインラインオブジェクトが数式かを QueryInterface で見分ける。
MIDL_INTERFACE("5b3f8c1e-9d47-4a26-8e1b-7c2d4f60a913")
IInlineMathRef : public IUnknown
{
    virtual InlineMathObjectRef STDMETHODCALLTYPE Ref() const noexcept = 0;
};

// 寸法と key を持つだけで描かない。計測ワーカーで作って UI スレッドで描くので、参照カウントはスレッド安全なものを使う。
class InlineMathObject final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDWriteInlineObject, IInlineMathRef> {
public:
    InlineMathObject(uint64_t key, const InlineMathMetrics& metrics, float scale) noexcept
        : ref_{ key, scale }, metrics_{ metrics.width * scale, metrics.height * scale, metrics.baseline * scale }
    {
    }

    InlineMathObjectRef STDMETHODCALLTYPE Ref() const noexcept override
    {
        return ref_;
    }

    HRESULT STDMETHODCALLTYPE Draw(void*, IDWriteTextRenderer*, FLOAT, FLOAT, BOOL, BOOL, IUnknown*) noexcept override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS* metrics) noexcept override
    {
        *metrics = { metrics_.width, metrics_.height, metrics_.baseline, FALSE };
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS* overhangs) noexcept override
    {
        *overhangs = {};
        return S_OK;
    }
    // 前後で折り返せるかは隣の文字に任せる (数式を 1 つの語のように扱う)。
    HRESULT STDMETHODCALLTYPE GetBreakConditions(DWRITE_BREAK_CONDITION* before, DWRITE_BREAK_CONDITION* after) noexcept override
    {
        *before = DWRITE_BREAK_CONDITION_NEUTRAL;
        *after = DWRITE_BREAK_CONDITION_NEUTRAL;
        return S_OK;
    }

private:
    InlineMathObjectRef ref_;
    InlineMathMetrics metrics_;
};

} // namespace

Microsoft::WRL::ComPtr<IDWriteInlineObject> MakeInlineMathObject(uint64_t key, const InlineMathMetrics& metrics, float scale)
{
    return Microsoft::WRL::Make<InlineMathObject>(key, metrics, scale);
}

std::optional<InlineMathObjectRef> InlineMathObjectOf(IDWriteInlineObject* object) noexcept
{
    Microsoft::WRL::ComPtr<IInlineMathRef> math;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&math)))) {
        return std::nullopt;
    }
    return math->Ref();
}

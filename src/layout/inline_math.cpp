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

void InlineMathPlan::Build(std::span<const Node> nodes, std::span<const size_t> math_nodes, const Theme& theme)
{
    constexpr uint64_t kChunkKeySalt = 0xA54FF53A5F1D36F1ULL;

    chunks_.clear();
    chunk_of_.Clear();
    InlineMathChunk current;
    const auto close = [&] {
        if (current.items.empty()) {
            return;
        }
        current.key = std::ranges::fold_left(current.items, kChunkKeySalt, [](uint64_t key, const auto& item) static noexcept {
            return (key ^ item.key) * kFnv1a64Prime;
        });
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
            const auto [chunk, added] = chunk_of_.TryEmplace(key);
            if (!added) {
                continue;
            }
            *chunk = static_cast<uint32_t>(chunks_.size());
            current.items.push_back({ key, std::string{ *tex }, font_size });
            if (current.items.size() >= kInlineMathPerChunk) {
                close();
            }
        }
    };
    for (const size_t i : math_nodes) {
        const Node& node = nodes[i];
        if (const auto* tbl = node.table_data()) {
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
}

const InlineMathChunk* InlineMathPlan::TakeChunk(size_t chunk) noexcept
{
    InlineMathChunk& c = chunks_[chunk];
    return std::exchange(c.requested, true) ? nullptr : &c;
}

void InlineMathPlan::FinishChunk(size_t chunk, uint64_t key) noexcept
{
    // 計画を作り直す前に頼んだ組は、番号と key が合わない。
    if (chunk < chunks_.size() && chunks_[chunk].key == key) {
        chunks_[chunk].requested = false;
    }
}

std::optional<size_t> InlineMathPlan::ChunkOf(uint64_t key) const noexcept
{
    const uint32_t* const chunk = chunk_of_.Find(key);
    return chunk ? std::optional<size_t>{ *chunk } : std::nullopt;
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

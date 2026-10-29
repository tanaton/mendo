#include "inline_math.h"
#include "document_types.h"
#include "theme.h"
#include "utility.h"
#include <wrl/implements.h>
#include <cmath>

std::optional<std::string> InlineMathTex(std::string_view text, const TextRun& run)
{
    const std::string_view delim = run.display_math() ? "$$" : "$";
    const std::string_view whole = text.substr(run.start, run.length);
    if (whole.size() <= delim.size() * 2 || !whole.starts_with(delim) || !whole.ends_with(delim)) {
        return std::nullopt;
    }
    const std::string_view tex = whole.substr(delim.size(), whole.size() - delim.size() * 2);
    return run.display_math() ? std::string{ "\\displaystyle " }.append(tex) : std::string{ tex };
}

uint64_t InlineMathKey(std::string_view tex, float font_size) noexcept
{
    // 1/8 DIP 単位に丸め、浮動小数の誤差で同じ大きさが別のキーにならないようにする。
    const auto size_term = static_cast<uint64_t>(std::lround(font_size * 8.0f));
    return Fnv1a64(tex) ^ (size_term * 0x9E3779B97F4A7C15ULL);
}

void InlineMathPlan::Build(std::span<const Node> nodes, const Theme& theme)
{
    // 平均してこの数ごとに区切る。上限 (kMaxInlineMathPerChunk) で区切ると、数式の増減でその後ろの区切りも
    // 次にキーで区切るまでずれるので、上限に届くことが少ないよう (約 4%) 平均を上限の 1/3 ほどにする。
    constexpr uint64_t kAverageChunk = 64;
    // 描き方を変えたら値も変え、ディスクキャッシュに残った古い描き方のページを引かせない。
    constexpr uint64_t kChunkKeySalt = 0x3C6EF372FE94F82BULL;

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
            auto tex = InlineMathTex(text, r);
            if (!tex) {
                continue;
            }
            const uint64_t key = InlineMathKey(*tex, font_size);
            if (!chunk_of_.try_emplace(key, static_cast<uint32_t>(chunks_.size())).second) {
                continue;
            }
            current.items.push_back({ key, std::move(*tex), font_size });
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
            for (size_t r = 0; r < tbl->row_count; ++r) {
                const float size = tbl->IsHeaderRow(r) ? theme.font_size_h[3] : theme.font_size_body;
                for (size_t c = 0; c < tbl->col_count; ++c) {
                    add_runs(tbl->GetCellText(r, c), tbl->GetCellRuns(r, c), size);
                }
            }
            continue;
        }
        add_runs(node.GetText(), node.runs(), node.type == NodeType::Heading ? theme.GetHeadingSize(node.heading_level) : theme.font_size_body);
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

std::pair<InlineMathTable::Entry&, bool> InlineMathTable::Add(uint64_t key, std::string_view tex, float font_size)
{
    const auto [it, inserted] = entries_.try_emplace(key);
    if (inserted) {
        it->second.item = { key, std::string{ tex }, font_size };
        ++unresolved_;
    }
    return { it->second, inserted };
}

void InlineMathTable::MarkRequested(std::span<const InlineMathItem> items)
{
    const std::lock_guard lock(mutex_);
    for (const auto& item : items) {
        Add(item.key, item.tex, item.font_size);
    }
}

void InlineMathTable::SetWake(std::move_only_function<void()> wake)
{
    const std::lock_guard lock(mutex_);
    wake_ = std::move(wake);
}

void InlineMathTable::PushRequest(const InlineMathItem& item)
{
    const bool was_empty = requests_.empty();
    requests_.push_back(item);
    if (was_empty && wake_) {
        wake_();
    }
}

std::optional<InlineMathMetrics> InlineMathTable::FindOrRequest(uint64_t key, std::string_view tex, float font_size)
{
    const std::lock_guard lock(mutex_);
    const auto [e, added] = Add(key, tex, font_size);
    if (added) {
        PushRequest(e.item);
    }
    return e.metrics;
}

std::vector<InlineMathItem> InlineMathTable::TakeRequests()
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
    if (!it->second.metrics) {
        --unresolved_;
    }
    it->second.metrics = metrics;
    return true;
}

std::optional<InlineMathMetrics> InlineMathTable::Find(uint64_t key)
{
    const std::lock_guard lock(mutex_);
    const auto it = entries_.find(key);
    return it != entries_.end() ? it->second.metrics : std::nullopt;
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

void InlineMathTable::RequestAllAgain(const std::function<bool(uint64_t)>& again)
{
    const std::lock_guard lock(mutex_);
    for (const auto& [key, e] : entries_) {
        if (e.metrics && e.metrics->ok() && again(key)) {
            PushRequest(e.item);
        }
    }
}

namespace {

// 描画に届いたインラインオブジェクトが数式かを QueryInterface で見分ける。
MIDL_INTERFACE("5b3f8c1e-9d47-4a26-8e1b-7c2d4f60a913")
IInlineMathKey : public IUnknown
{
    virtual uint64_t STDMETHODCALLTYPE Key() const noexcept = 0;
};

// 寸法と key を持つだけで描かない。計測ワーカーで作って UI スレッドで描くので、参照カウントはスレッド安全なものを使う。
class InlineMathObject final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDWriteInlineObject, IInlineMathKey> {
public:
    InlineMathObject(uint64_t key, const InlineMathMetrics& metrics) noexcept : key_(key), metrics_(metrics)
    {
    }

    uint64_t STDMETHODCALLTYPE Key() const noexcept override
    {
        return key_;
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
    uint64_t key_;
    InlineMathMetrics metrics_;
};

} // namespace

Microsoft::WRL::ComPtr<IDWriteInlineObject> MakeInlineMathObject(uint64_t key, const InlineMathMetrics& metrics)
{
    return Microsoft::WRL::Make<InlineMathObject>(key, metrics);
}

std::optional<uint64_t> InlineMathObjectKey(IDWriteInlineObject* object) noexcept
{
    Microsoft::WRL::ComPtr<IInlineMathKey> math;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&math)))) {
        return std::nullopt;
    }
    return math->Key();
}

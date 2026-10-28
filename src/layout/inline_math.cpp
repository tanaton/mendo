#include "inline_math.h"
#include "document_types.h"
#include "theme.h"
#include "utility.h"
#include <atomic>
#include <cmath>
#include <unordered_set>

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
    std::unordered_set<uint64_t> seen;
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
            if (!seen.insert(key).second) {
                continue;
            }
            chunk_of_.emplace(key, static_cast<uint32_t>(chunks_.size()));
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
        const bool heading = node.type == NodeType::Heading && node.heading_level >= 1 && node.heading_level <= 6;
        add_runs(node.GetText(), node.runs(), heading ? theme.font_size_h[node.heading_level - 1] : theme.font_size_body);
    }
    close();
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
        if (const auto [it, inserted] = entries_.try_emplace(item.key); inserted) {
            it->second.tex = item.tex;
            it->second.font_size = item.font_size;
        }
    }
}

void InlineMathTable::SetWake(std::move_only_function<void()> wake)
{
    const std::lock_guard lock(mutex_);
    wake_ = std::move(wake);
}

void InlineMathTable::PushRequest(uint64_t key, const Entry& e)
{
    const bool was_empty = requests_.empty();
    requests_.push_back({ key, e.tex, e.font_size });
    if (was_empty && wake_) {
        wake_();
    }
}

std::optional<InlineMathMetrics> InlineMathTable::FindOrRequest(uint64_t key, std::string_view tex, float font_size)
{
    const std::lock_guard lock(mutex_);
    const auto [it, inserted] = entries_.try_emplace(key);
    if (!inserted) {
        return it->second.resolved ? std::optional{ it->second.metrics } : std::nullopt;
    }
    it->second.tex = tex;
    it->second.font_size = font_size;
    PushRequest(key, it->second);
    return std::nullopt;
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
    it->second.metrics = metrics;
    it->second.resolved = true;
    return true;
}

void InlineMathTable::Clear()
{
    const std::lock_guard lock(mutex_);
    entries_.clear();
    requests_.clear();
}

void InlineMathTable::Forget(uint64_t key)
{
    const std::lock_guard lock(mutex_);
    if (const auto it = entries_.find(key); it != entries_.end() && !it->second.resolved) {
        entries_.erase(it);
    }
}

void InlineMathTable::RequestAllAgain()
{
    const std::lock_guard lock(mutex_);
    for (const auto& [key, e] : entries_) {
        if (e.resolved && e.metrics.ok()) {
            PushRequest(key, e);
        }
    }
}

namespace {

// 寸法と key を持つだけで描かない。計測ワーカーで作って UI スレッドで描くので、参照カウントは atomic にする。
class InlineMathObject final : public IDWriteInlineObject {
public:
    InlineMathObject(uint64_t key, const InlineMathMetrics& metrics) noexcept : key_(key), metrics_(metrics)
    {
    }

    uint64_t key() const noexcept
    {
        return key_;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) noexcept override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteInlineObject)) {
            AddRef();
            *out = static_cast<IDWriteInlineObject*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override
    {
        return refs_.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() noexcept override
    {
        const ULONG left = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (left == 0) {
            delete this;
        }
        return left;
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
    std::atomic<ULONG> refs_{ 1 };
    uint64_t key_;
    InlineMathMetrics metrics_;
};

} // namespace

Microsoft::WRL::ComPtr<IDWriteInlineObject> MakeInlineMathObject(uint64_t key, const InlineMathMetrics& metrics)
{
    Microsoft::WRL::ComPtr<IDWriteInlineObject> object;
    object.Attach(new InlineMathObject(key, metrics));
    return object;
}

uint64_t InlineMathObjectKey(IDWriteInlineObject* object) noexcept
{
    return static_cast<InlineMathObject*>(object)->key();
}

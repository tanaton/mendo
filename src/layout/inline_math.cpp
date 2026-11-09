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

void InlineMathPlan::Build(std::span<const Node> nodes, std::span<const size_t> math_nodes, const Theme& theme)
{
    ++generation_;
    chunks_.clear();
    chunk_of_.Clear();
    InlineMathChunk current;
    const auto close = [&] {
        if (current.items.empty()) {
            return;
        }
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

void InlineMathPlan::FinishChunk(size_t chunk, uint64_t generation) noexcept
{
    if (generation == generation_) {
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

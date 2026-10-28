#include "inline_math.h"
#include "utility.h"
#include <atomic>
#include <cmath>

uint64_t InlineMathKey(std::string_view tex, float font_size) noexcept
{
    // 1/8 DIP 単位に丸め、浮動小数の誤差で同じ大きさが別のキーにならないようにする。
    const auto size_term = static_cast<uint64_t>(std::lround(font_size * 8.0f));
    return Fnv1a64(tex) ^ (size_term * 0x9E3779B97F4A7C15ULL);
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

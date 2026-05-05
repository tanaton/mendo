#include "doc_dwrite_bridge.h"

#if !MENDO_DOC_USE_UTF16
#  include "memory_resource.h"
#  include <windows.h>
#  include <stringapiset.h>
#  include <limits>

namespace mendo {

WideViewForDWrite::WideViewForDWrite(doc_string_view text)
    : scratch_(GetThreadLocalPoolResource()),
      utf16_offsets_(GetThreadLocalPoolResource())
{
    if (text.empty()) {
        view_ = {};
        return;
    }
    if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        view_ = {};
        return;
    }
    // UTF-16 code unit 数 <= UTF-8 byte 数 が常に成立するので 1 回確保で済む。
    scratch_.resize_and_overwrite(text.size(), [&](wchar_t* buf, size_t cap) -> size_t {
        const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()),
                                          buf, static_cast<int>(cap));
        return n > 0 ? static_cast<size_t>(n) : 0;
    });
    view_ = scratch_;
}

void WideViewForDWrite::EnsureOffsetMap() const noexcept
{
    if (offsets_built_) {
        return;
    }
    offsets_built_ = true;
    // ここに到達するのは UTF-8 ビルドで HitTest/Search 等が offset 変換を要求した場合のみ。
    // Measure ホットパスでは呼ばれない。
    // utf16_offsets_[i] (i = 0..utf8_size) = doc byte i に対応する wide code unit offset。
    auto& self = const_cast<WideViewForDWrite&>(*this);
    // scratch_ は構築時に確定しているが、offset map の元になる UTF-8 source は scratch_ には保持していない。
    // C2 段階では offset 変換は使われないので、ここでは恒等動作 (build at first request) のみ用意する。
    // 実装は C4 で詳細化する。
    self.utf16_offsets_.assign(view_.size() + 1, 0);
    for (size_t i = 0; i <= view_.size(); ++i) {
        self.utf16_offsets_[i] = static_cast<uint32_t>(i);
    }
}

uint32_t WideViewForDWrite::WideOffsetFromDocOffset(doc_offset doc_off) const noexcept
{
    EnsureOffsetMap();
    const uint32_t n = static_cast<uint32_t>(utf16_offsets_.size() - 1);
    if (doc_off >= utf16_offsets_.size()) {
        return utf16_offsets_.empty() ? 0 : utf16_offsets_.back();
    }
    (void)n;
    return utf16_offsets_[doc_off];
}

doc_offset WideViewForDWrite::DocOffsetFromWideOffset(uint32_t wide_off) const noexcept
{
    EnsureOffsetMap();
    if (utf16_offsets_.empty()) {
        return 0;
    }
    if (wide_off >= utf16_offsets_.back()) {
        return static_cast<doc_offset>(utf16_offsets_.size() - 1);
    }
    // 単調増加なので二分探索。
    size_t lo = 0;
    size_t hi = utf16_offsets_.size();
    while (lo + 1 < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (utf16_offsets_[mid] <= wide_off) {
            lo = mid;
        }
        else {
            hi = mid;
        }
    }
    return static_cast<doc_offset>(lo);
}

HRESULT CreateDocTextLayout(IDWriteFactory* factory, doc_string_view text,
                            IDWriteTextFormat* fmt, float max_w, float max_h,
                            IDWriteTextLayout** out) noexcept
{
    WideViewForDWrite view{text};
    const auto wide = view.wide();
    return factory->CreateTextLayout(wide.data(), static_cast<UINT32>(wide.size()),
                                     fmt, max_w, max_h, out);
}

} // namespace mendo

#endif // !MENDO_DOC_USE_UTF16

#pragma once
#include "brush_id.h"
#include <d2d1.h>
#include <dwrite.h>
#include <variant>
#include <vector>
#include <algorithm>
#include <iterator>
#include <memory_resource>
#include <utility>
#include <type_traits>
#include <cstdint>

// brush_id != Custom のコマンドは CommandExecutor が固定ブラシ配列で O(1) 解決する。
// Custom のときだけ color から brush_pool 経由で解決する。

struct ClearCmd {
    D2D1_COLOR_F color;
};

struct FillRectCmd {
    D2D1_RECT_F rect;
    D2D1_COLOR_F color;
    BrushId brush_id = BrushId::Custom;
};

struct FillRoundedRectCmd {
    D2D1_RECT_F rect;
    float rx, ry;
    D2D1_COLOR_F color;
    BrushId brush_id = BrushId::Custom;
};

struct DrawLineCmd {
    D2D1_POINT_2F p0, p1;
    D2D1_COLOR_F color;
    float stroke_width;
    BrushId brush_id = BrushId::Custom;
};

struct DrawTextLayoutCmd {
    D2D1_POINT_2F origin;
    IDWriteTextLayout* layout; // 非所有; ライフタイムはLayoutCacheが管理。
    D2D1_COLOR_F color;
    BrushId brush_id = BrushId::Custom;
};

struct DrawTextCmd {
    // 1〜INLINE_TEXT_CAPACITY 文字は inline_buf に直接格納し、
    // monotonic resource からの小さな allocate を回避する（icon 1 文字や "1." 等の頻出ケース）。
    // pointer と同サイズに収めることで variant 最大サイズに影響しない。
    static constexpr uint8_t INLINE_TEXT_CAPACITY = 4;
    union {
        const wchar_t* text_ptr; // 非所有; ライフタイムはMonotonicResourceが管理。
        wchar_t inline_buf[INLINE_TEXT_CAPACITY];
    };
    IDWriteTextFormat* format = nullptr; // 非所有; ライフタイムはRendererが管理。
    D2D1_RECT_F rect{};
    D2D1_COLOR_F color{};
    uint8_t text_len = 0;
    bool is_inline = false;
    BrushId brush_id = BrushId::Custom;

    DrawTextCmd() noexcept : text_ptr(nullptr)
    {}

    const wchar_t* text() const noexcept
    {
        return is_inline ? inline_buf : text_ptr;
    }
};

struct DrawBitmapCmd {
    ID2D1Bitmap* bitmap; // 非所有; ライフタイムはLayoutCache::DiagramEntryが管理。
    D2D1_RECT_F dest;
    float opacity = 1.0f;
    D2D1_BITMAP_INTERPOLATION_MODE interpolation_mode = D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
};

struct FillEllipseCmd {
    D2D1_POINT_2F center;
    float rx, ry;
    D2D1_COLOR_F color;
    BrushId brush_id = BrushId::Custom;
};

struct DrawEllipseCmd {
    D2D1_POINT_2F center;
    float rx, ry;
    D2D1_COLOR_F color;
    float stroke_width;
    BrushId brush_id = BrushId::Custom;
};

struct PushClipCmd {
    D2D1_RECT_F rect;
};

struct PopClipCmd {};

struct SetTransformCmd {
    D2D1_MATRIX_3X2_F transform;
};

// テスト専用: holds_alternative / get_if / range-for で参照される variant view。
// 本番経路 (Execute) は DrawCommandList::Visit が直接 SoA を走査するためここを通らない。
using DrawCommand = std::variant<
    ClearCmd,
    FillRectCmd,
    FillRoundedRectCmd,
    DrawLineCmd,
    DrawTextLayoutCmd,
    DrawTextCmd,
    DrawBitmapCmd,
    FillEllipseCmd,
    DrawEllipseCmd,
    PushClipCmd,
    PopClipCmd,
    SetTransformCmd>;

// 各コマンド型ごとに pmr::vector を持ち、seq_ に packed (kind:4, idx:28) を発行順で記録する SoA バッファ。
// variant の最大サイズ食いと visit ディスパッチを避けるため、Execute は Visit() の switch から直接呼び出す。
class DrawCommandList {
public:
    explicit constexpr DrawCommandList(std::pmr::memory_resource* mr = std::pmr::get_default_resource())
        : clears_(mr), fill_rects_(mr), fill_rounded_rects_(mr), lines_(mr),
          text_layouts_(mr), texts_(mr), bitmaps_(mr),
          fill_ellipses_(mr), ellipses_(mr), push_clips_(mr), transforms_(mr),
          seq_(mr)
    {}

    // テストで値受け (`auto cmds = gen.GenerateMdPane(...)`) するためコピーを許可する。
    // 本番経路は const& 参照のみでコピーは発生しない。
    constexpr DrawCommandList(const DrawCommandList&) = default;
    constexpr DrawCommandList& operator=(const DrawCommandList&) = default;
    constexpr DrawCommandList(DrawCommandList&&) noexcept = default;
    constexpr DrawCommandList& operator=(DrawCommandList&&) noexcept = default;

    constexpr void clear() noexcept
    {
        clears_.clear();
        fill_rects_.clear();
        fill_rounded_rects_.clear();
        lines_.clear();
        text_layouts_.clear();
        texts_.clear();
        bitmaps_.clear();
        fill_ellipses_.clear();
        ellipses_.clear();
        push_clips_.clear();
        transforms_.clear();
        seq_.clear();
    }

    // 発行順テーブルのみ予約。型別 vector はオンデマンドで伸ばす。
    constexpr void reserve(size_t n)
    {
        seq_.reserve(n);
    }

    constexpr bool empty() const noexcept
    {
        return seq_.empty();
    }

    constexpr size_t size() const noexcept
    {
        return seq_.size();
    }

    constexpr void push_back(const ClearCmd& c)
    {
        Append(Kind::Clear, clears_, c);
    }
    constexpr void push_back(const FillRectCmd& c)
    {
        Append(Kind::FillRect, fill_rects_, c);
    }
    constexpr void push_back(const FillRoundedRectCmd& c)
    {
        Append(Kind::FillRoundedRect, fill_rounded_rects_, c);
    }
    constexpr void push_back(const DrawLineCmd& c)
    {
        Append(Kind::DrawLine, lines_, c);
    }
    constexpr void push_back(const DrawTextLayoutCmd& c)
    {
        Append(Kind::DrawTextLayout, text_layouts_, c);
    }
    constexpr void push_back(const DrawTextCmd& c)
    {
        Append(Kind::DrawText, texts_, c);
    }
    constexpr void push_back(const DrawBitmapCmd& c)
    {
        Append(Kind::DrawBitmap, bitmaps_, c);
    }
    constexpr void push_back(const FillEllipseCmd& c)
    {
        Append(Kind::FillEllipse, fill_ellipses_, c);
    }
    constexpr void push_back(const DrawEllipseCmd& c)
    {
        Append(Kind::DrawEllipse, ellipses_, c);
    }
    constexpr void push_back(const PushClipCmd& c)
    {
        Append(Kind::PushClip, push_clips_, c);
    }
    constexpr void push_back(const SetTransformCmd& c)
    {
        Append(Kind::SetTransform, transforms_, c);
    }
    constexpr void push_back(PopClipCmd) noexcept
    {
        // state-less なので型別 vec は持たず、tag のみで表現する。
        seq_.push_back(Pack(Kind::PopClip, 0));
    }

    template <typename T>
    constexpr void emplace_back(T&& c)
    {
        push_back(std::forward<T>(c));
    }

    template <typename Visitor>
    constexpr void Visit(Visitor&& v, uint32_t entry) const
    {
        const uint32_t idx = GetIndex(entry);
        switch (GetKind(entry)) {
        case Kind::Clear:
            v(clears_[idx]);
            return;
        case Kind::FillRect:
            v(fill_rects_[idx]);
            return;
        case Kind::FillRoundedRect:
            v(fill_rounded_rects_[idx]);
            return;
        case Kind::DrawLine:
            v(lines_[idx]);
            return;
        case Kind::DrawTextLayout:
            v(text_layouts_[idx]);
            return;
        case Kind::DrawText:
            v(texts_[idx]);
            return;
        case Kind::DrawBitmap:
            v(bitmaps_[idx]);
            return;
        case Kind::FillEllipse:
            v(fill_ellipses_[idx]);
            return;
        case Kind::DrawEllipse:
            v(ellipses_[idx]);
            return;
        case Kind::PushClip:
            v(push_clips_[idx]);
            return;
        case Kind::SetTransform:
            v(transforms_[idx]);
            return;
        case Kind::PopClip: {
            static constexpr PopClipCmd kPop{};
            v(kPop);
            return;
        }
        }
        std::unreachable();
    }

    template <typename Visitor>
    constexpr void Visit(Visitor&& v) const
    {
        for (uint32_t entry : seq_) {
            Visit(std::forward<Visitor>(v), entry);
        }
    }

    // テスト用: 呼び出し毎に variant を構築する。本番経路は Visit() を使うこと。
    constexpr DrawCommand At(size_t i) const
    {
        const uint32_t entry = seq_[i];
        const Kind k = GetKind(entry);
        const uint32_t idx = GetIndex(entry);
        switch (k) {
        case Kind::Clear:
            return clears_[idx];
        case Kind::FillRect:
            return fill_rects_[idx];
        case Kind::FillRoundedRect:
            return fill_rounded_rects_[idx];
        case Kind::DrawLine:
            return lines_[idx];
        case Kind::DrawTextLayout:
            return text_layouts_[idx];
        case Kind::DrawText:
            return texts_[idx];
        case Kind::DrawBitmap:
            return bitmaps_[idx];
        case Kind::FillEllipse:
            return fill_ellipses_[idx];
        case Kind::DrawEllipse:
            return ellipses_[idx];
        case Kind::PushClip:
            return push_clips_[idx];
        case Kind::SetTransform:
            return transforms_[idx];
        case Kind::PopClip:
            return PopClipCmd{};
        }
        std::unreachable();
    }

    constexpr DrawCommand operator[](size_t i) const
    {
        return At(i);
    }

    constexpr DrawCommand front() const
    {
        return At(0);
    }

    constexpr DrawCommand back() const
    {
        return At(seq_.size() - 1);
    }

    // *it は prvalue を返すため `&*it` でアドレス取得すると即時ダングリングになる。
    // 取りたい場合は一度ローカル変数に束縛してから get_if 等を使うこと。
    class const_iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = DrawCommand;
        using reference = DrawCommand;
        using pointer = void;
        using difference_type = std::ptrdiff_t;

        constexpr const_iterator() noexcept = default;
        constexpr const_iterator(const DrawCommandList* owner, size_t i) noexcept : owner_(owner), i_(i)
        {}

        constexpr DrawCommand operator*() const
        {
            return owner_->At(i_);
        }

        constexpr const_iterator& operator++() noexcept
        {
            ++i_;
            return *this;
        }
        constexpr const_iterator operator++(int) noexcept
        {
            auto tmp = *this;
            ++i_;
            return tmp;
        }
        constexpr bool operator==(const const_iterator& o) const noexcept
        {
            return i_ == o.i_ && owner_ == o.owner_;
        }
        constexpr bool operator!=(const const_iterator& o) const noexcept
        {
            return !(*this == o);
        }

    private:
        const DrawCommandList* owner_ = nullptr;
        size_t i_ = 0;
    };

    constexpr const_iterator begin() const noexcept
    {
        return { this, 0 };
    }
    constexpr const_iterator end() const noexcept
    {
        return { this, seq_.size() };
    }

private:
    enum class Kind : uint8_t {
        Clear,
        FillRect,
        FillRoundedRect,
        DrawLine,
        DrawTextLayout,
        DrawText,
        DrawBitmap,
        FillEllipse,
        DrawEllipse,
        PushClip,
        SetTransform,
        PopClip,
    };

    static constexpr uint32_t kIndexBits = 28;
    static constexpr uint32_t kIndexMask = (1u << kIndexBits) - 1u;

    static constexpr uint32_t Pack(Kind k, uint32_t idx) noexcept
    {
        return (static_cast<uint32_t>(k) << kIndexBits) | (idx & kIndexMask);
    }
    static constexpr Kind GetKind(uint32_t entry) noexcept
    {
        return static_cast<Kind>(entry >> kIndexBits);
    }
    static constexpr uint32_t GetIndex(uint32_t entry) noexcept
    {
        return entry & kIndexMask;
    }

    template <typename T>
    constexpr void Append(Kind k, std::pmr::vector<std::remove_cvref_t<T>>& vec, T&& cmd)
    {
        const auto idx = static_cast<uint32_t>(vec.size());
        vec.emplace_back(std::forward<T>(cmd));
        seq_.push_back(Pack(k, idx));
    }

    std::pmr::vector<ClearCmd> clears_;
    std::pmr::vector<FillRectCmd> fill_rects_;
    std::pmr::vector<FillRoundedRectCmd> fill_rounded_rects_;
    std::pmr::vector<DrawLineCmd> lines_;
    std::pmr::vector<DrawTextLayoutCmd> text_layouts_;
    std::pmr::vector<DrawTextCmd> texts_;
    std::pmr::vector<DrawBitmapCmd> bitmaps_;
    std::pmr::vector<FillEllipseCmd> fill_ellipses_;
    std::pmr::vector<DrawEllipseCmd> ellipses_;
    std::pmr::vector<PushClipCmd> push_clips_;
    std::pmr::vector<SetTransformCmd> transforms_;

    std::pmr::vector<uint32_t> seq_;
};

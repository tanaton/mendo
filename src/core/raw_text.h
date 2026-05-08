#pragma once
#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

// Document の raw_text_ 専用の薄いラッパ。
// 書き換え API として `Replace(std::pmr::string)` のみを公開し、
// `append/resize/operator+=` のような relocate を発生させ得る関数を型レベルで遮断する。
//
// 不変条件: 一度 Replace されたヒープバッファは Document の生存期間中、再 Replace か
// move-assign 以外で relocate しない。これによって、view モードノードの view_base_ が
// 指し続けるバッファの安全性が型レベルで担保される。
class RawText {
public:
    RawText() = default;

    RawText(const RawText&) = delete;
    RawText& operator=(const RawText&) = delete;
    RawText(RawText&&) noexcept = default;
    RawText& operator=(RawText&&) noexcept = default;

    const char* data() const noexcept
    {
        return text_.data();
    }
    size_t size() const noexcept
    {
        return text_.size();
    }
    bool empty() const noexcept
    {
        return text_.empty();
    }
    char operator[](size_t i) const noexcept
    {
        return text_[i];
    }
    operator std::string_view() const noexcept
    {
        return std::string_view{ text_ };
    }

    // 比較は std::string_view ベース (gtest の EXPECT_EQ などで利用)。
    friend bool operator==(const RawText& a, std::string_view b) noexcept
    {
        return std::string_view{ a.text_ } == b;
    }

    // 完全置換のみを許す書き換え API。append/resize は意図的に提供しない。
    void Replace(std::pmr::string text) noexcept
    {
        text_ = std::move(text);
    }

private:
    std::pmr::string text_;
};

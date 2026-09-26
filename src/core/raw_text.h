#pragma once
#include <cstddef>
#include <memory>
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
// 本体は不変の共有文字列で持ち、非同期リロードの worker が Document の差し替えと無関係に
// 旧テキストを読めるようにする (Share)。move でバッファが relocate しない利点もある。
class RawText {
public:
    RawText() = default;

    RawText(const RawText&) = delete;
    RawText& operator=(const RawText&) = delete;
    RawText(RawText&&) noexcept = default;
    RawText& operator=(RawText&&) noexcept = default;

    const char* data() const noexcept
    {
        return view().data();
    }
    size_t size() const noexcept
    {
        return text_ ? text_->size() : 0;
    }
    bool empty() const noexcept
    {
        return size() == 0;
    }
    char operator[](size_t i) const noexcept
    {
        return (*text_)[i];
    }
    operator std::string_view() const noexcept
    {
        return view();
    }

    // 比較は std::string_view ベース (gtest の EXPECT_EQ などで利用)。
    friend bool operator==(const RawText& a, std::string_view b) noexcept
    {
        return a.view() == b;
    }

    // 完全置換のみを許す書き換え API。append/resize は意図的に提供しない。
    void Replace(std::pmr::string text)
    {
        text_ = std::make_shared<const std::pmr::string>(std::move(text));
    }

    // 同一性 (ポインタ一致) で「同じテキストか」を判定できる共有参照。
    std::shared_ptr<const std::pmr::string> Share() const noexcept
    {
        return text_;
    }

private:
    std::string_view view() const noexcept
    {
        return text_ ? std::string_view{ *text_ } : std::string_view{ "" };
    }

    std::shared_ptr<const std::pmr::string> text_;
};

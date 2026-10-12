#pragma once
#include <memory_resource>
#include <string>
#include <string_view>

// フェードは毎フレームの全画面再描画を伴うので、表示と消去の 2 回だけ描き直す。
class ToastNotifier {
public:
    void Show(std::wstring_view message)
    {
        message_ = message;
    }

    void Hide() noexcept
    {
        message_.clear();
    }

    bool IsVisible() const noexcept
    {
        return !message_.empty();
    }
    std::wstring_view GetMessage() const noexcept
    {
        return message_;
    }

private:
    std::pmr::wstring message_;
};

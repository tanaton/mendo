#pragma once
#include "nav_history.h"
#include "document_utils.h"
#include <string>
#include <string_view>
#include <vector>
#include <memory_resource>

class NavigationService {
public:
    explicit NavigationService(NavHistory& history) noexcept : history_(history) {}

    struct NavigateResult {
        enum class Type { None, Anchor, ExternalUrl, LoadFile };
        Type type = Type::None;
        std::pmr::wstring target;
        float scroll_y = 0.0f;
    };

    // リンククリック処理。結果を返すだけで副作用は起こさない。
    NavigateResult HandleLinkClick(std::wstring_view url,
        std::wstring_view current_file);

    // 戻る
    NavigateResult GoBack(std::wstring_view current_file, float scroll_y);

    // 進む
    NavigateResult GoForward(std::wstring_view current_file, float scroll_y);

    // 履歴にプッシュ
    void PushHistory(std::wstring_view file, float scroll_y);

    constexpr bool CanGoBack() const noexcept { return history_.CanGoBack(); }
    constexpr bool CanGoForward() const noexcept { return history_.CanGoForward(); }

private:
    NavigateResult MakeResultFromEntry(NavEntry&& entry, std::wstring_view current_file);
    NavHistory& history_;
};

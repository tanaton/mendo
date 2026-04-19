#include "navigation_service.h"
#include <algorithm>
#include <ranges>

// ShellExecuteWに渡しても安全なURLスキームかどうかを判定する。
// file:// やその他の危険なスキームをブロックし、http/https/mailto のみ許可する。
bool IsSafeUrlScheme(std::wstring_view url) noexcept
{
    const auto to_lower = [](wchar_t c) static noexcept {
        return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
    };
    const auto starts_with_i = [&](std::wstring_view s, std::wstring_view prefix) noexcept {
        return s.size() >= prefix.size()
            && std::ranges::equal(s | std::views::take(prefix.size()), prefix, {}, to_lower, to_lower);
    };
    return starts_with_i(url, L"http://") || starts_with_i(url, L"https://") || starts_with_i(url, L"mailto:");
}

LinkClickResult HandleLinkClick(std::wstring_view url)
{
    LinkClickResult result;
    if (url.empty()) {
        return result;
    }
    // 内部アンカーリンク: #something
    if (url[0] == L'#') {
        result.type = LinkClickResult::Type::Anchor;
        result.target = url.substr(1);
        return result;
    }
    // 安全なスキームの外部リンクのみ許可
    if (!IsSafeUrlScheme(url)) {
        return result;
    }
    result.type = LinkClickResult::Type::ExternalUrl;
    result.target = url;
    return result;
}

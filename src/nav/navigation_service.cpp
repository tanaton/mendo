#include "navigation_service.h"

// ShellExecuteWに渡しても安全なURLスキームかどうかを判定する。
// file:// やその他の危険なスキームをブロックし、http/https/mailto のみ許可する。
static bool IsSafeUrlScheme(std::wstring_view url) noexcept
{
    const auto starts_with_i = [](std::wstring_view s, std::wstring_view prefix) static noexcept {
        if (s.size() < prefix.size()) {
            return false;
        }
        const auto prefix_len = prefix.size();
        for (size_t i = 0; i < prefix_len; i++) {
            wchar_t a = s[i], b = prefix[i];
            if (a >= L'A' && a <= L'Z') {
                a += L'a' - L'A';
            }
            if (a != b) {
                return false;
            }
        }
        return true;
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

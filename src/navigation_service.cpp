#include "navigation_service.h"

// ShellExecuteWに渡しても安全なURLスキームかどうかを判定する。
// file:// やその他の危険なスキームをブロックし、http/https/mailto のみ許可する。
static bool IsSafeUrlScheme(std::wstring_view url) noexcept
{
    const auto starts_with_i = [](std::wstring_view s, std::wstring_view prefix) noexcept {
        if (s.size() < prefix.size()) {
            return false;
        }
        for (size_t i = 0; i < prefix.size(); i++) {
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
    return starts_with_i(url, L"http://")
        || starts_with_i(url, L"https://")
        || starts_with_i(url, L"mailto:");
}

NavigationService::NavigateResult NavigationService::HandleLinkClick(
    std::wstring_view url, [[maybe_unused]] std::wstring_view current_file)
{
    NavigateResult result;
    if (url.empty()) {
        return result;
    }
    // 内部アンカーリンク: #something
    if (url[0] == L'#') {
        result.type = NavigateResult::Type::Anchor;
        result.target = url.substr(1);
        return result;
    }
    // 安全なスキームの外部リンクのみ許可
    if (!IsSafeUrlScheme(url)) {
        return result;
    }
    result.type = NavigateResult::Type::ExternalUrl;
    result.target = url;
    return result;
}

NavigationService::NavigateResult NavigationService::MakeResultFromEntry(
    NavEntry&& entry, std::wstring_view current_file)
{
    NavigateResult result;
    if (entry.file_path != current_file && !entry.file_path.empty()) {
        result.type = NavigateResult::Type::LoadFile;
        result.target = std::move(entry.file_path);
        result.scroll_y = entry.scroll_y;
    }
    else {
        result.type = NavigateResult::Type::Anchor;
        result.scroll_y = entry.scroll_y;
    }
    return result;
}

NavigationService::NavigateResult NavigationService::GoBack(
    std::wstring_view current_file, float scroll_y)
{
    NavEntry out;
    if (!history_.GoBack(NavEntry{ current_file, scroll_y }, out)) {
        return {};
    }
    return MakeResultFromEntry(std::move(out), current_file);
}

NavigationService::NavigateResult NavigationService::GoForward(
    std::wstring_view current_file, float scroll_y)
{
    NavEntry out;
    if (!history_.GoForward(NavEntry{ current_file, scroll_y }, out)) {
        return {};
    }
    return MakeResultFromEntry(std::move(out), current_file);
}

void NavigationService::PushHistory(std::wstring_view file, float scroll_y)
{
    history_.Push(NavEntry{ file, scroll_y });
}

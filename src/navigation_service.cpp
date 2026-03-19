#include "navigation_service.h"

NavigationService::NavigateResult NavigationService::HandleLinkClick(
    const std::wstring& url, const std::wstring& current_file) {
    NavigateResult result;
    if (url.empty()) return result;

    // Internal anchor link: #something
    if (url[0] == L'#') {
        result.type = NavigateResult::Type::Anchor;
        result.target = url.substr(1);
        return result;
    }

    // External link
    result.type = NavigateResult::Type::ExternalUrl;
    result.target = url;
    return result;
}

NavigationService::NavigateResult NavigationService::MakeResultFromEntry(
    NavEntry&& entry, const std::wstring& current_file) {
    NavigateResult result;
    if (entry.file_path != current_file && !entry.file_path.empty()) {
        result.type = NavigateResult::Type::LoadFile;
        result.target = std::move(entry.file_path);
        result.scroll_y = entry.scroll_y;
    } else {
        result.type = NavigateResult::Type::Anchor;
        result.scroll_y = entry.scroll_y;
    }
    return result;
}

NavigationService::NavigateResult NavigationService::GoBack(
    const std::wstring& current_file, float scroll_y) {
    NavEntry out;
    if (!history_.GoBack({current_file, scroll_y}, out)) {
        return {};
    }
    return MakeResultFromEntry(std::move(out), current_file);
}

NavigationService::NavigateResult NavigationService::GoForward(
    const std::wstring& current_file, float scroll_y) {
    NavEntry out;
    if (!history_.GoForward({current_file, scroll_y}, out)) {
        return {};
    }
    return MakeResultFromEntry(std::move(out), current_file);
}

void NavigationService::PushHistory(const std::wstring& file, float scroll_y) {
    history_.Push({file, scroll_y});
}

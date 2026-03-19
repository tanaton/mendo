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

NavigationService::NavigateResult NavigationService::GoBack(
    const std::wstring& current_file, float scroll_y) {
    NavEntry out;
    if (!history_.GoBack({current_file, scroll_y}, out)) {
        return {};
    }

    NavigateResult result;
    if (out.file_path != current_file && !out.file_path.empty()) {
        result.type = NavigateResult::Type::LoadFile;
        result.target = out.file_path;
        result.scroll_y = out.scroll_y;
    } else {
        result.type = NavigateResult::Type::Anchor;
        result.scroll_y = out.scroll_y;
    }
    return result;
}

NavigationService::NavigateResult NavigationService::GoForward(
    const std::wstring& current_file, float scroll_y) {
    NavEntry out;
    if (!history_.GoForward({current_file, scroll_y}, out)) {
        return {};
    }

    NavigateResult result;
    if (out.file_path != current_file && !out.file_path.empty()) {
        result.type = NavigateResult::Type::LoadFile;
        result.target = out.file_path;
        result.scroll_y = out.scroll_y;
    } else {
        result.type = NavigateResult::Type::Anchor;
        result.scroll_y = out.scroll_y;
    }
    return result;
}

void NavigationService::PushHistory(const std::wstring& file, float scroll_y) {
    history_.Push({file, scroll_y});
}

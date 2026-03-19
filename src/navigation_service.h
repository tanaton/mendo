#pragma once
#include "nav_history.h"
#include "document_utils.h"
#include <string>
#include <vector>

class NavigationService {
public:
    explicit NavigationService(NavHistory& history) : history_(history) {}

    struct NavigateResult {
        enum class Type { None, Anchor, ExternalUrl, LoadFile };
        Type type = Type::None;
        std::wstring target;
        float scroll_y = 0.0f;
    };

    // リンククリック処理。結果を返すだけで副作用は起こさない。
    NavigateResult HandleLinkClick(const std::wstring& url,
                                   const std::wstring& current_file);

    // 戻る
    NavigateResult GoBack(const std::wstring& current_file, float scroll_y);

    // 進む
    NavigateResult GoForward(const std::wstring& current_file, float scroll_y);

    // 履歴にプッシュ
    void PushHistory(const std::wstring& file, float scroll_y);

    bool CanGoBack() const { return history_.CanGoBack(); }
    bool CanGoForward() const { return history_.CanGoForward(); }

private:
    NavHistory& history_;
};

#pragma once
#include "config_service.h"
#include "pane_controller.h"
#include <string_view>
#include <memory_resource>

// セッション状態（最後に開いたファイル、ペイン構成、スクロール位置）の永続化を担当する
class SessionService {
public:
    explicit SessionService(ConfigService& config) noexcept : config_(config) {}

    void SaveLastFilePath(std::wstring_view path);
    std::pmr::wstring LoadLastFilePath() const;

    void SavePaneState(const PaneController& panes);
    void LoadPaneState(PaneController& panes, float client_width);

    struct ScrollPosition {
        int node = -1;
        int offset = 0;
    };
    void SaveScrollPosition(int node, float scroll_y, float node_y);
    ScrollPosition LoadScrollPosition() const;

private:
    ConfigService& config_;
};

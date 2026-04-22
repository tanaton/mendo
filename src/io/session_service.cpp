#include "session_service.h"
#include <algorithm>
#include <cmath>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

void SessionService::SaveLastFilePath(std::wstring_view path)
{
    if (path.empty()) {
        return;
    }
    config_.SaveWString("Session", "LastFile", path);
}

std::pmr::wstring SessionService::LoadLastFilePath() const
{
    std::pmr::wstring path = config_.LoadWString("Session", "LastFile");
    if (path.empty()) {
        return {};
    }
    // 安全なローカルファイルパスであることを検証
    // UNCパス (\\server\...) やデバイスパス (\\.\, \\?\) をブロック
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        return {};
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return {};
    }
    return path;
}

void SessionService::SavePaneState(const PaneController& panes)
{
    config_.SaveBool("Pane", "ShowFile", panes.IsFilePaneVisible());
    config_.SaveBool("Pane", "ShowToc", panes.IsTocPaneVisible());
    config_.SaveInt("Pane", "FileWidth", static_cast<int>(std::lround(panes.GetFilePaneWidth())));
    config_.SaveInt("Pane", "TocWidth", static_cast<int>(std::lround(panes.GetTocPaneWidth())));
}

void SessionService::LoadPaneState(PaneController& panes, float client_width)
{
    panes.SetFilePaneVisible(config_.LoadBool("Pane", "ShowFile", true));
    panes.SetTocPaneVisible(config_.LoadBool("Pane", "ShowToc", true));

    constexpr int DEFAULT_WIDTH = static_cast<int>(PaneController::PANE_DEFAULT_WIDTH);
    constexpr int MIN_WIDTH = static_cast<int>(PaneController::PANE_MIN_WIDTH);

    // クライアント幅に基づいて有効な最大ペイン幅を計算する
    int dynamic_max = DEFAULT_WIDTH;
    if (client_width > 0.0f) {
        dynamic_max = std::max(MIN_WIDTH, static_cast<int>(client_width) - MIN_WIDTH);
    }

    // LoadInt は範囲外時に DEFAULT_WIDTH を返すが、その既定値自体が
    // 狭いウィンドウでは dynamic_max を超えうる。最終結果も clamp してから
    // 適用し、SetXxxPaneWidth 側の最小値 clamp に過剰な幅が漏れないようにする。
    const int file_w = std::clamp(
        config_.LoadInt("Pane", "FileWidth", DEFAULT_WIDTH, MIN_WIDTH, dynamic_max),
        MIN_WIDTH, dynamic_max);
    const int toc_w = std::clamp(
        config_.LoadInt("Pane", "TocWidth", DEFAULT_WIDTH, MIN_WIDTH, dynamic_max),
        MIN_WIDTH, dynamic_max);
    panes.SetFilePaneWidth(static_cast<float>(file_w));
    panes.SetTocPaneWidth(static_cast<float>(toc_w));
}

void SessionService::SaveScrollPosition(int node, float scroll_y, float node_y)
{
    const int offset = static_cast<int>(std::lround(scroll_y - node_y));
    config_.SaveInt("Session", "ScrollNode", node);
    config_.SaveInt("Session", "ScrollOffset", offset);
}

SessionService::ScrollPosition SessionService::LoadScrollPosition() const
{
    return {
        .node = config_.LoadInt("Session", "ScrollNode", -1, -1, 1000000),
        .offset = config_.LoadInt("Session", "ScrollOffset", 0, -1000000, 1000000),
    };
}

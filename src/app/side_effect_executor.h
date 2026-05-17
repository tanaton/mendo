#pragma once
#include "side_effect.h"
#include <string_view>

class IWin32Host;
class FileWatcher;
class LayoutService;
struct AppState;

template <class Cb>
class SideEffectExecutorT {
public:
    void Init(IWin32Host& host, FileWatcher& file_watcher, AppState& state, LayoutService& layout_service, Cb cb);
    void Execute(const SideEffectList& effects);
    void ExecuteOne(const SideEffect& e);

private:
    IWin32Host* host_ = nullptr;
    FileWatcher* file_watcher_ = nullptr;
    AppState* state_ = nullptr;
    LayoutService* layout_service_ = nullptr;
    Cb cb_{};
};

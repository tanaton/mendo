#pragma once
#include "ui_types.h"
#include <cstdint>
#include <functional>

// context_menu.h と同じく、Win32 の公開ヘッダを App に漏らさない。
struct HWND__;
using HWND = HWND__*;

namespace i18n {
struct Strings;
}

struct SettingsDialogParams {
    PopupEnv env;
    uint64_t diagram_cache_bytes = 0;
    // 言語を選んだ。nullptr は OS に合わせる。呼んだ後にダイアログ自身の文言も選んだ言語で組み直す。
    std::function<void(const i18n::Strings*)> on_language;
    std::function<void()> on_clear_cache;
};

// 本体と同じ色で描くため自前描画。
class SettingsDialog {
public:
    // 閉じるまで戻らない (入力はダイアログが取る)。
    static void Show(HWND owner_hwnd, const SettingsDialogParams& params);

    // 定義は settings_dialog_impl.h。開くたびに作る。テストはウィンドウを作らずに Impl の各段階を直接呼ぶ。
    struct Impl;
};

#pragma once
#include <cstdint>
#include <functional>
#include <memory>

struct Theme;

// context_menu.h と同じく、Win32 / D2D / DWrite の公開ヘッダを App に漏らさない。
struct HWND__;
using HWND = HWND__*;
struct ID2D1Factory;
struct IDWriteFactory;

namespace i18n {
struct Strings;
}

struct SettingsDialogParams {
    float dpi_scale = 1.0f;
    const Theme* theme = nullptr;
    uint64_t diagram_cache_bytes = 0;
    // 言語を選んだ。nullptr は OS に合わせる。呼んだ後にダイアログ自身の文言も選んだ言語で組み直す。
    std::function<void(const i18n::Strings*)> on_language;
    std::function<void()> on_clear_cache;
};

// 本体と同じ色で描くため自前描画。開いている間は本体を無効にする (モーダル)。
class SettingsDialog {
public:
    SettingsDialog();
    ~SettingsDialog();

    SettingsDialog(const SettingsDialog&) = delete;
    SettingsDialog& operator=(const SettingsDialog&) = delete;

    // App の初期化時に 1 回呼ぶ。
    void Init(ID2D1Factory* d2d_factory, IDWriteFactory* dwrite_factory);

    // 閉じるまで戻らない。
    void Show(HWND owner_hwnd, const SettingsDialogParams& params);

    // 定義は settings_dialog_impl.h。テストはウィンドウを作らずに Impl の各段階を直接呼ぶ。
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

#pragma once
#include "measure_backend.h"
#include "theme.h"

// lifecycle 系は UI スレッドからのみ呼ぶ。並列計測中は呼ばない契約。
class ITextMeasurer : public IMeasureBackend {
public:
    virtual bool Init(const Theme& theme) = 0;
    virtual bool RecreateFormats() = 0;
    virtual void UpdateTheme(const Theme& theme) noexcept = 0;
};

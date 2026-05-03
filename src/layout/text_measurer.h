#pragma once
#include "measure_backend.h"

// 既存呼び出しサイト互換用の合成 IF。新規コードは IMeasureBackend を直接使うこと。
// DWriteTextMeasurer / MockTextMeasurer などの実装は引き続き ITextMeasurer を継承し、
// 両 IF のメソッドをまとめて override する。
class ITextMeasurer : public IMeasureBackend, public IMeasureLifecycle {
};

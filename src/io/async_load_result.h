#pragma once
#include "document.h"
#include "layout_cache.h"
#include "reload.h"
#include <memory>
#include <optional>
#include <string>

// 同一ファイルのリロードで、worker がパース前に差分判定まで済ませた結果。
// NoChange / DeferPrefixShrink はパースせずに返す (doc は空)。
struct ReloadCheck {
    ReloadDecision decision{ ReloadOp::NoChange, std::string_view::npos };
    // 判定に使った旧テキスト。UI 側の文書がこれと同一のときだけ decision を信用できる。
    // 弱参照なので、判定後に UI が文書を差し替えれば旧テキストはその時点で解放される。
    std::weak_ptr<const std::pmr::string> base;
    std::pmr::wstring path;
    size_t loaded_byte_size = 0;
};

// ワーカースレッドでのパース結果。cache は doc 向けに EstimateNodeHeights 済み。
struct AsyncLoadResult {
    Document doc;
    LayoutCache cache;
    std::optional<ReloadCheck> reload;
};

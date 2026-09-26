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

// ワーカースレッドでのパース結果。heights_estimated=false は preload (Theme 不在) から
// 来たケースで、UI スレッド側で EstimateNodeHeights を補完する必要がある。
struct AsyncLoadResult {
    Document doc;
    LayoutCache cache;
    bool heights_estimated = true;
    std::optional<ReloadCheck> reload;
};

#pragma once
#include "document.h"
#include "layout_cache.h"

// ワーカースレッドでのパース結果。heights_estimated=false は preload (Theme 不在) から
// 来たケースで、UI スレッド側で EstimateNodeHeights を補完する必要がある。
struct AsyncLoadResult {
    Document doc;
    LayoutCache cache;
    bool heights_estimated = true;
};

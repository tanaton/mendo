#pragma once
#include "document.h"
#include "file_loader.h"
#include "layout_cache.h"
#include "document_utils.h"
#include <expected>
#include <memory>
#include <optional>
#include <string>

// 同一ファイルのリロードで worker に渡す旧文書。text はパース前の差分判定に、nodes はパース後に
// 変わらなかったノードを数えるのに使う。text が空ならリロードとして扱わない。
struct ReloadBase {
    std::shared_ptr<const std::pmr::string> text;
    std::weak_ptr<const NodeStorage> nodes;
};

// 同一ファイルのリロードで、worker がパース前に差分判定まで済ませた結果。
// NoChange / Defer* はパースせずに返す (doc は空)。
struct ReloadCheck {
    ReloadDecision decision{ ReloadOp::NoChange, std::string_view::npos };
    // 判定に使った旧テキスト。UI 側の文書がこれと同一のときだけ decision と ends を信用できる。
    // 弱参照なので、判定後に UI が文書を差し替えれば旧テキストはその時点で解放される。
    std::weak_ptr<const std::pmr::string> base;
    std::pmr::wstring path;
    size_t loaded_byte_size = 0;
    // 旧文書のノードと比べて変わらなかった先頭と末尾のノード数 (Replace のとき)。大きな文書で UI を止めないよう worker で数える。
    // パース中に UI が旧文書を手放していれば空。
    std::optional<UnchangedEnds> ends;
};

// ワーカースレッドでのパース結果。cache は doc 向けに EstimateNodeHeights 済み。
struct AsyncLoadResult {
    Document doc;
    LayoutCache cache;
    std::optional<ReloadCheck> reload;
};

// 非同期ロード 1 回分の結果。成功時は AsyncLoadResult、失敗時はその理由。
using AsyncLoadOutcome = std::expected<AsyncLoadResult, FileLoadError>;

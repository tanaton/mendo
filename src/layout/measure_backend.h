#pragma once
#include "document_types.h"
#include "layout_cache.h"
#include "syntax.h"
#include "theme.h"
#include <memory_resource>

// per-node 計測専用 IF。const 仮想で thread-safe な API のみを露出する。
// IDWriteFactory はスレッドセーフで IDWriteTextLayout は per-call 生成のため、
// 同一インスタンスを複数ワーカーから const 経由で叩いて良い。lifecycle 系
// (Init/RecreateFormats/UpdateTheme) は IMeasureLifecycle に分離してあり、
// 並列計測中に呼ばない契約。
class IMeasureBackend {
public:
    virtual ~IMeasureBackend() = default;

    // tokens_out が非 nullptr の場合、CodeBlock の Tokenize 結果を Node ではなく
    // 当該 vector に書き出す (per-node 並列計測用)。Node の syntax_tokens は
    // 触らない (UI スレッドで集約後に書き戻す責務は呼び出し側にある)。
    // tokens_out == nullptr (default) のシリアル経路では従来通り
    // node.syntax_tokens_mut() に直接書き込む。
    virtual void MeasureNode(
        Node& node, NodeLayoutEntry& entry, float max_width,
        std::pmr::vector<SyntaxToken>* tokens_out = nullptr) const = 0;
    virtual void MeasureTable(Node& node, NodeLayoutEntry& entry, float max_width) const = 0;
};

// セットアップ系 IF。UI スレッドからのみ呼び、並列計測中は呼ばない契約。
class IMeasureLifecycle {
public:
    virtual ~IMeasureLifecycle() = default;
    virtual bool Init(const Theme& theme) = 0;
    virtual bool RecreateFormats() = 0;
    virtual void UpdateTheme(const Theme& theme) noexcept = 0;
};

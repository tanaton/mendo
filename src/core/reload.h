#pragma once
// 同一ファイルのリロード判定ロジック。
// 旧/新コンテンツの UTF-8 テキストを比較し、リロード方針を決定する純粋関数群と、
// 差分位置 (UTF-8 byte オフセット) を対応するノードへ写像する
// ルックアップを提供する。OnParseComplete / DoReloadCurrentFile の同一ファイル
// 再読み込み時の分岐を統一するために 1 か所に集約している。
#include "document_types.h"
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <vector>

// 新旧コンテンツの最初の差分 UTF-8 byte 位置を返す。同一の場合は npos。
// 前提: 両テキストとも LF 正規化済みであること。old は raw_text_、new は FileLoader 由来で
// どちらも正規化済み (issue #273)。
size_t FindFirstDifference(std::string_view old_text, std::string_view new_text) noexcept;

// 同一ファイルのリロード時に取るべき操作を示す。
enum class ReloadOp : uint8_t {
    NoChange,          // 差分なし。リロード不要。
    DeferPrefixShrink, // prefix-only shrink。truncate→rewrite の前半として defer。
    DeferPartialWrite, // 読み終えた後もファイルが伸びている (書き込み途中)。差分判定もパースもせず defer。
    Replace,           // 再パースした文書に差し替え、差分位置へスクロールする。
};

struct ReloadDecision {
    ReloadOp op;
    size_t diff_pos; // NoChange のとき std::string_view::npos、それ以外は差分開始位置。
};

// 旧/新コンテンツの UTF-8 テキストを比較し、リロード方針を決定する純粋関数。
ReloadDecision AnalyzeReloadDiff(std::string_view old_text, std::string_view new_text) noexcept;

// still_growing: 読み終えた後もファイルが伸びている。書き込み途中なので差分を取らずに defer する。
inline ReloadDecision DecideReload(bool still_growing, std::string_view old_text, std::string_view new_text) noexcept
{
    if (still_growing) {
        return { ReloadOp::DeferPartialWrite, std::string_view::npos };
    }
    return AnalyzeReloadDiff(old_text, new_text);
}

// 末尾を削っただけの保存は何度読み直しても prefix shrink になるため、defer 回数に上限を設け、
// 超えたら縮んだ内容を確定版として受け入れる。
class PrefixShrinkRetry {
public:
    static constexpr uint8_t kMaxDefers = 10;

    // true なら defer を続ける (回数を 1 つ消費する)。
    bool TryDefer() noexcept
    {
        if (Exhausted()) {
            return false;
        }
        ++count_;
        return true;
    }
    bool Exhausted() const noexcept
    {
        return count_ >= kMaxDefers;
    }
    void Reset() noexcept
    {
        count_ = 0;
    }

private:
    uint8_t count_ = 0;
};

// source_offset が diff_offset 以下の最後のノードを返す。該当なしの場合は -1。
[[nodiscard]] int FindNodeBySourceOffset(const std::pmr::vector<Node>& nodes, size_t diff_offset) noexcept;

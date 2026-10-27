#include "parser.h"
#include "ascii_util.h"
#include "document_utils.h"
#include "syntax.h"
#include "profiler.h"
#include "utility.h"
#include "utf8_codec.h"
#include "md4c.h"
#include <cassert>
#include <unordered_map>
#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <iterator>
#include <limits>
#include <ranges>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace {

// 装飾 span が付ける TextRun のフラグ。装飾でない span は 0。
constexpr uint8_t RunFlagOf(MD_SPANTYPE type) noexcept
{
    switch (type) {
    case MD_SPAN_STRONG:
        return TextRun::kBold;
    case MD_SPAN_EM:
        return TextRun::kItalic;
    case MD_SPAN_CODE:
        return TextRun::kCode;
    case MD_SPAN_DEL:
        return TextRun::kStrikethrough;
    default:
        return 0;
    }
}

// Node の深さ系フィールドは int8_t なので、極端に深い入れ子は最大値で頭打ちにする。
constexpr int8_t SaturateToInt8(int value) noexcept
{
    return static_cast<int8_t>(std::min(value, int{ std::numeric_limits<int8_t>::max() }));
}

struct ParseContext {
    std::stop_token stop_token;
    // md4c コールバックは 100MB 入力で 50 万回以上呼ばれるため、毎回 atomic load せず
    // 1024 callbacks ごとに stop_requested() を確認する間引きカウンタ。
    // 最大 1024 callback 分の検知遅延は 600ms parse に対し μs オーダーで許容範囲。
    uint32_t cancel_check_counter = 0;
    bool cancel_requested = false;

    bool ShouldCancel() noexcept
    {
        if (cancel_requested) {
            return true;
        }
        if ((++cancel_check_counter & 0x3FFu) != 0u) {
            return false;
        }
        cancel_requested = stop_token.stop_requested();
        return cancel_requested;
    }

    std::pmr::vector<Node> nodes;
    // 原文をそのまま使えないテキストと、runs・リンク URL の置き場所。NodeStorage として Document に渡す。
    TextArena arena;
    // Table ノードが指すテーブル本体。arena と同じく Document に渡す。
    std::pmr::vector<std::unique_ptr<NodeTableData>> tables;

    NodeIndices indices;
    std::pmr::vector<size_t> blockquote_indices;
    // [id - 1] が脚注 id への参照数。md4c の ref_id はテーブル行の判定でも数えられて飛ぶので自前で数える。
    std::pmr::vector<uint32_t> footnote_ref_counts;
    // 最後に記録したソース位置。RecordSourceOffset がこれより前の位置を記録しないために持つ。
    size_t last_source_offset = 0;

    // 装飾 span のネスト追跡は md4c 推奨のカウンタ方式。enter で +1, leave で -1。
    // [std::countr_zero(RunFlagOf(type))] が span の種別ごとの深さ。CommonMark で link はネスト禁止のため
    // link_url_index のみ単純フィールド (-1 = リンク外)。
    std::array<uint8_t, 4> span_depths{};
    // span_depths を TextRun のフラグビットに射影したキャッシュ。MakeRun のたびに 4 回
    // ビット操作するのではなく、span enter/leave のときだけ 1 回ビット OR/AND を更新する。
    // 不変式: bit X が立っている <=> 対応する span_depths > 0。
    uint8_t current_run_flags = 0;
    // 現在の <a> span に対応する current_link_urls のインデックス。-1 = リンク外。
    // CommonMark で <a> はネスト禁止のため leave までこの値が安定する。
    int16_t current_link_url_index = -1;

    // 現在ノード用のテキストと runs の蓄積スクラッチ (UTF-8)。FinalizeCurrentNode で DocumentData::raw を
    // 指すか arena へコピーされる。runs は伸長のたびに確保し直さないよう、ここに貯めて 1 回だけコピーする。
    std::pmr::string current_text;
    std::pmr::vector<TextRun> current_runs;
    // 文字参照を解いた href / src (AttributeText)。
    std::pmr::string attr_scratch;

    // current_text 内の「未確定 TextRun」の開始位置 (UTF-8 byte unit)。
    // 同じ span 状態で連続する AppendDoc は 1 つの TextRun に統合される。
    // span 切替 / ブロック退出時に FlushPendingRun が呼ばれて確定する。
    uint32_t pending_run_start = 0;
    bool has_pending_run = false;
    // pending_run_start 以降に AppendDoc で押し込まれた改行の数。FlushPendingRun の count 走査を排除。
    // md4c の \n を size==1 の単独 chunk としてしか OnText に渡さない契約 (BR/SOFTBR/HTML 改行/code 行末) に依存している。
    int32_t pending_run_newlines = 0;

    int indent_level = 0;
    int blockquote_depth = 0;
    int blockquote_group_counter = 0;  // 最外側 blockquote に入るたびに進める group ID（ネスト中は共有）
    int outermost_quote_indent = 0;    // 最外側 blockquote 進入時の indent_level（描画時のバー起点）

    // リスト追跡: 順序なしリストは -1、順序ありリストは次の番号 (0 以上)。
    // スタックアダプタを挟まず vector を直接扱う (back/push_back/pop_back)。
    std::pmr::vector<int> list_counter;

    Node* current_node = nullptr;

    // AppendDoc / FlushPendingRun のターゲットバッファのキャッシュ。
    // 47-94 万回呼ばれる hot path で毎回テーブルセル内かを判定しないよう、
    // 状態遷移点 (BeginNode / TD/TH 進入退出 / current_node clear) で更新したポインタを直接使う。
    // &current_text 以外を指すのはテーブルセル内 (NodeTableData::concat_text) のときだけ。
    // nullptr のときは AppendDoc / FlushPendingRun は no-op。
    std::pmr::string* active_text_buffer = nullptr;

    // 現在ノードのリンク URL。各 URL は DocumentData::raw か arena 上にあり、FinalizeCurrentNode で配列だけ arena へコピーする。
    std::pmr::vector<std::string_view> current_link_urls;
    // current_link_urls の URL -> インデックス索引。URL 数が kLinkUrlLinearScanMax を
    // 超えたノードでのみ構築する (テーブルは全セルで 1 ノードのため数万 URL になりうる)。
    static constexpr size_t kLinkUrlLinearScanMax = 8;
    std::pmr::unordered_map<std::string_view, int16_t> link_url_lookup;

    // ネスト画像 (![a ![b](inner)](outer)) では最外側の src を採用するため深度を追跡する。
    int image_span_depth = 0;
    // 現在ノードの最後の画像の src。段落の終わりで Image ノードへ昇格するときに Node へ移す。
    std::string_view image_src;

    // display math スパンが 1 個だけで他の内容が無い段落を LatexMath コードブロックに昇格する状態
    int paragraph_display_math_count = 0;
    bool paragraph_has_other_content = false;

    // md_parse() に渡した入力バッファ。source_offset 計算と、md4c が渡すポインタが原文を指すかの判定に使う。
    std::string_view markdown;

    // 改行の位置で区切ると決めたが、まだ区切っていない。後に本文が続くときだけ区切り、末尾に空のノードを作らない。
    // 改行は足さずに数えて待ち、区切るときは 1 個を継ぎ目として取り除き、残り (空行) を続きの頭に置く。
    // 区切らずにブロックが終わったら前のノードに戻す。SOFTBR は代わりの空白を前のノードに足してから待つ。
    uint32_t pending_newlines = 0;
    bool pending_joined = false;
    // 区切った (色分けする) コードブロックの並びの先頭。パースを終えてから AnnotateCodeChunkLeads に渡す。
    std::pmr::vector<size_t> split_code_runs;

    // 現在ノードのテキストを arena へコピーすることが確定しているか。span markup (** _ ` 等) の存在や、
    // entity 解決 / BR / SOFTBR で text を置換するケースは current_text と raw_slice が
    // 構造的に一致しなくなるため、FinalizeCurrentNode の memcmp を完全にスキップできる。
    bool current_node_needs_copy = false;

    // current_text が source の (offset, current_text.size()) 範囲とバイト一致するか。
    // 一致するノードは arena にコピーせず DocumentData::raw を指せる。
    // span マークアップ (** _ ` 等) や BR/SOFTBR/ENTITY 混在のノードは不一致で arena へのコピーに落ちる。
    // offset は呼び出し側で SourceOffset() から取り出した値を渡す (FinalizeCurrentNode で再利用するため)。
    bool CurrentTextMatchesRawSliceAt(size_t offset) const noexcept
    {
        // 高頻度呼び出し (100MB で 40万回超) のため MENDO_PROFILE は外す。
        // zone overhead が ~120ms 単位で計測自体を歪めるため。
        const size_t len = current_text.size();
        if (offset + len > markdown.size()) {
            return false;
        }
        // current_node_needs_copy で span/entity 混在は事前に弾けるため、ここまで来たノードは大半が
        // 全長一致 (success) になる。probe 短絡は worst-case で len 分の比較が走り意味がないため、
        // 1 回の memcmp に統一する。
        return std::char_traits<char>::compare(markdown.data() + offset, current_text.data(), len) == 0;
    }

    // 現在ノードのテキストと runs のスクラッチを Node に確定し、スクラッチをクリアする。
    // BeginNode 直前と各 OnLeaveBlock の current_node 解除直前に呼ぶ。
    // スクラッチの capacity は保持して次ノードで再利用する (確保回数削減のため)。
    void FinalizeCurrentNode()
    {
        CancelPendingSplit();
        FlushPendingRun();
        if (current_node) {
            // 昇格処理 (TryPromoteParagraphToDisplayMath 等) がテキストを直接設定済みのノードは、
            // current_text で上書きすると line_count ごと巻き戻るのでスキップする。
            if (!current_text.empty() && !current_node->HasText()) {
                const auto offset = current_node->SourceOffset();
                const bool matches_raw = !current_node_needs_copy && offset != kUnsetSourceOffset && CurrentTextMatchesRawSliceAt(offset);
                const std::string_view text = matches_raw
                                                  ? markdown.substr(offset, current_text.size())
                                                  : arena.CopyText(current_text);
                current_node->SetText(text);
            }
            if (!current_runs.empty()) {
                current_node->SetRuns(arena.Copy(current_runs));
            }
            if (!current_link_urls.empty()) {
                current_node->SetLinkUrls(arena.Copy(current_link_urls));
            }
        }
        current_text.clear();
        current_runs.clear();
        current_link_urls.clear();
        link_url_lookup.clear();
    }

    // ノードを追加するのは BeginNode だけなので、最後に始めたノードは常に末尾にある。
    size_t CurrentNodeIndex() const noexcept
    {
        return nodes.size() - 1;
    }

    // current_node と active_text_buffer は対で管理する不変式があるため一括で nullptr に倒す。
    void EndNode()
    {
        FinalizeCurrentNode();
        current_node = nullptr;
        active_text_buffer = nullptr;
    }

    // 予約を使い切ったら、読み終えた入力の割合から最終的なノード数を見積もって 1 回で広げる。
    // リンク集のようにノードが細かい文書でも 1.5 倍ずつの伸長 (全ノードの移動) を繰り返さない。
    // ソース位置を持たないノード (HR 等) だけが続くと見積もりが過大になるので 4 倍で頭打ちにする。
    void GrowNodes()
    {
        const size_t size = nodes.size();
        const size_t projected = size * markdown.size() / std::max<size_t>(last_source_offset, 1);
        nodes.reserve(std::clamp(projected + projected / 8, size + size / 2, size * 4));
    }

    void BeginNode(NodeType type)
    {
        // UL/OL/QUOTE の進入はノードを閉じないので、LI の中の最初の子ブロックが始まる時点ではまだ LI が開いている。
        if (current_node && IsListItem(*current_node)) {
            current_node->set_has_child_blocks();
        }
        FinalizeCurrentNode();
        if (nodes.size() == nodes.capacity()) [[unlikely]] {
            GrowNodes();
        }
        nodes.emplace_back();
        current_node = &nodes.back();
        active_text_buffer = &current_text;
        image_src = {};
        current_node->type = type;
        current_node->indent_level = SaturateToInt8(indent_level);
        if (blockquote_depth > 0) {
            current_node->blockquote_group = blockquote_group_counter;
            current_node->quote_depth = SaturateToInt8(blockquote_depth);
            current_node->quote_outer_indent = SaturateToInt8(outermost_quote_indent);
        }
        current_node_needs_copy = false;
    }

    // 区切るのは段落・引用・コードブロック (図を除く) だけ。見出しやリスト項目は区切ると記号や下線が増え、
    // 図は 1 つのコードから描く。
    bool CanSplitCurrentNode() const noexcept
    {
        if (active_text_buffer != &current_text) {
            return false;
        }
        switch (current_node->type) {
        case NodeType::Paragraph:
        case NodeType::BlockQuote:
            return true;
        case NodeType::CodeBlock:
            return !IsDiagramLanguage(current_node->code_language);
        default:
            return false;
        }
    }

    // 現在のノードをここで閉じ、同じ種別の続きのノードを始める。開いている装飾とリンクは続きにも掛ける。
    void SplitCurrentNode(bool newline_before)
    {
        const std::string_view link_url = OpenLinkUrl();
        const NodeType type = current_node->type;
        const SyntaxLanguage language = current_node->code_language;
        if (type == NodeType::CodeBlock && language != SyntaxLanguage::None && !current_node->continues_prev()) {
            split_code_runs.push_back(CurrentNodeIndex());
        }
        current_node->set_continues_next();
        BeginNode(type);
        current_node->code_language = language;
        current_node->set_continues_prev(newline_before);
        ResolveLinkUrlIndex(link_url);
    }

    constexpr bool HasPendingSplit() const noexcept
    {
        return pending_newlines > 0 || pending_joined;
    }

    // 続きの頭に置く空行が目安の長さを超えると、そこでまた区切りを待つので、待ちが無くなるまで繰り返す。
    void ApplyPendingSplit()
    {
        while (HasPendingSplit()) {
            const uint32_t newlines = std::exchange(pending_newlines, 0);
            pending_joined = false;
            SplitCurrentNode(newlines > 0);
            for (uint32_t k = 1; k < newlines; ++k) {
                AppendLineBreak("\n");
            }
        }
    }

    // 区切らずにブロックが終わるので、待っていた改行を前のノードに戻す。
    void CancelPendingSplit()
    {
        for (; pending_newlines > 0; --pending_newlines) {
            AppendDoc("\n");
        }
        pending_joined = false;
    }

    // window の中で最後の和文の句読点 (、。，．！？) の直後。無ければ npos。
    // 1 バイトずつ調べると、句読点の無い 16MB の 1 行で毎回窓を調べ切り、パースが約 3ms (3 割) 遅くなる。
    // 2 バイト目 (0x80 / 0xBC) は和文でもまれなので、ベクトル化される find_last_of でまとめて探してから前後を確かめる。
    static size_t FindLastCjkPunctuationEnd(std::string_view window) noexcept
    {
        constexpr std::string_view kSecondBytes = "\x80\xBC";
        for (size_t mid = window.find_last_of(kSecondBytes); mid != std::string_view::npos && mid > 0;
             mid = window.find_last_of(kSecondBytes, mid - 1)) {
            if (mid + 1 >= window.size()) {
                continue;
            }
            const auto b0 = static_cast<uint8_t>(window[mid - 1]);
            const auto b1 = static_cast<uint8_t>(window[mid]);
            const auto b2 = static_cast<uint8_t>(window[mid + 1]);
            if ((b0 == 0xE3 && b1 == 0x80 && (b2 == 0x81 || b2 == 0x82)) ||
                (b0 == 0xEF && b1 == 0xBC && (b2 == 0x81 || b2 == 0x8C || b2 == 0x8E || b2 == 0x9F))) {
                return mid + 2;
            }
        }
        return std::string_view::npos;
    }

    // chunk を先頭から target バイト付近で区切る位置 (target < chunk.size())。前後 kTextChunkWindow にある最後の空白の
    // 直後で区切り、空白が無ければ和文の句読点の直後、それも無ければ target の手前の文字の境目で区切る。
    // 語の途中で区切ると、見た目も語の途中で改行し、検索や語の選択も継ぎ目をつながないと切れる。後ろに必ず 1 バイト以上残す。
    static size_t FindChunkCut(std::string_view chunk, size_t target) noexcept
    {
        const size_t lo = target > kTextChunkWindow ? target - kTextChunkWindow : 0;
        const size_t hi = std::min(chunk.size() - 1, target + kTextChunkWindow);
        const std::string_view window = chunk.substr(lo, hi - lo);
        if (const size_t space = window.find_last_of(" \t"); space != std::string_view::npos) {
            return lo + space + 1;
        }
        if (const size_t punct = FindLastCjkPunctuationEnd(window); punct != std::string_view::npos) {
            return lo + punct;
        }
        // 文字の境目まで戻れないほど壊れた UTF-8 でも、区切りが進むよう target で切る。
        const size_t cut = utf8_codec::SnapToCpStart(chunk, static_cast<uint32_t>(target));
        return cut > 0 ? cut : target;
    }

    // 本文を足す。区切りを待っているか目安の長さを大きく超えるときだけ区切る。
    // 100MB の入力で数百万回呼ばれるので、ほとんどの呼び出しは比較だけで AppendDoc に進める
    // (表のセル内では current_text を使わないが、比べても結果は CanSplitCurrentNode で弾かれる)。
    void AppendSplittable(std::string_view chunk)
    {
        if (current_text.size() + chunk.size() > kTextChunkTarget + kTextChunkWindow || HasPendingSplit()) [[unlikely]] {
            chunk = SplitBeforeAppend(chunk);
        }
        AppendDoc(chunk);
    }

    // 区切ってから、現在のノードに足す chunk の残りを返す。
    __declspec(noinline) std::string_view SplitBeforeAppend(std::string_view chunk)
    {
        if (HasPendingSplit()) {
            ApplyPendingSplit();
            RecordSourceOffset(chunk);
        }
        // 区切る処理は 1 か所にまとめる。コードの区切りを別の関数にして区切りの補助関数を呼ぶと、段落の長い 1 行の
        // 区切りが 16MB で 1ms (8%) 遅くなる (呼ばない関数として置くだけなら変わらないので、展開のされ方による)。
        std::pmr::string indent;
        while (current_text.size() + chunk.size() > kTextChunkTarget + kTextChunkWindow && CanSplitCurrentNode()) {
            const size_t line_begin = CodeLineBeginToMove(chunk);
            const bool move_line = line_begin != std::string_view::npos;
            size_t cut = 0;
            if (move_line) {
                // 行の頭に積んだ字下げは続きのノードへ移す。コードには run が無いので、テキストと改行の数だけ直せばよい。
                FlushPendingRun();
                indent.assign(std::string_view{ current_text }.substr(line_begin));
                current_text.resize(line_begin - 1);
                current_node->line_count--;
            }
            else {
                cut = FindChunkCut(chunk, kTextChunkTarget - std::min(current_text.size(), kTextChunkTarget));
                AppendDoc(chunk.substr(0, cut));
            }
            SplitCurrentNode(move_line);
            if (!indent.empty()) {
                AppendDoc(indent);
                indent.clear();
            }
            chunk.remove_prefix(cut);
            RecordSourceOffset(chunk);
        }
        return chunk;
    }

    // コードブロックで、chunk を足す行がノード 1 つに収まるなら、その行の頭 (current_text での位置)。そうでなければ npos で、
    // 行の途中で区切る。前のノードが空になるときも、区切っても行の途中で区切るのと変わらないので npos。
    size_t CodeLineBeginToMove(std::string_view chunk) const noexcept
    {
        if (current_node->type != NodeType::CodeBlock) {
            return std::string_view::npos;
        }
        const size_t newline = current_text.rfind('\n');
        if (newline == std::string_view::npos || newline == 0) {
            return std::string_view::npos;
        }
        const size_t line_begin = newline + 1;
        return current_text.size() - line_begin + chunk.size() <= kTextChunkTarget + kTextChunkWindow ? line_begin : std::string_view::npos;
    }

    // 改行 ("\n" か、SOFTBR の代わりの " ") を足す。区切りの目安に近ければ、ここを区切りの候補にする。
    void AppendLineBreak(std::string_view text)
    {
        if (current_text.size() + kTextChunkWindow < kTextChunkTarget && !HasPendingSplit()) [[likely]] {
            AppendDoc(text);
            return;
        }
        AppendLineBreakNearSplit(text);
    }

    __declspec(noinline) void AppendLineBreakNearSplit(std::string_view text)
    {
        if (pending_newlines > 0 && text == "\n") {
            ++pending_newlines;
            return;
        }
        ApplyPendingSplit();
        if (current_text.size() + kTextChunkWindow < kTextChunkTarget || !CanSplitCurrentNode()) {
            AppendDoc(text);
            return;
        }
        if (text == "\n") {
            pending_newlines = 1;
            return;
        }
        AppendDoc(text);
        pending_joined = true;
    }

    // md4c は MD_TEXT_NULLCHAR / MD_TEXT_BR / MD_TEXT_SOFTBR や、CODE/LATEXMATH/HTML の改行・空白置換で
    // _T(""), _T("\n"), _T(" ") といった内部静的リテラルを text として渡すことがある。
    // 範囲外 (静的リテラル) のときは記録せず、後続の実体テキストで設定できるようにする。
    // 脚注定義のように原文の途中から文書末尾へ移るノードは、ソース位置の単調増加
    // (FindNodeBySourceOffset の二分探索の前提) を崩すので記録しない。
    void RecordSourceOffset(std::string_view chunk) noexcept
    {
        if (!IsSubrangeOf(chunk, markdown)) {
            return;
        }
        auto offset = static_cast<size_t>(chunk.data() - markdown.data());
        // コードの字下げは静的な空白で届くので、1 行目が字下げされたコードでは本文が原文と突き合わず
        // arena へコピーになる。先に積んだ字下げが直前の原文と同じなら、そこから記録する。
        const size_t pending = active_text_buffer == &current_text ? current_text.size() : 0;
        if (pending > 0 && offset >= last_source_offset + pending && markdown.compare(offset - pending, pending, current_text) == 0) {
            offset -= pending;
        }
        if (offset < last_source_offset) {
            return;
        }
        current_node->SetSourceOffset(offset);
        last_source_offset = offset;
    }

    // 引用内の段落は BlockQuote ノードとして作り、Alert 検出対象に登録する。
    void BeginParagraphNode()
    {
        const auto node_type = (blockquote_depth > 0) ? NodeType::BlockQuote : NodeType::Paragraph;
        BeginNode(node_type);
        if (node_type == NodeType::BlockQuote) {
            blockquote_indices.emplace_back(CurrentNodeIndex());
        }
    }

    // md4c は tight list の LI 直下の段落を MD_BLOCK_P で囲まないため、LI 内で見出し / HR /
    // フェンス等の後に続く本文は current_node 無しで届く。loose list と同じ段落ノードを補って受け止める。
    bool EnsureNodeForListItemText()
    {
        if (current_node) {
            return true;
        }
        // md4c は UL/OL 直下に LI しか置かないため、list_counter が空でなければ LI 内にいる。
        if (list_counter.empty()) {
            return false;
        }
        BeginParagraphNode();
        return true;
    }

    constexpr TextRun MakeRun(uint32_t start, uint32_t length)
    {
        TextRun run;
        run.start = start;
        run.length = length;
        run.set_raw_flags(current_run_flags);
        run.link_url_index = current_link_url_index;
        return run;
    }

    // ノードを閉じても続きのノードへ登録し直せるよう、開いているリンクの URL を返す。無ければ空。
    std::string_view OpenLinkUrl() const noexcept
    {
        return current_link_url_index >= 0 ? std::string_view{ current_link_urls[static_cast<size_t>(current_link_url_index)] } : std::string_view{};
    }

    // url を current_link_urls に登録し、インデックスを current_link_url_index にキャッシュする。
    void ResolveLinkUrlIndex(std::string_view url)
    {
        current_link_url_index = (current_node && !url.empty()) ? FindOrAddLinkUrl(url) : int16_t{ -1 };
    }

    // 1 ノードあたりの URL 数は典型的に < 8 で、その領域ではハッシュ計算を伴う
    // ハッシュマップより線形 memcmp の方が速い。数が増えたノードだけ link_url_lookup に切り替え、
    // テーブル等で O(n^2) になるのを防ぐ。
    int16_t FindOrAddLinkUrl(std::string_view url)
    {
        const size_t n = current_link_urls.size();
        if (n <= kLinkUrlLinearScanMax) {
            if (const auto it = std::ranges::find(current_link_urls, url); it != current_link_urls.end()) {
                return static_cast<int16_t>(it - current_link_urls.begin());
            }
        }
        else {
            if (link_url_lookup.empty()) {
                for (size_t i = 0; i < n; ++i) {
                    link_url_lookup.emplace(current_link_urls[i], static_cast<int16_t>(i));
                }
            }
            if (const auto it = link_url_lookup.find(url); it != link_url_lookup.end()) {
                return it->second;
            }
        }
        if (n >= kMaxLinkUrls) {
            return -1;
        }
        const auto new_index = static_cast<int16_t>(n);
        current_link_urls.push_back(InternUrl(url));
        if (!link_url_lookup.empty()) {
            link_url_lookup.emplace(current_link_urls.back(), new_index);
        }
        return new_index;
    }

    // md4c の href / 画像 src は原文を指すことが多いのでそのまま使う。参照定義の展開や脚注 URL のように
    // 原文の外にあるものだけ arena へコピーする。
    std::string_view InternUrl(std::string_view url)
    {
        return IsSubrangeOf(url, markdown) ? url : arena.CopyText(url);
    }

    // md4c は属性 (href / src / コードブロックの言語) の中の文字参照を解かずに MD_TEXT_ENTITY の部分として渡すので、ここで解く
    // (解かないと [a](?x=1&amp;y=2) が "&amp;" のまま開かれる)。文字参照の無い属性はそのまま返す。
    // 戻り値は次の呼び出しまで有効なので、残すなら InternUrl を通す。
    std::string_view AttributeText(const MD_ATTRIBUTE& attr)
    {
        const std::string_view whole{ attr.text, attr.size };
        if (whole.empty() || (attr.substr_types[0] == MD_TEXT_NORMAL && attr.substr_offsets[1] == attr.size)) {
            return whole;
        }
        attr_scratch.clear();
        for (size_t k = 0; attr.substr_offsets[k] < attr.size; ++k) {
            const auto sub = whole.substr(attr.substr_offsets[k], attr.substr_offsets[k + 1] - attr.substr_offsets[k]);
            char entity_buf[4];
            switch (attr.substr_types[k]) {
            case MD_TEXT_ENTITY:
                attr_scratch.append(ResolveHtmlEntity(sub, entity_buf).value_or(sub));
                break;
            case MD_TEXT_NULLCHAR:
                break;
            default:
                attr_scratch.append(sub);
                break;
            }
        }
        return attr_scratch;
    }

    // テキストを現在のノードまたはセルに追加する (UTF-8)。
    // 同じ span 状態で連続して呼ばれると 1 つの TextRun に統合される (cell も統合対象)。
    // セル切替・span 切替・ブロック退出の各タイミングで FlushPendingRun が走る前提。
    // セル内では active_text_buffer が NodeTableData::concat_text を指す。pending_run_start は
    // バッファサイズベースで、FlushPendingRun が現在セルの開始 offset を引いて cell-local にする。
    constexpr void AppendDoc(std::string_view text)
    {
        std::pmr::string* const target = active_text_buffer;
        if (!target) {
            return;
        }
        // md4c は size 0 の text コールバックを発生させない契約 (md4c.c の MD_TEXT マクロで size > 0 ガード済) なので empty 判定は省く。
        if (!has_pending_run) {
            pending_run_start = static_cast<uint32_t>(target->size());
            has_pending_run = true;
        }
        // 1-char chunk fastpath: md4c は \n / 空白 / 単一 entity 等を size==1 で渡してくるので push_back に振り分ける。
        if (text.size() == 1) {
            const char c = text[0];
            pending_run_newlines += (c == '\n');
            target->push_back(c);
        }
        else {
            target->append(text);
        }
    }

    // DirectWrite は書式を付けるたびに範囲の並びを作り直すので、1 レイアウトに数万の装飾があると
    // 計測と描画が二乗で遅くなる (1 段落に `a` を 8 万個で 20 秒超)。普通の文書が届かない数で打ち切り、
    // 残りは装飾なしのテキストにする。
    static constexpr size_t kMaxRunsPerText = 4096;

    // 未確定 TextRun を確定して runs に push する。
    // span 状態が変わる直前 (OnEnter/Leave Span)、セル切替、OnLeaveBlock の冒頭で呼ぶ。
    // line_count はノード単位なので、セル内では更新しない。
    // active_text_buffer が非 null なら current_node も非 null (両者は対で更新される)。
    // 装飾もリンクも無い run は出さない。runs の隙間は既定スタイルとして扱われる。
    constexpr void FlushPendingRun()
    {
        if (has_pending_run) {
            std::pmr::string* const buf = active_text_buffer;
            if (buf && buf->size() > pending_run_start) {
                const uint32_t length = static_cast<uint32_t>(buf->size() - pending_run_start);
                const bool plain = current_run_flags == 0 && current_link_url_index < 0;
                if (buf == &current_text) {
                    current_node->line_count += pending_run_newlines;
                    if (!plain && current_runs.size() < kMaxRunsPerText) {
                        current_runs.push_back(MakeRun(pending_run_start, length));
                    }
                }
                else if (!plain) {
                    // セル内: cell_text_starts / cell_run_starts の back() は TD/TH 進入時に積んだ現在セルの開始位置。
                    auto* const tbl = current_node->table_data();
                    if (tbl->cell_run_starts.empty()) {
                        StartCellRunStarts(*tbl);
                    }
                    if (tbl->all_runs.size() - tbl->cell_run_starts.back() < kMaxRunsPerText) {
                        tbl->all_runs.push_back(MakeRun(pending_run_start - tbl->cell_text_starts.back(), length));
                    }
                }
            }
        }
        has_pending_run = false;
        pending_run_newlines = 0;
    }

    // 装飾もリンクも無い表は cell_run_starts を持たない。セルごとに 4 byte で、短いセルが 1100 万個の 60MB の表では
    // commit が 242MB から 198MB に、private working set が 219MB から 175MB に減る (どちらも約 18%)。
    // 最初の run を積むときに、ここまでのセル (run は無い) の開始位置を埋める。
    // 以後はセルに入るたびに積むので、表のセル数分を cell_text_starts と同じだけ確保しておく。
    static void StartCellRunStarts(NodeTableData& tbl)
    {
        tbl.cell_run_starts.reserve(tbl.cell_text_starts.capacity());
        tbl.cell_run_starts.resize(tbl.cell_text_starts.size(), 0);
    }
};

// 画像 src を受け取り Image ノードへ昇格できる種別。
// 他の種別は payload を別の用途に使うため、昇格すると壊れる (issue#298)。
constexpr bool IsImagePromotionCandidate(const Node& node) noexcept
{
    return node.type == NodeType::Paragraph || node.type == NodeType::BlockQuote;
}

// MD_BLOCK_P 終了時、画像 span を含む段落 / 引用ブロックを Image ノードへ昇格させる。
// 戻り値 true なら他の昇格処理はスキップしてよい。
bool TryPromoteParagraphToImage(ParseContext* ctx)
{
    auto* const cn = ctx->current_node;
    // 区切った段落の続きを画像にすると、前の部分だけが本文として残る。
    if (!cn || !IsImagePromotionCandidate(*cn) || ctx->image_src.empty() || cn->continues_prev()) {
        return false;
    }
    const NodeImageData image{ ctx->image_src };
    cn->set_image_data(ctx->arena.Copy(std::span{ &image, 1 }).data());
    cn->type = NodeType::Image;
    ctx->indices.image_indices.emplace_back(ctx->CurrentNodeIndex());
    return true;
}

// MD_BLOCK_P 終了時、$$..$$ ブロック数式が単独の段落を LaTeX CodeBlock へ昇格させる。
// blockquote 内は引用文脈を保ちたいため Paragraph のみ昇格対象。
void TryPromoteParagraphToDisplayMath(ParseContext* ctx)
{
    auto* const node = ctx->current_node;
    if (!node || node->type != NodeType::Paragraph) {
        return;
    }
    // 昇格できる段落のテキストは "$$" + 数式 + "$$" だけ。md4c は数式内の改行を空白にして渡すので改行は無い。
    // 区切った段落は $$..$$ だけの段落ではないので昇格させない。
    const std::string_view text = ctx->current_text;
    if (ctx->paragraph_display_math_count != 1 ||
        ctx->paragraph_has_other_content ||
        node->continues_prev() ||
        text.size() <= 4) {
        return;
    }
    assert(text.starts_with("$$") && text.ends_with("$$"));
    node->type = NodeType::Diagram;
    node->code_language = SyntaxLanguage::LatexMath;
    ctx->current_runs.clear();
    node->SetText(ctx->arena.CopyText(text.substr(2, text.size() - 4)), 0);
    ctx->indices.diagram_indices.emplace_back(ctx->CurrentNodeIndex());
}

void BeginListItem(ParseContext* ctx, const MD_BLOCK_LI_DETAIL* li)
{
    const bool is_task = li->is_task;
    ctx->BeginNode(is_task ? NodeType::TaskListItem : NodeType::ListItem);
    if (is_task && (li->task_mark == 'x' || li->task_mark == 'X')) {
        ctx->current_node->set_task_checked();
    }
    // OL は start>=0、UL は番兵 -1。md4c は LI を UL/OL の直下にしか置かない。
    assert(!ctx->list_counter.empty());
    if (ctx->list_counter.back() >= 0) {
        ctx->current_node->set_list_number(ctx->list_counter.back()++);
    }
}

void BeginTable(ParseContext* ctx, const MD_BLOCK_TABLE_DETAIL* detail)
{
    ctx->BeginNode(NodeType::Table);
    ctx->indices.table_indices.emplace_back(ctx->CurrentNodeIndex());
    // 後続の TR/TH/TD で nullable チェックなく参照できるよう先に確保する。
    // これに依存して TR/TH/TD は has_table() ガードを省いている。
    auto* const tbl = ctx->tables.emplace_back(std::make_unique<NodeTableData>()).get();
    ctx->current_node->set_table_data(tbl);
    if (!detail) {
        return;
    }
    // md4c から正確なテーブルサイズが渡されるので、要素数が決まる配列は一度に reserve して
    // 余りを持たせない。セル本文の長さは分からないので concat_text は伸長に任せる。
    const size_t total_rows = static_cast<size_t>(detail->head_row_count) + detail->body_row_count;
    tbl->head_row_count = detail->head_row_count;
    // md4c は列数を 16 bit で持ち、各行をちょうどその数のセルで出す。
    tbl->col_count = static_cast<uint16_t>(detail->col_count);
    const size_t total_cells = total_rows * tbl->col_count;
    tbl->cell_text_starts.reserve(total_cells + 1);
    tbl->aligns.reserve(tbl->col_count);
}

void BeginTableCell(ParseContext* ctx, const MD_BLOCK_TD_DETAIL* detail)
{
    // セルの中身は行内要素だけなので、表の中では current_node は表ノードのまま。md4c は TH/TD を TR の中にしか置かない。
    auto* const cn = ctx->current_node;
    assert(cn && cn->type == NodeType::Table);
    auto* const tbl = cn->table_data();
    assert(tbl->row_count > 0);
    const size_t col = tbl->cell_text_starts.size() - static_cast<size_t>(tbl->row_count - 1) * tbl->col_count;
    assert(col < tbl->col_count);
    const bool first_row = (tbl->row_count == 1);
    // 区切り: 行内 2 セル目以降は '\t'、行頭かつ 2 行目以降は '\n'。
    if (col > 0) {
        tbl->concat_text.push_back('\t');
    }
    else if (!first_row) {
        tbl->concat_text.push_back('\n');
    }
    tbl->cell_text_starts.push_back(static_cast<uint32_t>(tbl->concat_text.size()));
    if (!tbl->cell_run_starts.empty()) {
        tbl->cell_run_starts.push_back(static_cast<uint32_t>(tbl->all_runs.size()));
    }
    ctx->active_text_buffer = &tbl->concat_text;

    // 列単位の align は header 行 (1 行目) で決まる。col_count は BeginTable で確定済み。
    if (first_row) {
        tbl->aligns.push_back(detail ? static_cast<TableAlign>(detail->align) : TableAlign::Default);
    }
}

// 番兵を積む。セルは md4c が全部出しているので、埋めるものは無い。
void FinalizeTableOffsets(NodeTableData& tbl)
{
    assert(tbl.cell_text_starts.size() == static_cast<size_t>(tbl.row_count) * tbl.col_count);
    tbl.cell_text_starts.push_back(static_cast<uint32_t>(tbl.concat_text.size()));
    if (!tbl.cell_run_starts.empty()) {
        tbl.cell_run_starts.push_back(static_cast<uint32_t>(tbl.all_runs.size()));
    }
}

// md4c は参照の中身をテキストで渡さないので、GitHub と同じく脚注番号を定義へのリンクとして補う。
void AppendFootnoteRef(ParseContext* ctx, const MD_SPAN_FOOTNOTE_REF_DETAIL* ref)
{
    if (!ctx->current_node) {
        return;
    }
    if (ref->id > ctx->footnote_ref_counts.size()) {
        ctx->footnote_ref_counts.resize(ref->id, 0);
    }
    const uint32_t ref_id = ++ctx->footnote_ref_counts[ref->id - 1];
    ctx->indices.footnote_refs.push_back({ ref->id, ref_id, static_cast<int32_t>(ctx->CurrentNodeIndex()) });
    // リンク文字列内の参照でも外側のリンクが続くよう戻す。
    const int16_t outer_link = ctx->current_link_url_index;
    ctx->ResolveLinkUrlIndex(FootnoteDefUrl(ref->id));
    ctx->AppendDoc(std::format("[{}]", ref->id));
    ctx->FlushPendingRun();
    ctx->current_link_url_index = outer_link;
}

// 脚注定義は文書末尾で番号付きリストの項目にする。md4c は中身を MD_BLOCK_P 無しで、脚注番号順に渡す。
void BeginFootnoteDef(ParseContext* ctx, const MD_BLOCK_FOOTNOTE_DEF_DETAIL* def)
{
    ctx->BeginNode(NodeType::ListItem);
    ctx->current_node->set_list_number(static_cast<int32_t>(def->id));
    ctx->indices.footnote_def_indices.push_back(static_cast<int32_t>(ctx->CurrentNodeIndex()));
}

// GitHub と同じく、定義の末尾に参照へ戻るリンク "↩" (2 回目以降は "↩2" ...) を参照の数だけ付ける。
// 後の定義の中の参照は前の定義を閉じた後に届くので、パースを終えてからまとめて付ける。
void AppendFootnoteBackrefs(std::pmr::vector<Node>& nodes, TextArena& arena, std::span<const int32_t> def_indices, std::span<const uint32_t> ref_counts)
{
    std::pmr::string text;
    std::pmr::vector<TextRun> runs;
    std::pmr::vector<std::string_view> urls;
    for (size_t i = 0; i < def_indices.size() && i < ref_counts.size(); ++i) {
        const auto id = static_cast<uint32_t>(i + 1);
        auto& node = nodes[static_cast<size_t>(def_indices[i])];
        text = node.GetText();
        runs.assign_range(node.runs());
        urls.assign_range(node.view_link_urls());
        for (uint32_t ref_id = 1; ref_id <= ref_counts[i] && urls.size() < kMaxLinkUrls; ++ref_id) {
            if (!text.empty()) {
                text += ' ';
            }
            urls.push_back(arena.CopyText(FootnoteRefUrl(id, ref_id)));
            TextRun link;
            link.start = static_cast<uint32_t>(text.size());
            link.link_url_index = static_cast<int16_t>(urls.size() - 1);
            text += "↩";
            if (ref_id > 1) {
                std::format_to(std::back_inserter(text), "{}", ref_id);
            }
            link.length = static_cast<uint32_t>(text.size()) - link.start;
            runs.push_back(link);
        }
        node.SetText(arena.CopyText(text));
        node.SetRuns(arena.Copy(runs));
        node.SetLinkUrls(arena.Copy(urls));
    }
}

void LeaveCodeBlock(ParseContext* ctx)
{
    auto* const cn = ctx->current_node;
    assert(cn && cn->type == NodeType::CodeBlock);
    // md4c はコード各行を改行付きで渡すため、最終行の改行を表示テキストから落とす。
    // コード内には装飾もリンクも付かないので run は無く、調整は要らない。
    // 区切りを待って取り除いていた改行は戻してから落とす。
    ctx->CancelPendingSplit();
    ctx->FlushPendingRun();
    assert(ctx->current_runs.empty());
    if (ctx->current_text.ends_with('\n')) {
        ctx->current_text.pop_back();
        cn->line_count--;
    }
    if (IsDiagramLanguage(cn->code_language)) {
        cn->type = NodeType::Diagram;
        ctx->indices.diagram_indices.emplace_back(ctx->CurrentNodeIndex());
    }
}

int OnEnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata)
{
    auto* const ctx = static_cast<ParseContext*>(userdata);
    if (ctx->ShouldCancel()) {
        return 1;
    }

    switch (type) {
    case MD_BLOCK_DOC:
        break;

    case MD_BLOCK_H: {
        auto* const h = static_cast<MD_BLOCK_H_DETAIL*>(detail);
        ctx->BeginNode(NodeType::Heading);
        ctx->current_node->heading_level = static_cast<int8_t>(h->level);
        break;
    }

    case MD_BLOCK_P:
        ctx->BeginParagraphNode();
        ctx->paragraph_display_math_count = 0;
        ctx->paragraph_has_other_content = false;
        break;

    case MD_BLOCK_CODE: {
        ctx->BeginNode(NodeType::CodeBlock);
        auto* const code_detail = static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
        if (code_detail && code_detail->lang.text && code_detail->lang.size > 0) {
            ctx->current_node->code_language = DetectLanguage(ctx->AttributeText(code_detail->lang));
        }
        break;
    }

    case MD_BLOCK_QUOTE:
        if (ctx->blockquote_depth == 0) {
            ++ctx->blockquote_group_counter;
            ctx->outermost_quote_indent = ctx->indent_level + 1;
        }
        ctx->blockquote_depth++;
        ctx->indent_level++;
        break;

    case MD_BLOCK_UL:
        ctx->list_counter.push_back(-1); // unordered の番兵 (ordered の start は 0 以上)
        ctx->indent_level++;
        break;

    case MD_BLOCK_OL: {
        auto* const ol = static_cast<MD_BLOCK_OL_DETAIL*>(detail);
        ctx->list_counter.push_back(static_cast<int>(ol->start));
        ctx->indent_level++;
        break;
    }

    case MD_BLOCK_LI:
        BeginListItem(ctx, static_cast<const MD_BLOCK_LI_DETAIL*>(detail));
        break;

    case MD_BLOCK_HR:
        ctx->BeginNode(NodeType::HorizontalRule);
        break;

    case MD_BLOCK_TABLE:
        BeginTable(ctx, static_cast<const MD_BLOCK_TABLE_DETAIL*>(detail));
        break;

    case MD_BLOCK_THEAD:
    case MD_BLOCK_TBODY:
        break;

    case MD_BLOCK_TR:
        assert(ctx->current_node && ctx->current_node->type == NodeType::Table);
        ++ctx->current_node->table_data()->row_count;
        break;

    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        BeginTableCell(ctx, static_cast<const MD_BLOCK_TD_DETAIL*>(detail));
        break;

    case MD_BLOCK_FOOTNOTE_DEF_SECTION:
        ctx->BeginNode(NodeType::HorizontalRule);
        ctx->EndNode();
        ctx->indent_level++;
        break;

    case MD_BLOCK_FOOTNOTE_DEF:
        BeginFootnoteDef(ctx, static_cast<const MD_BLOCK_FOOTNOTE_DEF_DETAIL*>(detail));
        break;

    case MD_BLOCK_HTML:
        break;

    default:
        std::unreachable();
    }

    return 0;
}

// md4c は enter/leave が常にバランスするので、enter で積んだ深さやスタックは無条件に戻す。
// キャンセル後は enter も leave も何もしないので、途中で止めても釣り合いは崩れない。
int OnLeaveBlock(MD_BLOCKTYPE type, void* /*detail*/, void* userdata)
{
    auto* const ctx = static_cast<ParseContext*>(userdata);
    if (ctx->ShouldCancel()) {
        return 1;
    }

    ctx->FlushPendingRun();

    switch (type) {
    case MD_BLOCK_CODE:
        LeaveCodeBlock(ctx);
        // 他のリーフブロックと同様に leave で閉じる。残すと tight list でフェンス直後に
        // MD_BLOCK_P 無しで届く本文がコードに混入する。
        ctx->EndNode();
        break;

    case MD_BLOCK_QUOTE:
        --ctx->blockquote_depth;
        --ctx->indent_level;
        break;

    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
        ctx->list_counter.pop_back();
        --ctx->indent_level;
        break;

    case MD_BLOCK_TABLE: {
        auto* const cn = ctx->current_node;
        assert(cn && cn->has_table());
        FinalizeTableOffsets(*cn->table_data());
        // 以後 concat_text は伸縮しないので、本文として直接指す。
        cn->SetText(cn->table_data()->concat_text);
        ctx->EndNode();
        break;
    }

    case MD_BLOCK_THEAD:
    case MD_BLOCK_TBODY:
    case MD_BLOCK_TR:
        break;

    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        // セル退出後は table ノード自体への AppendDoc は想定されないため nullptr に倒す。
        // 次の TR/TD/TH 進入で再設定される。
        ctx->active_text_buffer = nullptr;
        break;

    case MD_BLOCK_H:
        // 見出しの中身は行内要素だけで、見出しは区切らないので current_node は見出しのまま。
        assert(ctx->current_node && ctx->current_node->type == NodeType::Heading);
        ctx->indices.toc_entries.emplace_back(static_cast<int>(ctx->CurrentNodeIndex()));
        ctx->EndNode();
        break;

    case MD_BLOCK_P:
        if (!TryPromoteParagraphToImage(ctx)) {
            TryPromoteParagraphToDisplayMath(ctx);
        }
        ctx->EndNode();
        break;

    case MD_BLOCK_LI:
    case MD_BLOCK_HR:
    case MD_BLOCK_FOOTNOTE_DEF:
        ctx->EndNode();
        break;

    case MD_BLOCK_FOOTNOTE_DEF_SECTION:
        --ctx->indent_level;
        break;

    case MD_BLOCK_DOC:
    case MD_BLOCK_HTML:
        break;

    default:
        std::unreachable();
    }

    return 0;
}

struct AlertInfo {
    ascii_util::DocLowercaseLiteral name; // [!TYPE] マーカー名 (GitHub 仕様で ASCII 固定)
    AlertType type;
    std::string_view label;
    std::string_view icon;
};

// AlertColorIndex 順。
inline constexpr AlertInfo kAlerts[]{
    { "note",      AlertType::Note,      "Note",      "ℹ"                },
    { "tip",       AlertType::Tip,       "Tip",       "\xF0\x9F\x92\xA1" }, // U+1F4A1 (BMP 外)
    { "important", AlertType::Important, "Important", "❗"               },
    { "warning",   AlertType::Warning,   "Warning",   "⚠"                },
    { "caution",   AlertType::Caution,   "Caution",   "⛔"               },
};
static_assert(std::size(kAlerts) == ALERT_TYPE_COUNT);
static_assert(std::ranges::all_of(std::views::iota(size_t{ 0 }, ALERT_TYPE_COUNT), [](size_t i) static {
    return AlertColorIndex(kAlerts[i].type) == i;
}));

// "[!" + 最長の名前 + "]"。マーカー判定はソフト改行ごとに走るので、']' を段落全体から探さない。
constexpr size_t kMaxMarkerLength = 3 + std::ranges::max(kAlerts | std::views::transform([](const AlertInfo& e) static { return e.name.value.size(); }));

struct AlertMarker {
    const AlertInfo* info = nullptr; // マーカーでなければ null
    size_t end = 0;                  // マーカー直後の改行を含む終端位置
};

// テキスト先頭の [!TYPE] パターンを検出する。
// Alert マーカーは GitHub 仕様で ASCII 固定なので大小無視 ASCII 比較でよい。
AlertMarker DetectAlertMarker(std::string_view text)
{
    if (text.size() < 3 || text[0] != '[' || text[1] != '!') {
        return {};
    }
    const auto close = text.substr(0, kMaxMarkerLength).find(']');
    if (close == std::string_view::npos || close <= 2) {
        return {};
    }

    const auto type_str = text.substr(2, close - 2);
    const auto it = std::ranges::find_if(kAlerts, [type_str](const AlertInfo& e) noexcept {
        return ascii_util::iequal(type_str, e.name);
    });
    if (it == std::end(kAlerts)) {
        return {};
    }

    // GitHub に合わせ、マーカーと本文や画像が同じ行にある "[!NOTE] 本文" は Alert にしない
    // (fe1ae552 で同じ行の画像を Alert にし、db6320d7 で戻した)。表示を GitHub と食い違わせない。
    // 行末の空白は md4c が落とすので、マーカー直後は改行か終端だけを見ればよい。
    const size_t end = close + 1;
    if (end == text.size()) {
        return { &*it, end };
    }
    if (text[end] == '\n') {
        return { &*it, end + 1 };
    }
    return {};
}

// マーカーを除去しアイコン+ラベルを挿入する。TextRunも調整する。
// テキスト構造: "[icon] Label" (コンテンツなし) または "[icon] Label\n[content]" (コンテンツあり)
void TransformAlertNode(Node& node, TextArena& arena, const AlertInfo& info, size_t marker_end)
{
    const std::string_view label = info.label;
    const std::string_view icon = info.icon;
    const std::string_view current_text = node.GetText();
    const bool has_content = (marker_end < current_text.size());

    const size_t full_label_len = icon.size() + 1 + label.size(); // "icon Label"
    std::string new_text;
    new_text.reserve(full_label_len + 4 + (has_content ? current_text.size() - marker_end : 0));
    new_text.append(icon);
    new_text += ' ';
    new_text.append(label);

    size_t new_content_start = full_label_len;
    if (has_content) {
        new_text += '\n';
        new_content_start = full_label_len + 1;
        new_text.append(current_text.substr(marker_end));
    }

    std::pmr::vector<TextRun> new_runs;
    new_runs.reserve(node.runs().size() + 1);
    // アイコン絵文字は太字にしない (スペース + ラベルテキストのみ太字)。
    TextRun label_run;
    label_run.start = static_cast<uint32_t>(icon.size());
    label_run.length = static_cast<uint32_t>(full_label_len - icon.size());
    label_run.set_raw_flags(TextRun::kBold);
    new_runs.emplace_back(label_run);

    // マーカー部分を除外し、残りを新しい本文開始位置へ平行移動する。
    const auto marker = static_cast<uint32_t>(marker_end);
    const auto content_start = static_cast<uint32_t>(new_content_start);
    for (TextRun adjusted : node.runs()) {
        const uint32_t run_end = adjusted.start + adjusted.length;
        if (run_end <= marker) {
            continue;
        }
        if (adjusted.start < marker) {
            adjusted.start = marker;
            adjusted.length = run_end - marker;
        }
        adjusted.start = content_start + (adjusted.start - marker);
        new_runs.emplace_back(adjusted);
    }

    // 全文走査で改行を数え直すと O(text) かかるため、ここで差分計算する。
    // マーカー [!TYPE] 本体には改行が入らず、DetectAlertMarker が直後の改行を含めたときだけ
    // 改行 1 個。marker_end 直前の 1 文字だけを見ればよい。
    const int32_t marker_newlines = (marker_end > 0 && current_text[marker_end - 1] == '\n');
    const int32_t new_line_count = node.line_count - marker_newlines + has_content;
    node.SetText(arena.CopyText(new_text), new_line_count);
    node.SetRuns(arena.Copy(new_runs));
    node.alert_type = info.type;
    node.set_alert_label_length(static_cast<uint32_t>(full_label_len));
}

// nodes[i] が Alert マーカーを持ちうる位置 (最外側 blockquote の先頭段落) か。
// GitHub 仕様: Alert は最外側 blockquote (quote_depth==1) でのみ認識し、ネスト内 (`> > [!NOTE]`) は通常の引用。
// マーカーは blockquote の先頭でのみ有効で、"> intro" の後に続く段落の [!NOTE] は通常の引用。
bool IsAlertHeadCandidate(const std::pmr::vector<Node>& nodes, size_t i) noexcept
{
    const auto& node = nodes[i];
    return node.type == NodeType::BlockQuote && node.quote_depth == 1 &&
           (i == 0 || nodes[i - 1].blockquote_group != node.blockquote_group);
}

// 単一の BlockQuote ノードに対して Alert マーカーを検出・適用し、
// 同一 blockquote_group の後続ノードに alert_type を伝播する。
// 伝播先は先頭候補にならないので、先頭候補が既に alert_type を持つことはない。
void DetectAlertAt(std::pmr::vector<Node>& nodes, TextArena& arena, size_t i)
{
    if (!IsAlertHeadCandidate(nodes, i)) {
        return;
    }
    auto& node = nodes[i];
    const auto [info, marker_end] = DetectAlertMarker(node.GetText());
    if (!info) {
        return;
    }
    const int group = node.blockquote_group;
    TransformAlertNode(node, arena, *info, marker_end);

    // ノード種別に依存せず、グループIDで判定する（リスト等も含む）。
    for (size_t j = i + 1; j < nodes.size(); j++) {
        if (nodes[j].blockquote_group != group) {
            break;
        }
        nodes[j].alert_type = info->type;
    }
}

// 今の段落が Alert の先頭候補で、テキストが Alert マーカー ([!TYPE] と直後の改行) だけからなるか。
bool IsAlertMarkerOnlyHead(const ParseContext* ctx)
{
    if (!ctx->current_node || !IsAlertHeadCandidate(ctx->nodes, ctx->CurrentNodeIndex())) {
        return false;
    }
    const auto [info, end] = DetectAlertMarker(ctx->current_text);
    return info && end == ctx->current_text.size();
}

// "> [!NOTE]\n> ![img](p.png)" は md4c では 1 段落になり、画像段落として丸ごと Image に昇格すると
// Alert 判定の対象 (引用ノード) が消える。マーカーだけのノードをここで閉じ、画像は同じ引用グループの
// 次の段落で受けて、マーカーと画像が別段落の場合と同じ構造にする (Alert は後続ノードへ伝播する)。
void SplitAlertMarkerBeforeImage(ParseContext* ctx)
{
    if (!IsAlertMarkerOnlyHead(ctx)) {
        return;
    }
    // [![badge](b.svg)](url) では MD_SPAN_A が先に来て URL を閉じるノードへ登録済みなので、新ノードへ登録し直す。
    const std::string_view link_url = ctx->OpenLinkUrl();
    ctx->EndNode();
    ctx->BeginParagraphNode();
    ctx->ResolveLinkUrlIndex(link_url);
}

int OnEnterSpan(MD_SPANTYPE type, void* detail, void* userdata)
{
    auto* const ctx = static_cast<ParseContext*>(userdata);
    if (ctx->ShouldCancel()) {
        return 1;
    }

    ctx->EnsureNodeForListItemText();
    ctx->FlushPendingRun();
    // span が自分で足す文字 ("$" や脚注番号) は、待っている区切りの後ろに入れる。
    if (type == MD_SPAN_LATEXMATH || type == MD_SPAN_LATEXMATH_DISPLAY || type == MD_SPAN_FOOTNOTE_REF) {
        ctx->ApplyPendingSplit();
    }
    // src が空の画像は Image ノードに昇格しないので、分割せず Alert 本文のままにする。
    if (type == MD_SPAN_IMG && ctx->image_span_depth == 0 && static_cast<const MD_SPAN_IMG_DETAIL*>(detail)->src.size > 0) {
        SplitAlertMarkerBeforeImage(ctx);
    }
    // span markup (** _ ` [] 等) は原文にあるが current_text には入らないため、
    // どの span でも原文と一致しなくなる。FinalizeCurrentNode の memcmp をスキップさせる。
    ctx->current_node_needs_copy = true;

    // display math は個数 (paragraph_display_math_count) で昇格を判定するので、ここで立てるのは他の span 全種類。
    if (type != MD_SPAN_LATEXMATH_DISPLAY) {
        ctx->paragraph_has_other_content = true;
    }

    if (const uint8_t flag = RunFlagOf(type); flag != 0) {
        ++ctx->span_depths[std::countr_zero(flag)];
        ctx->current_run_flags |= flag;
    }
    switch (type) {
    case MD_SPAN_A: {
        auto* const a = static_cast<MD_SPAN_A_DETAIL*>(detail);
        ctx->ResolveLinkUrlIndex(ctx->AttributeText(a->href));
        break;
    }
    case MD_SPAN_IMG:
        ++ctx->image_span_depth;
        break;
    case MD_SPAN_LATEXMATH_DISPLAY:
        // "$$" は昇格しなかったときのフォールバックテキスト用。
        ctx->AppendDoc("$$");
        break;
    case MD_SPAN_LATEXMATH:
        // インライン $...$ は昇格対象外。元の "$" を復元してテキストとして残す
        ctx->AppendDoc("$");
        break;
    case MD_SPAN_FOOTNOTE_REF:
        AppendFootnoteRef(ctx, static_cast<const MD_SPAN_FOOTNOTE_REF_DETAIL*>(detail));
        break;
    case MD_SPAN_STRONG:
    case MD_SPAN_EM:
    case MD_SPAN_CODE:
    case MD_SPAN_DEL:
    case MD_SPAN_WIKILINK:
    case MD_SPAN_U:
        break;
    default:
        std::unreachable();
    }

    return 0;
}

// md4c は leave でも enter と同じ src を渡すので、最外側の leave で受け取る。
void LeaveImageSpan(ParseContext* ctx, const MD_SPAN_IMG_DETAIL* img)
{
    if (--ctx->image_span_depth != 0 || img->src.size == 0) {
        return;
    }
    if (auto* const cn = ctx->current_node; cn && IsImagePromotionCandidate(*cn)) {
        ctx->image_src = ctx->InternUrl(ctx->AttributeText(img->src));
    }
}

int OnLeaveSpan(MD_SPANTYPE type, void* detail, void* userdata)
{
    auto* const ctx = static_cast<ParseContext*>(userdata);
    if (ctx->ShouldCancel()) {
        return 1;
    }

    ctx->FlushPendingRun();

    // md4c は enter/leave が常にバランスする契約なのでアンダーフローは起きない。
    if (const uint8_t flag = RunFlagOf(type); flag != 0 && --ctx->span_depths[std::countr_zero(flag)] == 0) {
        ctx->current_run_flags &= static_cast<uint8_t>(~flag);
    }
    switch (type) {
    case MD_SPAN_A:
        ctx->current_link_url_index = -1;
        break;
    case MD_SPAN_IMG:
        LeaveImageSpan(ctx, static_cast<const MD_SPAN_IMG_DETAIL*>(detail));
        break;
    case MD_SPAN_LATEXMATH_DISPLAY:
        ctx->AppendDoc("$$");
        ctx->paragraph_display_math_count++;
        break;
    case MD_SPAN_LATEXMATH:
        ctx->AppendDoc("$");
        break;
    case MD_SPAN_STRONG:
    case MD_SPAN_EM:
    case MD_SPAN_CODE:
    case MD_SPAN_DEL:
    case MD_SPAN_FOOTNOTE_REF:
    case MD_SPAN_WIKILINK:
    case MD_SPAN_U:
        break;
    default:
        std::unreachable();
    }

    return 0;
}

int OnText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata)
{
    auto* const ctx = static_cast<ParseContext*>(userdata);
    if (ctx->ShouldCancel()) {
        return 1;
    }

    if (!ctx->current_node) [[unlikely]] {
        if (type == MD_TEXT_HTML || type == MD_TEXT_NULLCHAR || !ctx->EnsureNodeForListItemText()) {
            return 0;
        }
    }

    // 数式の中身・HTML・NUL 以外のテキストがあれば、$$..$$ だけの段落ではない。
    ctx->paragraph_has_other_content |= type != MD_TEXT_LATEXMATH && type != MD_TEXT_HTML && type != MD_TEXT_NULLCHAR;

    const std::string_view chunk{ text, static_cast<size_t>(size) };

    if (!ctx->current_node->HasSourceOffset()) [[unlikely]] {
        ctx->RecordSourceOffset(chunk);
    }

    switch (type) {
    case MD_TEXT_NORMAL:
    case MD_TEXT_CODE:
        // コードの行末は単独の "\n" で届く。
        if (type == MD_TEXT_CODE && chunk == "\n") {
            ctx->AppendLineBreak(chunk);
        }
        else {
            ctx->AppendSplittable(chunk);
        }
        break;

    case MD_TEXT_LATEXMATH:
        ctx->AppendSplittable(chunk);
        break;

    case MD_TEXT_ENTITY: {
        // entity (`&amp;` 等) は現状文字に解決される。原文 (`&amp;`) と current_text (`&`) が
        // 不一致になり arena へのコピーが確定。memcmp スキップフラグを立てる。
        ctx->current_node_needs_copy = true;
        char entity_buf[4];
        ctx->AppendSplittable(ResolveHtmlEntity(chunk, entity_buf).value_or(chunk));
        break;
    }

    case MD_TEXT_BR:
        // BR / SOFTBR は原文の `<br>` や 2 個の半角空白+改行を `\n`/` ` に置換するため raw_slice 不一致。
        ctx->current_node_needs_copy = true;
        ctx->AppendLineBreak("\n");
        break;

    case MD_TEXT_SOFTBR:
        ctx->current_node_needs_copy = true;
        // Alert マーカーだけの先頭行の後ろは改行のまま残す。空白にすると、GitHub が Alert にしない
        // 同じ行の "[!NOTE] 本文" と区別できない。
        ctx->AppendLineBreak(IsAlertMarkerOnlyHead(ctx) ? "\n" : " ");
        break;

    case MD_TEXT_NULLCHAR:
    case MD_TEXT_HTML:
        break;

    default:
        std::unreachable();
    }

    return 0;
}

// 各種予約サイズのヒント定数。値は「入力 N byte あたり 1 個」を表す:
//   - kScratchInputBytesPerByte=8 → ノード当たりの平均テキスト長 ~8B 想定の scratch。
//   - kInputBytesPerNode=128      → 256 と 2 往復した (8b22f224, cc98b79b で 256 にし、8eee1cc6, 29e67089 で戻した)。
//                                   256 に戻さないこと。実測 246〜283 byte/ノードの文書が予約内に収まる。256 だと 246 byte/ノードの
//                                   100MB 文書で終盤に伸長 (全ノードのコピー) が起き、md_parse が約 3% 遅れた。
//                                   余りはパース後に詰めるので、増えるのはパース中のコミットだけ。
//                                   リンク集 (~47 byte/ノード) のように細かい文書は GrowNodes が 1 回で広げる。
// arena はチャンクを積み足すので予約しない。見出し・画像などのインデックス配列は件数が少なく、通常の伸長で足りる。
constexpr size_t kScratchInputBytesPerByte = 8;
constexpr size_t kInputBytesPerNode = 128;
constexpr size_t kScratchReserveMin = 1024;
constexpr size_t kScratchReserveMax = 64 * 1024;

void ReserveForInput(ParseContext& ctx, size_t input_size)
{
    ctx.current_text.reserve(std::clamp(input_size / kScratchInputBytesPerByte, kScratchReserveMin, kScratchReserveMax));
    const size_t nodes_reserve = std::max(input_size / kInputBytesPerNode, 64uz);
    ctx.nodes.reserve(nodes_reserve);
    ctx.list_counter.reserve(8);
    MENDO_STATF("nodes.reserve: input={} reserve={}", input_size, nodes_reserve);
}

} // namespace

NodeStorage ParseMarkdown(std::string_view markdown_text, std::stop_token stop_token, TaskScheduler* scheduler)
{
    MENDO_PROFILE("ParseMarkdown");
    const size_t input_size = markdown_text.size();
    ParseContext ctx;
    ctx.stop_token = std::move(stop_token);
    ctx.markdown = markdown_text;
    ReserveForInput(ctx, input_size);

    MD_PARSER parser{};
    parser.abi_version = 0;
    // Alert は引用ノードから自前で検出する。md4c の admonitions はネストした引用でも認識する等、GitHub と挙動が違うので外す。
    parser.flags = (MD_DIALECT_GITHUB & ~MD_FLAG_ADMONITIONS) | MD_FLAG_LATEXMATHSPANS;
    parser.enter_block = OnEnterBlock;
    parser.leave_block = OnLeaveBlock;
    parser.enter_span = OnEnterSpan;
    parser.leave_span = OnLeaveSpan;
    parser.text = OnText;

    {
        MENDO_PROFILE("md_parse");
        md_parse(markdown_text.data(), static_cast<MD_SIZE>(markdown_text.size()), &parser, &ctx);
        // 最後の current_node が残っていれば（典型的には全 OnLeaveBlock で処理済みだが
        // 安全のため）テキストを確定する。
        ctx.FinalizeCurrentNode();
    }

    // キャンセル時は中途半端な nodes の後処理 (脚注の戻りリンク・Alert 検出・詰め) を飛ばし、
    // 呼び出し側がチェックしやすいよう空の NodeStorage を返す。
    if (ctx.cancel_requested) {
        return {};
    }

    AppendFootnoteBackrefs(ctx.nodes, ctx.arena, ctx.indices.footnote_def_indices, ctx.footnote_ref_counts);

    for (const size_t first : ctx.split_code_runs) {
        const size_t last = ChunkRunLast(ctx.nodes, first);
        AnnotateCodeChunkLeads(std::span{ ctx.nodes }.subspan(first, last - first + 1), scheduler);
    }

    {
        // BlockQuote ノードから GitHub Alerts を検出し、マーカー除去・ラベル挿入・グルーピングを行う。
        MENDO_PROFILE("DetectAlerts");
        for (const size_t i : ctx.blockquote_indices) {
            DetectAlertAt(ctx.nodes, ctx.arena, i);
        }
    }

    // 予約や見積もりの余りを文書の寿命の間持ち続けないよう、大きく余ったときだけ詰める。
    // 詰める処理は全体のメモリ (commit) の 10% 以上効くものだけ置く。これは 100MB の文書で
    // commit を約 30MB (詰めないときの 266MB の約 12%) 減らし、約 3.6ms かかる。物理メモリは変わらない。
    // 表・構文トークン・ヒットテストのバッファ・表の行の背景は 0.5MB 未満しか効かないので詰めない (e48be6cc)。
    if (ctx.nodes.capacity() > ctx.nodes.size() + ctx.nodes.size() / 4) {
        ctx.nodes.shrink_to_fit();
    }

    return NodeStorage{ std::move(ctx.nodes), std::move(ctx.arena), std::move(ctx.tables), std::move(ctx.indices) };
}

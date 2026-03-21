#include <gtest/gtest.h>
#include "parser.h"

// ---- 基本的なパース ----

TEST(Parser, EmptyInputReturnsNoNodes) {
    auto nodes = ParseMarkdown("");
    EXPECT_TRUE(nodes.empty());
}

TEST(Parser, WhitespaceOnlyReturnsNoNodes) {
    auto nodes = ParseMarkdown("   \n\n  ");
    EXPECT_TRUE(nodes.empty());
}

TEST(Parser, SingleParagraph) {
    auto nodes = ParseMarkdown("Hello world");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_EQ(nodes[0].text, L"Hello world");
}

TEST(Parser, MultipleParagraphs) {
    auto nodes = ParseMarkdown("First\n\nSecond\n\nThird");
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(nodes[0].type, NodeType::Paragraph);
    EXPECT_EQ(nodes[1].type, NodeType::Paragraph);
    EXPECT_EQ(nodes[2].type, NodeType::Paragraph);
    EXPECT_EQ(nodes[0].text, L"First");
    EXPECT_EQ(nodes[1].text, L"Second");
    EXPECT_EQ(nodes[2].text, L"Third");
}

// ---- 見出し ----

TEST(Parser, HeadingH1) {
    auto nodes = ParseMarkdown("# Title");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Heading);
    EXPECT_EQ(nodes[0].heading_level, 1);
    EXPECT_EQ(nodes[0].text, L"Title");
}

TEST(Parser, HeadingH2) {
    auto nodes = ParseMarkdown("## Subtitle");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].heading_level, 2);
}

TEST(Parser, HeadingH3ToH6) {
    for (int level = 3; level <= 6; level++) {
        std::string md(level, '#');
        md += " Test";
        auto nodes = ParseMarkdown(md);
        ASSERT_EQ(nodes.size(), 1u) << "level=" << level;
        EXPECT_EQ(nodes[0].heading_level, level) << "level=" << level;
    }
}

TEST(Parser, HeadingAnchorId) {
    auto nodes = ParseMarkdown("# Hello World");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].anchor_id, L"hello-world");
}

TEST(Parser, HeadingAnchorIdCjk) {
    auto nodes = ParseMarkdown("## コードブロック");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].anchor_id, L"コードブロック");
}

// ---- インライン書式 ----

TEST(Parser, BoldText) {
    auto nodes = ParseMarkdown("**bold**");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].text, L"bold");
    ASSERT_GE(nodes[0].runs.size(), 1u);
    EXPECT_TRUE(nodes[0].runs[0].bold);
    EXPECT_FALSE(nodes[0].runs[0].italic);
}

TEST(Parser, ItalicText) {
    auto nodes = ParseMarkdown("*italic*");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].text, L"italic");
    ASSERT_GE(nodes[0].runs.size(), 1u);
    EXPECT_TRUE(nodes[0].runs[0].italic);
    EXPECT_FALSE(nodes[0].runs[0].bold);
}

TEST(Parser, BoldItalicText) {
    auto nodes = ParseMarkdown("***bolditalic***");
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_GE(nodes[0].runs.size(), 1u);
    EXPECT_TRUE(nodes[0].runs[0].bold);
    EXPECT_TRUE(nodes[0].runs[0].italic);
}

TEST(Parser, InlineCode) {
    auto nodes = ParseMarkdown("`code`");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].text, L"code");
    ASSERT_GE(nodes[0].runs.size(), 1u);
    EXPECT_TRUE(nodes[0].runs[0].code);
}

TEST(Parser, StrikethroughText) {
    auto nodes = ParseMarkdown("~~deleted~~");
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_GE(nodes[0].runs.size(), 1u);
    EXPECT_TRUE(nodes[0].runs[0].strikethrough);
}

TEST(Parser, MixedFormattingPreservesOrder) {
    auto nodes = ParseMarkdown("normal **bold** normal");
    ASSERT_EQ(nodes.size(), 1u);
    // 少なくとも3つのランを持つべき: "normal ", "bold", " normal"
    ASSERT_GE(nodes[0].runs.size(), 3u);
    EXPECT_FALSE(nodes[0].runs[0].bold);
    EXPECT_TRUE(nodes[0].runs[1].bold);
    EXPECT_FALSE(nodes[0].runs[2].bold);
}

// ---- リンク ----

TEST(Parser, ExternalLink) {
    auto nodes = ParseMarkdown("[text](https://example.com)");
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_GE(nodes[0].runs.size(), 1u);
    ASSERT_TRUE(nodes[0].runs[0].link_url.has_value());
    EXPECT_EQ(nodes[0].runs[0].link_url.value(), L"https://example.com");
    EXPECT_EQ(nodes[0].text, L"text");
}

TEST(Parser, InternalLink) {
    auto nodes = ParseMarkdown("[section](#my-section)");
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_GE(nodes[0].runs.size(), 1u);
    ASSERT_TRUE(nodes[0].runs[0].link_url.has_value());
    EXPECT_EQ(nodes[0].runs[0].link_url.value(), L"#my-section");
}

TEST(Parser, ParagraphWithNoLink) {
    auto nodes = ParseMarkdown("plain text");
    ASSERT_EQ(nodes.size(), 1u);
    for (const auto& run : nodes[0].runs) {
        EXPECT_FALSE(run.link_url.has_value());
    }
}

// ---- コードブロック ----

TEST(Parser, FencedCodeBlock) {
    auto nodes = ParseMarkdown("```\ncode line 1\ncode line 2\n```");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    EXPECT_NE(nodes[0].text.find(L"code line 1"), std::wstring::npos);
    EXPECT_NE(nodes[0].text.find(L"code line 2"), std::wstring::npos);
}

TEST(Parser, CodeBlockPreservesNewlines) {
    auto nodes = ParseMarkdown("```\na\nb\nc\n```");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    // 行間に改行を含むべき
    auto& text = nodes[0].text;
    int newlines = 0;
    for (wchar_t c : text) if (c == L'\n') newlines++;
    EXPECT_GE(newlines, 2);
}

// ---- 水平線 ----

TEST(Parser, HorizontalRule) {
    auto nodes = ParseMarkdown("---");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::HorizontalRule);
}

TEST(Parser, HorizontalRuleWithAsterisks) {
    auto nodes = ParseMarkdown("***");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::HorizontalRule);
}

// ---- リスト ----

TEST(Parser, UnorderedList) {
    auto nodes = ParseMarkdown("- item1\n- item2\n- item3");
    ASSERT_EQ(nodes.size(), 3u);
    for (const auto& node : nodes) {
        EXPECT_EQ(node.type, NodeType::ListItem);
        EXPECT_EQ(node.list_number, 0); // 順序なしリスト
    }
    EXPECT_EQ(nodes[0].text, L"item1");
    EXPECT_EQ(nodes[1].text, L"item2");
    EXPECT_EQ(nodes[2].text, L"item3");
}

// ---- バグ #10: ネストされた引用ブロック ----

TEST(Parser, NestedBlockquotePreservesOuterStyle) {
    auto nodes = ParseMarkdown("> outer\n>\n> > inner\n>\n> still outer");
    // 内側の引用ブロックが終了した後、"still outer"はまだBlockQuoteであるべき
    bool found_still_outer = false;
    for (const auto& node : nodes) {
        if (node.text.find(L"still outer") != std::wstring::npos) {
            EXPECT_EQ(node.type, NodeType::BlockQuote)
                << "内側の引用ブロック後のテキストはBlockQuoteのままであるべき";
            found_still_outer = true;
        }
    }
    EXPECT_TRUE(found_still_outer) << "ノード内に'still outer'が見つかるべき";
}

TEST(Parser, SingleBlockquoteIsBlockQuoteType) {
    auto nodes = ParseMarkdown("> quoted text");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::BlockQuote);
    EXPECT_EQ(nodes[0].text, L"quoted text");
}

// ---- バグ #11: Unicode追加面のエンティティ ----

TEST(Parser, HtmlEntitySupplementaryPlane) {
    // U+1F600 = ニコニコ顔の絵文字（追加面）
    auto nodes = ParseMarkdown("&#x1F600;");
    ASSERT_EQ(nodes.size(), 1u);
    // U+1F600のUTF-16サロゲートペア: D83D DE00
    ASSERT_GE(nodes[0].text.size(), 2u);
    EXPECT_EQ(nodes[0].text[0], static_cast<wchar_t>(0xD83D));
    EXPECT_EQ(nodes[0].text[1], static_cast<wchar_t>(0xDE00));
}

TEST(Parser, HtmlEntityDecimalSupplementaryPlane) {
    // U+1F4A9 = 128169 decimal
    auto nodes = ParseMarkdown("&#128169;");
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_GE(nodes[0].text.size(), 2u);
    // U+1F4A9 -> D83D DCA9
    EXPECT_EQ(nodes[0].text[0], static_cast<wchar_t>(0xD83D));
    EXPECT_EQ(nodes[0].text[1], static_cast<wchar_t>(0xDCA9));
}

TEST(Parser, HtmlEntityBmpStillWorks) {
    // U+00A9 = 著作権記号（基本多言語面）
    auto nodes = ParseMarkdown("&#xA9;");
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].text.size(), 1u);
    EXPECT_EQ(nodes[0].text[0], L'\u00A9');
}

TEST(Parser, HtmlEntityBeyondUnicode) {
    // U+110000はUnicodeの最大値を超えている; 無視されるべき
    auto nodes = ParseMarkdown("&#x110000;");
    ASSERT_EQ(nodes.size(), 1u);
    // 生のエンティティテキストとしてそのまま渡されるべき
    EXPECT_NE(nodes[0].text.find(L"110000"), std::wstring::npos);
}

// ---- リスト ----

TEST(Parser, OrderedList) {
    auto nodes = ParseMarkdown("1. first\n2. second\n3. third");
    ASSERT_EQ(nodes.size(), 3u);
    for (const auto& node : nodes) {
        EXPECT_EQ(node.type, NodeType::ListItem);
    }
    EXPECT_EQ(nodes[0].list_number, 1);
    EXPECT_EQ(nodes[1].list_number, 2);
    EXPECT_EQ(nodes[2].list_number, 3);
}

TEST(Parser, OrderedListStartsFromN) {
    auto nodes = ParseMarkdown("5. five\n6. six");
    ASSERT_EQ(nodes.size(), 2u);
    EXPECT_EQ(nodes[0].list_number, 5);
    EXPECT_EQ(nodes[1].list_number, 6);
}

TEST(Parser, ListIndentLevel) {
    auto nodes = ParseMarkdown("- item");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_GT(nodes[0].indent_level, 0);
}

// ---- タスクリスト ----

TEST(Parser, TaskListChecked) {
    auto nodes = ParseMarkdown("- [x] done");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::TaskListItem);
    EXPECT_TRUE(nodes[0].task_checked);
    EXPECT_EQ(nodes[0].text, L"done");
}

TEST(Parser, TaskListUnchecked) {
    auto nodes = ParseMarkdown("- [ ] todo");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::TaskListItem);
    EXPECT_FALSE(nodes[0].task_checked);
}

TEST(Parser, TaskListUpperX) {
    auto nodes = ParseMarkdown("- [X] also done");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_TRUE(nodes[0].task_checked);
}

// ---- 引用ブロック ----

TEST(Parser, BlockQuote) {
    auto nodes = ParseMarkdown("> quoted text");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::BlockQuote);
    EXPECT_EQ(nodes[0].text, L"quoted text");
}

TEST(Parser, BlockQuoteIndentLevel) {
    auto nodes = ParseMarkdown("> quoted");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_GT(nodes[0].indent_level, 0);
}

// ---- テーブル ----

TEST(Parser, SimpleTable) {
    auto nodes = ParseMarkdown(
        "| A | B |\n"
        "|---|---|\n"
        "| 1 | 2 |"
    );
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Table);
    ASSERT_GE(nodes[0].table_rows.size(), 2u); // header + 1 data row
}

TEST(Parser, TableHeaderCells) {
    auto nodes = ParseMarkdown(
        "| H1 | H2 |\n"
        "|---|---|\n"
        "| D1 | D2 |"
    );
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_GE(nodes[0].table_rows.size(), 2u);

    // 最初の行はヘッダーであるべき
    const auto& header = nodes[0].table_rows[0];
    ASSERT_EQ(header.cells.size(), 2u);
    EXPECT_TRUE(header.cells[0].is_header);
    EXPECT_TRUE(header.cells[1].is_header);
    EXPECT_EQ(header.cells[0].text, L"H1");
    EXPECT_EQ(header.cells[1].text, L"H2");

    // 2番目の行はヘッダーではないべき
    const auto& data = nodes[0].table_rows[1];
    EXPECT_FALSE(data.cells[0].is_header);
}

TEST(Parser, TableAlignment) {
    auto nodes = ParseMarkdown(
        "| L | C | R |\n"
        "|:--|:--:|--:|\n"
        "| a | b | c |"
    );
    ASSERT_EQ(nodes.size(), 1u);
    // データ行の配置を確認（配置はMD_BLOCK_TD_DETAILから取得）
    ASSERT_GE(nodes[0].table_rows.size(), 2u);
    const auto& row = nodes[0].table_rows[1];
    ASSERT_EQ(row.cells.size(), 3u);
    EXPECT_EQ(row.cells[0].align, 1); // MD_ALIGN_LEFT
    EXPECT_EQ(row.cells[1].align, 2); // MD_ALIGN_CENTER
    EXPECT_EQ(row.cells[2].align, 3); // MD_ALIGN_RIGHT
}

TEST(Parser, TableMultipleRows) {
    auto nodes = ParseMarkdown(
        "| A |\n"
        "|---|\n"
        "| 1 |\n"
        "| 2 |\n"
        "| 3 |"
    );
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].table_rows.size(), 4u); // 1 header + 3 data
}

// ---- HTMLエンティティ ----

TEST(Parser, HtmlEntityAmp) {
    auto nodes = ParseMarkdown("A &amp; B");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_NE(nodes[0].text.find(L"&"), std::wstring::npos);
}

TEST(Parser, HtmlEntityLtGt) {
    auto nodes = ParseMarkdown("&lt;tag&gt;");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_NE(nodes[0].text.find(L"<"), std::wstring::npos);
    EXPECT_NE(nodes[0].text.find(L">"), std::wstring::npos);
}

// ---- 複雑なドキュメント ----

TEST(Parser, ComplexDocumentNodeCount) {
    auto nodes = ParseMarkdown(
        "# Title\n\n"
        "Paragraph.\n\n"
        "## Section\n\n"
        "- item1\n"
        "- item2\n\n"
        "---\n\n"
        "> quote\n\n"
        "```\ncode\n```\n"
    );
    // タイトル、段落、セクション、item1、item2、水平線、引用、コード
    EXPECT_GE(nodes.size(), 7u);
}

TEST(Parser, NodeTypesInComplexDocument) {
    auto nodes = ParseMarkdown(
        "# H\n\nP\n\n- L\n\n---\n\n> Q\n\n```\nC\n```\n"
    );
    bool has_heading = false, has_para = false, has_list = false;
    bool has_hr = false, has_quote = false, has_code = false;
    for (const auto& n : nodes) {
        if (n.type == NodeType::Heading) has_heading = true;
        if (n.type == NodeType::Paragraph) has_para = true;
        if (n.type == NodeType::ListItem) has_list = true;
        if (n.type == NodeType::HorizontalRule) has_hr = true;
        if (n.type == NodeType::BlockQuote) has_quote = true;
        if (n.type == NodeType::CodeBlock) has_code = true;
    }
    EXPECT_TRUE(has_heading);
    EXPECT_TRUE(has_para);
    EXPECT_TRUE(has_list);
    EXPECT_TRUE(has_hr);
    EXPECT_TRUE(has_quote);
    EXPECT_TRUE(has_code);
}

// ---- UTF-8処理 ----

TEST(Parser, JapaneseText) {
    auto nodes = ParseMarkdown("日本語テスト");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].text, L"日本語テスト");
}

TEST(Parser, EmojiText) {
    auto nodes = ParseMarkdown("Hello 🎉");
    ASSERT_EQ(nodes.size(), 1u);
    // クラッシュせず出力が生成されることだけを確認
    EXPECT_FALSE(nodes[0].text.empty());
}

// ---- ソフトブレーク処理 ----

TEST(Parser, SoftBreakBecomesSpace) {
    auto nodes = ParseMarkdown("line1\nline2");
    ASSERT_EQ(nodes.size(), 1u);
    // 段落内のソフトブレークはスペースになるべき
    EXPECT_NE(nodes[0].text.find(L"line1"), std::wstring::npos);
    EXPECT_NE(nodes[0].text.find(L"line2"), std::wstring::npos);
}

// ---- ラン位置の整合性 ----

TEST(Parser, RunPositionsAreValid) {
    auto nodes = ParseMarkdown("normal **bold** `code` *italic*");
    ASSERT_EQ(nodes.size(), 1u);
    const auto& node = nodes[0];
    for (const auto& run : node.runs) {
        EXPECT_LE(run.start, node.text.size());
        EXPECT_LE(run.start + run.length, node.text.size());
    }
}

TEST(Parser, RunsCoverEntireText) {
    auto nodes = ParseMarkdown("aaa **bbb** ccc");
    ASSERT_EQ(nodes.size(), 1u);
    const auto& node = nodes[0];
    uint32_t total_length = 0;
    for (const auto& run : node.runs) {
        total_length += run.length;
    }
    EXPECT_EQ(total_length, static_cast<uint32_t>(node.text.size()));
}

TEST(Parser, RunsAreContiguous) {
    auto nodes = ParseMarkdown("a **b** c");
    ASSERT_EQ(nodes.size(), 1u);
    const auto& runs = nodes[0].runs;
    for (size_t i = 1; i < runs.size(); i++) {
        EXPECT_EQ(runs[i].start, runs[i - 1].start + runs[i - 1].length)
            << "ラン " << (i - 1) << " と " << i << " の間にギャップあり";
    }
}

// ---- ネストされたリスト ----

TEST(Parser, NestedUnorderedList) {
    auto nodes = ParseMarkdown("- a\n  - b\n    - c");
    ASSERT_GE(nodes.size(), 3u);
    // より深いアイテムはより高いインデントレベルを持つべき
    EXPECT_LT(nodes[0].indent_level, nodes[1].indent_level);
    EXPECT_LT(nodes[1].indent_level, nodes[2].indent_level);
}

TEST(Parser, NestedOrderedList) {
    auto nodes = ParseMarkdown("1. a\n   1. b\n      1. c");
    ASSERT_GE(nodes.size(), 3u);
    EXPECT_EQ(nodes[0].list_number, 1);
    EXPECT_EQ(nodes[1].list_number, 1);
    EXPECT_EQ(nodes[2].list_number, 1);
    EXPECT_LT(nodes[0].indent_level, nodes[1].indent_level);
}

TEST(Parser, MixedListNesting) {
    auto nodes = ParseMarkdown("1. ordered\n   - unordered\n   - unordered2");
    ASSERT_GE(nodes.size(), 3u);
    EXPECT_GT(nodes[0].list_number, 0);
    EXPECT_EQ(nodes[1].list_number, 0);
    EXPECT_EQ(nodes[2].list_number, 0);
}

// ---- 言語指定付きコードブロック ----

TEST(Parser, CodeBlockWithLanguage) {
    auto nodes = ParseMarkdown("```cpp\nint x = 1;\n```");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Cpp);
}

TEST(Parser, CodeBlockNoTrailingNewline) {
    auto nodes = ParseMarkdown("```\nhello\n```");
    ASSERT_EQ(nodes.size(), 1u);
    // 末尾の改行は除去されるべき
    EXPECT_FALSE(nodes[0].text.empty());
    EXPECT_NE(nodes[0].text.back(), L'\n');
}

// ---- インライン書式付きテーブル ----

TEST(Parser, TableCellWithBold) {
    auto nodes = ParseMarkdown(
        "| A | **B** |\n"
        "|---|---|\n"
        "| 1 | 2 |"
    );
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_GE(nodes[0].table_rows.size(), 1u);
    auto& header = nodes[0].table_rows[0];
    ASSERT_GE(header.cells.size(), 2u);
    // 2番目のヘッダーセルは太字ランを持つべき
    bool has_bold = false;
    for (const auto& run : header.cells[1].runs) {
        if (run.bold) has_bold = true;
    }
    EXPECT_TRUE(has_bold);
}

TEST(Parser, TableLinearizedText) {
    auto nodes = ParseMarkdown(
        "| A | B |\n"
        "|---|---|\n"
        "| 1 | 2 |"
    );
    ASSERT_EQ(nodes.size(), 1u);
    // パーサーは線形化テキストを構築しない（レイアウトが行う）ので、構造だけ確認
    ASSERT_GE(nodes[0].table_rows.size(), 2u);
    EXPECT_EQ(nodes[0].table_rows[0].cells[0].text, L"A");
    EXPECT_EQ(nodes[0].table_rows[0].cells[1].text, L"B");
}

// ---- リンク付きテーブル ----

TEST(Parser, TableCellWithLink) {
    auto nodes = ParseMarkdown(
        "| Name | Link |\n"
        "|------|------|\n"
        "| foo | [bar](https://example.com) |"
    );
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_GE(nodes[0].table_rows.size(), 2u);
    const auto& data_row = nodes[0].table_rows[1];
    ASSERT_GE(data_row.cells.size(), 2u);

    // リンクセルはランにlink_urlを持つべき
    bool found_link = false;
    for (const auto& run : data_row.cells[1].runs) {
        if (run.link_url.has_value()) {
            EXPECT_EQ(*run.link_url, L"https://example.com");
            found_link = true;
        }
    }
    EXPECT_TRUE(found_link);

    // リンクでないセルはリンクを持たないべき
    for (const auto& run : data_row.cells[0].runs) {
        EXPECT_FALSE(run.link_url.has_value());
    }
}

TEST(Parser, TableCellWithInternalLink) {
    auto nodes = ParseMarkdown(
        "| Section |\n"
        "|---------|\n"
        "| [intro](#introduction) |"
    );
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_GE(nodes[0].table_rows.size(), 2u);
    const auto& cell = nodes[0].table_rows[1].cells[0];

    bool found = false;
    for (const auto& run : cell.runs) {
        if (run.link_url.has_value()) {
            EXPECT_EQ(*run.link_url, L"#introduction");
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(Parser, TableCellWithBoldLink) {
    auto nodes = ParseMarkdown(
        "| Link |\n"
        "|------|\n"
        "| [**bold**](https://example.com) |"
    );
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_GE(nodes[0].table_rows.size(), 2u);
    const auto& cell = nodes[0].table_rows[1].cells[0];

    bool has_bold_link = false;
    for (const auto& run : cell.runs) {
        if (run.bold && run.link_url.has_value()) {
            has_bold_link = true;
        }
    }
    EXPECT_TRUE(has_bold_link);
}

TEST(Parser, TableCellMixedTextAndLink) {
    auto nodes = ParseMarkdown(
        "| Content |\n"
        "|---------|\n"
        "| before [link](https://example.com) after |"
    );
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_GE(nodes[0].table_rows.size(), 2u);
    const auto& cell = nodes[0].table_rows[1].cells[0];

    // リンク付きとリンクなしのランを持つべき
    bool has_link_run = false;
    bool has_plain_run = false;
    for (const auto& run : cell.runs) {
        if (run.link_url.has_value()) has_link_run = true;
        else has_plain_run = true;
    }
    EXPECT_TRUE(has_link_run);
    EXPECT_TRUE(has_plain_run);
}

// ---- HTMLエンティティのエッジケース ----

TEST(Parser, HtmlEntityQuot) {
    auto nodes = ParseMarkdown("&quot;hello&quot;");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_NE(nodes[0].text.find(L"\""), std::wstring::npos);
}

TEST(Parser, HtmlEntityNbsp) {
    auto nodes = ParseMarkdown("a&nbsp;b");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_NE(nodes[0].text.find(L'\u00A0'), std::wstring::npos);
}

// ---- ハードブレーク ----

TEST(Parser, HardBreakWithTwoSpaces) {
    auto nodes = ParseMarkdown("line1  \nline2");
    ASSERT_EQ(nodes.size(), 1u);
    // ハードブレークはテキスト内に改行を生成するべき
    EXPECT_NE(nodes[0].text.find(L'\n'), std::wstring::npos);
}

// ---- 複数見出しのアンカー一意性 ----

TEST(Parser, MultipleHeadingsHaveAnchors) {
    auto nodes = ParseMarkdown("# A\n\n## B\n\n### C");
    for (const auto& node : nodes) {
        if (node.type == NodeType::Heading) {
            EXPECT_FALSE(node.anchor_id.empty())
                << "見出しにアンカーがない";
        }
    }
}

// ---- インライン書式付きリンク ----

TEST(Parser, BoldLink) {
    auto nodes = ParseMarkdown("[**bold link**](https://example.com)");
    ASSERT_EQ(nodes.size(), 1u);
    bool has_bold_link = false;
    for (const auto& run : nodes[0].runs) {
        if (run.bold && run.link_url.has_value()) {
            has_bold_link = true;
        }
    }
    EXPECT_TRUE(has_bold_link);
}

// ---- 重複アンカーIDに一意のサフィックスが付与される ----

TEST(Parser, DuplicateHeadingAnchorsAreUnique) {
    auto nodes = ParseMarkdown("# Title\n\n## Title\n\n### Title");
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(nodes[0].anchor_id, L"title");
    EXPECT_EQ(nodes[1].anchor_id, L"title-1");
    EXPECT_EQ(nodes[2].anchor_id, L"title-2");
}

TEST(Parser, DuplicateAnchorsWithDifferentText) {
    auto nodes = ParseMarkdown("# A\n\n## B\n\n### A");
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(nodes[0].anchor_id, L"a");
    EXPECT_EQ(nodes[1].anchor_id, L"b");
    EXPECT_EQ(nodes[2].anchor_id, L"a-1");
}

// ---- 数値HTMLエンティティ ----

TEST(Parser, NumericEntityDecimal) {
    auto nodes = ParseMarkdown("&#65;");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].text, L"A");
}

TEST(Parser, NumericEntityHex) {
    auto nodes = ParseMarkdown("&#x41;");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].text, L"A");
}

TEST(Parser, NumericEntityJapanese) {
    // &#x3042; = あ (Hiragana A)
    auto nodes = ParseMarkdown("&#x3042;");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].text, L"\u3042");
}

// ---- 深いネスト ----

TEST(Parser, DeeplyNestedList) {
    std::string md;
    md += "- L1\n";
    md += "  - L2\n";
    md += "    - L3\n";
    md += "      - L4\n";
    auto nodes = ParseMarkdown(md);
    ASSERT_GE(nodes.size(), 4u);
    // より深いレベルはより高いindent_levelを持つべき
    for (size_t i = 1; i < nodes.size(); i++) {
        EXPECT_GE(nodes[i].indent_level, nodes[i - 1].indent_level);
    }
}

// ---- 不揃いな列数のテーブル ----

TEST(Parser, TableUnevenColumns) {
    // md4cがこれを処理する - 行内のセルが少ない場合
    auto nodes = ParseMarkdown(
        "| A | B | C |\n"
        "|---|---|---|\n"
        "| 1 | 2 |\n"
    );
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::Table);
    ASSERT_GE(nodes[0].table_rows.size(), 2u);
}

// ---- Mermaid言語のコードブロック ----

TEST(Parser, MermaidCodeBlock) {
    auto nodes = ParseMarkdown("```mermaid\ngraph TD;\n  A-->B;\n```");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, NodeType::CodeBlock);
    EXPECT_EQ(nodes[0].code_language, SyntaxLanguage::Mermaid);
}

// ---- 特殊文字を含むURL ----

TEST(Parser, LinkWithSpecialCharsInUrl) {
    auto nodes = ParseMarkdown("[link](https://example.com/path?q=1&r=2#frag)");
    ASSERT_EQ(nodes.size(), 1u);
    bool found = false;
    for (const auto& run : nodes[0].runs) {
        if (run.link_url.has_value()) {
            EXPECT_EQ(*run.link_url, L"https://example.com/path?q=1&r=2#frag");
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

// ---- 空の見出し ----

TEST(Parser, EmptyHeading) {
    auto nodes = ParseMarkdown("# \n\ntext");
    // md4cは空テキストの見出しノードを生成する場合がある
    bool found_heading = false;
    for (auto& n : nodes) {
        if (n.type == NodeType::Heading) {
            found_heading = true;
        }
    }
    // md4cの動作により空の見出しが生成されるかどうかは不定;
    // 少なくともクラッシュしないべき。
    (void)found_heading;
}

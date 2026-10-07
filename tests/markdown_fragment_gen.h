#pragma once
// プロパティテスト用のランダム Markdown 生成器。パーサが特殊扱いする断片
// (ネストしたリスト / 引用、tight / loose リスト内のブロック、Alert、セル数不揃いの表、
// entity、$$、画像、CJK、不正 UTF-8 バイト) を再帰的に組み合わせる。
// 同じ seed なら同じ文書を返すので、失敗時は SCOPED_TRACE の seed で再現できる。
#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

class MarkdownFragmentGen {
public:
    explicit MarkdownFragmentGen(uint32_t seed) : rng_(seed)
    {}

    std::string Document(int block_count)
    {
        std::string out;
        for (int i = 0; i < block_count; ++i) {
            if (i > 0) {
                out += Chance(4) ? "\n" : "\n\n";
            }
            out += Block(0);
        }
        return out;
    }

    // 改行を含まない 1 行分のインライン列。
    std::string InlineLine(int max_atoms = 4)
    {
        static constexpr std::array<std::string_view, 33> kAtoms{
            "alpha", "beta", "gamma", "**bold**", "*em*", "_em2_", "***both***", "`code`", "~~del~~",
            "[link](https://example.com/x)", "[**bl**](#anchor)", "[js](javascript:alert(1))",
            "<https://auto.example>", "&amp;", "&lt;x&gt;", "&#10;", "&#x1F600;", "&nbsp;", "&bogus;",
            "$x^2$", "$$y$$", "![img](pic.png)", "日本語", "漢字かな", "\xFF\xFE", "\xE3\x81", "\xC0\x80",
            "a\\*b", "<br>", "<b>x</b>", "1 < 2 & 3 > 0", "\"q\" 'q'", "**[x](https://b.example) y**",
        };
        const int n = Range(1, max_atoms);
        std::string out;
        for (int i = 0; i < n; ++i) {
            if (i > 0) {
                out += ' ';
            }
            out += kAtoms[Range(0, static_cast<int>(kAtoms.size()) - 1)];
        }
        return out;
    }

    std::string Block(int depth)
    {
        const int kind = Range(0, depth >= kMaxDepth ? 7 : 11);
        switch (kind) {
        case 0:
            return Paragraph();
        case 1:
            return std::string(static_cast<size_t>(Range(1, 6)), '#') + " " + InlineLine();
        case 2:
            return Fence();
        case 3:
            return Chance(2) ? "***" : "___";
        case 4:
            return Table();
        case 5:
            return Chance(2) ? "$$\na+b\n\\frac{1}{2}\n$$" : "$$x$$";
        case 6:
            return Chance(2) ? "![alt **b**](img/p.png)" : "&#10;&#10;x";
        case 7:
            return Alert();
        case 8:
        case 9:
            return Quote(depth);
        default:
            return List(depth);
        }
    }

    // 全行に prefix を付ける。空行には blank_prefix を付ける (リスト継続行の字下げ / 引用の ">")。
    static std::string PrefixLines(std::string_view body, std::string_view first, std::string_view rest, std::string_view blank)
    {
        std::string out;
        size_t pos = 0;
        bool is_first = true;
        while (true) {
            const size_t nl = body.find('\n', pos);
            const auto line = body.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
            if (is_first) {
                out += first;
            }
            else {
                out += line.empty() ? blank : rest;
            }
            out += line;
            is_first = false;
            if (nl == std::string_view::npos) {
                break;
            }
            out += '\n';
            pos = nl + 1;
        }
        return out;
    }

    bool Chance(int one_in)
    {
        return Range(1, one_in) == 1;
    }

    int Range(int lo, int hi)
    {
        return std::uniform_int_distribution<int>(lo, hi)(rng_);
    }

private:
    static constexpr int kMaxDepth = 3;

    std::string Paragraph()
    {
        std::string out = InlineLine();
        const int extra = Range(0, 2);
        for (int i = 0; i < extra; ++i) {
            out += Chance(2) ? "  \n" : "\n";
            out += InlineLine();
        }
        return out;
    }

    std::string Fence()
    {
        static constexpr std::array<std::string_view, 4> kLangs{ "", "cpp", "mermaid", "python" };
        static constexpr std::array<std::string_view, 4> kLines{ "int x = 1; // <&>", "日本 \"s\"", "", "\tend" };
        std::string out = "```";
        out += kLangs[Range(0, 3)];
        const int n = Range(0, 3);
        for (int i = 0; i < n; ++i) {
            out += '\n';
            out += kLines[Range(0, 3)];
        }
        out += "\n```";
        return out;
    }

    std::string Table()
    {
        static constexpr std::array<std::string_view, 4> kSeps{ "---", ":--", ":-:", "--:" };
        const int cols = Range(1, 4);
        std::string out = "|";
        for (int c = 0; c < cols; ++c) {
            out += " h" + std::to_string(c) + " |";
        }
        out += "\n|";
        for (int c = 0; c < cols; ++c) {
            out += ' ';
            out += kSeps[Range(0, 3)];
            out += " |";
        }
        const int rows = Range(0, 3);
        for (int r = 0; r < rows; ++r) {
            out += "\n|";
            // 列数より少ない / 多い行も混ぜる (md4c は不足を埋め、超過を捨てる)。
            const int cells = Range(std::max(0, cols - 1), cols + 1);
            for (int c = 0; c < cells; ++c) {
                out += ' ';
                if (!Chance(5)) {
                    out += InlineLine(2);
                }
                out += " |";
            }
        }
        return out;
    }

    std::string Alert()
    {
        static constexpr std::array<std::string_view, 6> kTypes{ "NOTE", "tip", "Warning", "IMPORTANT", "caution", "NOPE" };
        std::string out = "> [!";
        out += kTypes[Range(0, 5)];
        out += "]";
        if (Chance(3)) {
            out += " " + InlineLine();
        }
        const int lines = Range(0, 2);
        for (int i = 0; i < lines; ++i) {
            out += "\n> " + InlineLine();
        }
        return out;
    }

    std::string Quote(int depth)
    {
        std::string body = Block(depth + 1);
        if (Chance(2)) {
            body += "\n\n" + Block(depth + 1);
        }
        return PrefixLines(body, "> ", "> ", ">");
    }

    std::string List(int depth)
    {
        static constexpr std::array<std::string_view, 4> kMarkers{ "- ", "* ", "1. ", "- [ ] " };
        const auto marker = kMarkers[Range(0, 3)];
        // タスクマーカーの継続行は "- " と同じ 2 桁字下げ。
        const std::string indent(marker.size() == 6 ? 2 : marker.size(), ' ');
        const bool loose = Chance(2);
        const int items = Range(1, 3);
        std::string out;
        for (int i = 0; i < items; ++i) {
            if (i > 0) {
                out += loose ? "\n\n" : "\n";
            }
            std::string body = Block(depth + 1);
            const int extra = Range(0, 2);
            for (int k = 0; k < extra; ++k) {
                body += Chance(2) ? "\n" : "\n\n";
                body += Block(depth + 1);
            }
            out += PrefixLines(body, marker, indent, "");
        }
        return out;
    }

    std::mt19937 rng_;
};

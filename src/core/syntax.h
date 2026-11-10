#pragma once
#include <span>
#include <string_view>
#include <vector>
#include <cstdint>

enum class SyntaxLanguage : uint8_t {
    None,
    Cpp,
    Python,
    JavaScript,
    Mermaid,
    Go,
    Rust,
    TypeScript,
    Bash,
    PowerShell,
    Cmd,
    Json,
    LatexMath,
    Java,
    CSharp,
    Kotlin,
    Swift,
    Php,
    Ruby,
    Sql,
    Yaml,
    Toml,
    Ini,
    Html,
    Xml,
    Css,
    Scss,
    Diff,
    Dockerfile,
    Makefile,
    Zig,
    Haskell,
    Clojure,
    EmacsLisp,
    CommonLisp,
    Scheme, // Racket を含む
    Lua,
    Vim,
    Vim9 // コメントが # の Vim9 script
};

constexpr bool IsDiagramLanguage(SyntaxLanguage lang) noexcept
{
    return lang == SyntaxLanguage::Mermaid;
}

// トークンが行をまたがず、行から行へ持ち越す状態も無い言語。行の頭からなら、前を読まずに先頭と同じように解析できる。
constexpr bool IsLineLocalLanguage(SyntaxLanguage lang) noexcept
{
    return lang == SyntaxLanguage::Ini;
}

enum class SyntaxTokenType : uint8_t {
    Plain,
    Keyword,
    Type,
    String,
    Number,
    Comment,
    Preprocessor,
    Function,
    Inserted, // diff の追加行
    Deleted   // diff の削除行
};

struct SyntaxToken {
    uint32_t start = 0;
    uint32_t length = 0;
    SyntaxTokenType type = SyntaxTokenType::Plain;

    constexpr bool operator==(const SyntaxToken&) const noexcept = default;
};

// 行や入れ子の状態を持つ言語 (CSS・HTML・YAML など) を、コードの途中から解析し始めるための状態。
// 符号は言語ごとで、Node が続くトークンと一緒に持てるよう kSyntaxLexStateBits に収める。0 はコードの先頭の状態。
using SyntaxLexState = uint32_t;
inline constexpr int kSyntaxLexStateBits = 24;

// 解析の途中の状態の記録。区切ったコードの並びを窓ごとに前から解析するとき (AnnotateCodeChunkLeads) にだけ渡し、
// 継ぎ目での状態と、次の窓を始められる位置を取り出す。位置は解析したテキストの先頭から数える。
// 状態が変わるたびに残すと 16MB の Emacs Lisp のパースが 82 → 110ms になるので、残すのは知りたい位置 (継ぎ目と最後の行の頭) の
// 分だけにする。
class SyntaxStateTrace {
public:
    // 解析を始め直せる位置と、そこでの状態。
    struct Point {
        size_t pos = 0;
        SyntaxLexState state = 0;
    };

    // wanted は状態を知りたい位置 (昇順)。位置 0 の状態は entry。
    void Reset(SyntaxLexState entry, size_t text_size, std::span<const size_t> wanted);
    // どのトークンの頭からでも状態なしで始め直せる言語 (LexerConfig) の解析だった。
    void SetStateless() noexcept
    {
        stateless_ = true;
    }

    // --- lexer が呼ぶ。pos は昇順 ---

    // まだ状態を残していない知りたい位置が pos 以前にある。状態を詰めるのは安くないので、真のときだけ Set を呼ぶ。
    bool Wants(size_t pos) const noexcept
    {
        return pos >= next_wanted_;
    }
    // まだ状態を残していない最初の知りたい位置。無ければ size_t の最大値。
    size_t next_wanted() const noexcept
    {
        return next_wanted_;
    }
    // pos は解析を始め直せる位置で、そこでの状態が state。pos 以前の知りたい位置からの解析は、ここから始める
    // (知りたい位置がトークンの途中なら、pos はそのトークンの直後)。テキストの末尾でも呼ぶ。
    void Set(size_t pos, SyntaxLexState state);
    // [begin, end) のどこからでも state で始め直せる (<script> の中身や、継続行の残りなど)。
    void SetRange(size_t begin, size_t end, SyntaxLexState state);
    // 先読みが scanned_size (読んでいるテキストの長さ) に届き、pos (そこでの状態が state) から後ろの結果が続きを読むまで
    // 決まらない。テキストの一部だけを読む入れ子の lexer の末尾は、テキストの末尾ではないので数えない。
    void SetUnsettled(size_t pos, SyntaxLexState state, size_t scanned_size) noexcept;

    // --- 結果 ---

    // 知りたい位置 wanted からの解析を始める位置と状態。
    Point For(size_t wanted) const noexcept;
    // テキストの末尾に届いたトークン (頭が token_start) を、続きをつないで解析し直すときに始める位置と状態。
    Point RestartFor(size_t token_start) const noexcept;
    // ここまでは結果が決まっている。先読みが末尾に届いていなければテキストの末尾。
    Point SettledEnd() const noexcept
    {
        return settled_end_;
    }

    // 入れ子の lexer (HTML の <style> の中の CSS) が残す状態に重ねる、外側の lexer の状態。
    SyntaxLexState outer = 0;

private:
    static constexpr size_t kNone = static_cast<size_t>(-1);

    std::vector<size_t> wanted_;
    // wanted_ の先頭から、状態を残した分。
    std::vector<Point> points_;
    size_t next_wanted_ = kNone;
    size_t text_size_ = 0;
    Point entry_;
    Point settled_end_;
    bool unsettled_ = false;
    bool stateless_ = false;
};

SyntaxLanguage DetectLanguage(std::string_view info_string) noexcept;

std::vector<SyntaxToken> Tokenize(std::string_view text, SyntaxLanguage language);

// text の begin から後ろを解析し、Tokenize と同じトークンを tokens の後ろに足す。位置は text の先頭から数える。
// state は begin での状態で、begin はトークンの外にあること。
void AppendTokens(std::string_view text, SyntaxLanguage language, std::vector<SyntaxToken>& tokens, size_t begin = 0, SyntaxLexState state = 0);

// state から始めた Tokenize の結果のうち、boundaries (昇順の位置) のどれかをまたぐトークンと text の末尾に届くトークンを返す。
// LexerConfig で解析する言語はそれだけを作るので Tokenize より速い。そのほかの言語は全部のトークンを返し、trace に途中の状態を残す。
std::vector<SyntaxToken> TokensCrossing(std::string_view text, SyntaxLanguage language, std::span<const size_t> boundaries,
                                        SyntaxLexState state = 0, SyntaxStateTrace* trace = nullptr);

// 各言語のキーワード・型名テーブル。syntax.cpp からのみ include される内部ヘッダ。
// 語の多くはどの表にも無いので、文字列に触らず空き番地で外れと分かる開番地ハッシュにする。長さごとの二分探索
// より、16KB のトークン化が C++ 35.8→22.0µs、SQL 47.8→27.5µs、Vim 55.6→41.6µs、Emacs Lisp 59.4→50.9µs。
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <string_view>
#include <cstdint>
#include "ascii_util.h"

namespace syntax_keywords {

inline constexpr uint16_t kEmptySlot = 0xFFFF;

// 大文字小文字を区別しない探索でも同じ番地になるよう、文字を畳んでから混ぜる。w は空でないこと。
constexpr size_t WordHash(std::string_view w) noexcept
{
    const auto fold = [](char c) static noexcept { return size_t{ static_cast<unsigned char>(c) } | 0x20; };
    const size_t n = w.size();
    return n + fold(w[0]) * 5 + fold(w[n - 1]) * 31 + fold(w[n / 2]) * 131;
}

// type-erased view。言語ごとに異なる N のテーブルを同じ型で受け渡すため、
// 各 WordTable<N> から暗黙変換できる軽量ビューを使う。
struct KeywordTable {
    std::span<const std::string_view> entries;
    std::span<const uint16_t> slots; // 大きさは 2 のべき乗

    [[nodiscard]] constexpr bool contains(std::string_view word) const noexcept
    {
        return Probe(word, [word](std::string_view entry) { return entry == word; });
    }

    // 小文字で格納した表を引く。
    [[nodiscard]] constexpr bool contains_ignore_case(std::string_view word) const noexcept
    {
        return Probe(word, [word](std::string_view entry) { return std::ranges::equal(word, entry, {}, ascii_util::ToLowerAscii); });
    }

private:
    template <class Equal>
    [[nodiscard]] constexpr bool Probe(std::string_view word, Equal equal) const noexcept
    {
        if (word.empty() || slots.empty()) {
            return false;
        }
        const size_t mask = slots.size() - 1;
        for (size_t h = WordHash(word) & mask;; h = (h + 1) & mask) {
            const uint16_t index = slots[h];
            if (index == kEmptySlot) {
                return false;
            }
            if (equal(entries[index])) {
                return true;
            }
        }
    }
};

// 番地は語の数の 3〜6 倍。2〜4 倍にすると exe は 4KB 減るが Emacs Lisp が 50.9→55.6µs に戻る。
template <size_t N>
struct WordTable {
    std::array<std::string_view, N> entries{};
    std::array<uint16_t, std::bit_ceil(N * 3)> slots{};

    constexpr operator KeywordTable() const noexcept
    {
        return KeywordTable{
            std::span<const std::string_view>{ entries },
            std::span<const uint16_t>{ slots }
        };
    }

    [[nodiscard]] constexpr bool contains(std::string_view word) const noexcept
    {
        return KeywordTable(*this).contains(word);
    }

    [[nodiscard]] constexpr bool contains_ignore_case(std::string_view word) const noexcept
    {
        return KeywordTable(*this).contains_ignore_case(word);
    }
};

template <size_t N>
consteval WordTable<N> MakeWordTable(std::array<std::string_view, N> words)
{
    WordTable<N> r;
    r.entries = words;
    r.slots.fill(kEmptySlot);
    const size_t mask = r.slots.size() - 1;
    for (size_t i = 0; i < N; ++i) {
        size_t h = WordHash(words[i]) & mask;
        while (r.slots[h] != kEmptySlot) {
            h = (h + 1) & mask;
        }
        r.slots[h] = static_cast<uint16_t>(i);
    }
    return r;
}

template <size_t A, size_t B>
consteval std::array<std::string_view, A + B> Concat(const std::array<std::string_view, A>& a, const std::array<std::string_view, B>& b)
{
    std::array<std::string_view, A + B> r;
    std::ranges::copy(b, std::ranges::copy(a, r.begin()).out);
    return r;
}

// clang-format off
inline constexpr auto CPP_KEYWORDS = MakeWordTable(std::to_array<std::string_view>({
    "auto", "break", "case", "catch", "class", "co_await", "co_return", "co_yield", "concept",
    "const", "const_cast", "consteval", "constexpr", "constinit", "continue", "decltype", "default",
    "delete", "do", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false",
    "final", "for", "friend", "goto", "if", "import", "inline", "module", "mutable", "namespace",
    "new", "noexcept", "nullptr", "operator", "override", "private", "protected", "public",
    "register", "reinterpret_cast", "requires", "return", "sizeof", "static", "static_assert",
    "static_cast", "struct", "switch", "template", "this", "throw", "true", "try", "typedef",
    "typeid", "typename", "union", "using", "virtual", "void", "volatile", "while"
}));

inline constexpr auto CPP_TYPES = MakeWordTable(std::to_array<std::string_view>({
    "BOOL", "DWORD", "HANDLE", "HINSTANCE", "HRESULT", "HWND", "LPARAM", "LRESULT", "POINT", "RECT",
    "SIZE", "UINT", "WPARAM", "array", "bool", "char", "char16_t", "char32_t", "char8_t", "double",
    "float", "int", "int16_t", "int32_t", "int64_t", "int8_t", "long", "map", "optional", "pair",
    "ptrdiff_t", "set", "shared_ptr", "short", "signed", "size_t", "span", "string", "string_view",
    "tuple", "uint16_t", "uint32_t", "uint64_t", "uint8_t", "unique_ptr", "unordered_map",
    "unordered_set", "unsigned", "variant", "vector", "wchar_t", "weak_ptr", "wstring",
    "wstring_view"
}));

inline constexpr auto PYTHON_KEYWORDS = MakeWordTable(std::to_array<std::string_view>({
    "False", "None", "True", "and", "as", "assert", "async", "await", "break", "class", "continue",
    "def", "del", "elif", "else", "except", "finally", "for", "from", "global", "if", "import",
    "in", "is", "lambda", "nonlocal", "not", "or", "pass", "raise", "return", "try", "while",
    "with", "yield"
}));

inline constexpr auto PYTHON_TYPES = MakeWordTable(std::to_array<std::string_view>({
    "Exception", "IOError", "IndexError", "KeyError", "OSError", "RuntimeError", "StopIteration",
    "TypeError", "ValueError", "bool", "bytearray", "bytes", "classmethod", "complex", "dict",
    "float", "frozenset", "int", "list", "memoryview", "object", "property", "range", "set",
    "staticmethod", "str", "tuple", "type"
}));

inline constexpr auto JS_KEYWORD_LIST = std::to_array<std::string_view>({
    "as", "async", "await", "break", "case", "catch", "class", "const", "continue", "debugger",
    "default", "delete", "do", "else", "export", "extends", "finally", "for", "from", "function",
    "if", "import", "in", "instanceof", "let", "new", "of", "return", "static", "super", "switch",
    "this", "throw", "try", "typeof", "var", "void", "while", "with", "yield"
});

inline constexpr auto JS_KEYWORDS = MakeWordTable(JS_KEYWORD_LIST);

inline constexpr auto JS_TYPE_LIST = std::to_array<std::string_view>({
    "Array", "BigInt", "Boolean", "Date", "Error", "Function", "Infinity", "JSON", "Map", "Math",
    "NaN", "Number", "Object", "Promise", "Proxy", "Reflect", "RegExp", "Set", "String", "Symbol",
    "WeakMap", "WeakSet", "console", "document", "false", "globalThis", "null", "true", "undefined",
    "window"
});

inline constexpr auto JS_TYPES = MakeWordTable(JS_TYPE_LIST);

inline constexpr auto GO_KEYWORDS = MakeWordTable(std::to_array<std::string_view>({
    "break", "case", "chan", "const", "continue", "default", "defer", "else", "fallthrough", "for",
    "func", "go", "goto", "if", "import", "interface", "map", "package", "range", "return",
    "select", "struct", "switch", "type", "var"
}));

inline constexpr auto GO_TYPES = MakeWordTable(std::to_array<std::string_view>({
    "any", "bool", "byte", "comparable", "complex128", "complex64", "error", "false", "float32",
    "float64", "int", "int16", "int32", "int64", "int8", "iota", "nil", "rune", "string", "true",
    "uint", "uint16", "uint32", "uint64", "uint8", "uintptr"
}));

inline constexpr auto RUST_KEYWORDS = MakeWordTable(std::to_array<std::string_view>({
    "Self", "as", "async", "await", "break", "const", "continue", "crate", "dyn", "else", "enum",
    "extern", "false", "fn", "for", "if", "impl", "in", "let", "loop", "macro_rules", "match",
    "mod", "move", "mut", "pub", "ref", "return", "self", "static", "struct", "super", "trait",
    "true", "type", "unsafe", "use", "where", "while", "yield"
}));

inline constexpr auto RUST_TYPES = MakeWordTable(std::to_array<std::string_view>({
    "Arc", "BTreeMap", "BTreeSet", "Box", "Cell", "Cow", "Err", "HashMap", "HashSet", "LinkedList",
    "None", "Ok", "Option", "PhantomData", "Pin", "Rc", "RefCell", "Result", "Some", "String",
    "Vec", "VecDeque", "bool", "char", "f32", "f64", "i128", "i16", "i32", "i64", "i8", "isize",
    "str", "u128", "u16", "u32", "u64", "u8", "usize"
}));

inline constexpr auto TS_KEYWORDS = MakeWordTable(Concat(JS_KEYWORD_LIST, std::to_array<std::string_view>({
    "abstract", "asserts", "declare", "enum", "implements", "infer", "interface", "is", "keyof",
    "module", "namespace", "override", "readonly", "satisfies", "type"
})));

inline constexpr auto TS_TYPES = MakeWordTable(Concat(JS_TYPE_LIST, std::to_array<std::string_view>({
    "Awaited", "Capitalize", "ConstructorParameters", "Exclude", "Extract", "InstanceType",
    "Lowercase", "NonNullable", "Omit", "Parameters", "Partial", "Pick", "Readonly", "Record",
    "Required", "ReturnType", "ThisType", "Uncapitalize", "Uppercase", "any", "bigint", "boolean",
    "never", "number", "object", "string", "symbol", "unknown"
})));

inline constexpr auto BASH_KEYWORDS = MakeWordTable(std::to_array<std::string_view>({
    "break", "case", "continue", "declare", "do", "done", "elif", "else", "esac", "eval", "exec",
    "exit", "export", "fi", "for", "function", "if", "in", "local", "readonly", "return", "select",
    "set", "shift", "source", "then", "time", "trap", "typeset", "unset", "until", "while"
}));

inline constexpr auto BASH_TYPES = MakeWordTable(std::to_array<std::string_view>({
    "alias", "builtin", "cd", "command", "echo", "false", "getopts", "let", "mapfile", "printf",
    "pwd", "read", "readarray", "test", "true", "type", "unalias", "which"
}));

// 大文字小文字を区別しないマッチングのため小文字で格納。
inline constexpr auto PWSH_KEYWORDS = MakeWordTable(std::to_array<std::string_view>({
    "begin", "break", "catch", "class", "continue", "data", "do", "dynamicparam", "else", "elseif",
    "end", "enum", "exit", "filter", "finally", "for", "foreach", "from", "function", "hidden",
    "if", "in", "inlinescript", "param", "process", "return", "static", "switch", "throw", "trap",
    "try", "until", "using", "while", "workflow"
}));

inline constexpr auto PWSH_TYPES = MakeWordTable(std::to_array<std::string_view>({
    "array", "bool", "byte", "char", "datetime", "decimal", "double", "false", "float", "hashtable",
    "int", "long", "null", "regex", "scriptblock", "string", "timespan", "true", "void", "xml"
}));

// 大文字小文字を区別しないマッチングのため小文字で格納。
inline constexpr auto CMD_KEYWORDS = MakeWordTable(std::to_array<std::string_view>({
    "call", "defined", "do", "echo", "else", "endlocal", "equ", "errorlevel", "exist", "exit",
    "for", "geq", "goto", "gtr", "if", "in", "leq", "lss", "neq", "not", "off", "on", "pause",
    "rem", "set", "setlocal"
}));

inline constexpr auto CMD_TYPES = MakeWordTable(std::to_array<std::string_view>({
    "attrib", "cls", "color", "copy", "del", "dir", "find", "findstr", "ipconfig", "md", "mkdir",
    "more", "move", "net", "netsh", "ping", "rd", "reg", "ren", "rename", "rmdir", "robocopy", "sc",
    "sort", "start", "taskkill", "tasklist", "title", "type", "ver", "xcopy"
}));

// 型名は該当概念がない。
inline constexpr auto JSON_KEYWORDS = MakeWordTable(std::to_array<std::string_view>({
    "false", "null", "true"
}));

inline constexpr auto JAVA_KEYWORDS = MakeWordTable(std::to_array<std::string_view>({
    "abstract", "assert", "break", "case", "catch", "class", "const", "continue", "default", "do",
    "else", "enum", "extends", "false", "final", "finally", "for", "goto", "if", "implements",
    "import", "instanceof", "interface", "native", "new", "null", "package", "permits", "private",
    "protected", "public", "record", "return", "sealed", "static", "strictfp", "super", "switch",
    "synchronized", "this", "throw", "throws", "transient", "true", "try", "var", "void",
    "volatile", "while", "yield"
}));

inline constexpr auto JAVA_TYPES = MakeWordTable(std::to_array<std::string_view>({
    "ArrayList", "Boolean", "Byte", "Character", "Collection", "Double", "Exception", "Float",
    "HashMap", "HashSet", "Integer", "Iterable", "List", "Long", "Map", "Math", "Object",
    "Optional", "RuntimeException", "Set", "Short", "Stream", "String", "StringBuilder", "System",
    "Thread", "boolean", "byte", "char", "double", "float", "int", "long", "short"
}));

inline constexpr auto CSHARP_KEYWORDS = MakeWordTable(std::to_array<std::string_view>({
    "abstract", "and", "as", "async", "await", "base", "break", "case", "catch", "checked", "class",
    "const", "continue", "default", "delegate", "do", "else", "enum", "event", "explicit", "extern",
    "false", "file", "finally", "fixed", "for", "foreach", "get", "global", "goto", "if",
    "implicit", "in", "init", "interface", "internal", "is", "lock", "nameof", "namespace", "new",
    "not", "null", "operator", "or", "out", "override", "params", "partial", "private", "protected",
    "public", "readonly", "record", "ref", "required", "return", "sealed", "set", "sizeof",
    "stackalloc", "static", "struct", "switch", "this", "throw", "true", "try", "typeof",
    "unchecked", "unsafe", "using", "value", "var", "virtual", "void", "volatile", "when", "where",
    "while", "with", "yield"
}));

inline constexpr auto CSHARP_TYPES = MakeWordTable(std::to_array<std::string_view>({
    "Action", "Console", "DateTime", "Dictionary", "Exception", "Func", "Guid", "HashSet",
    "IEnumerable", "List", "Math", "Object", "ReadOnlySpan", "Span", "String", "StringBuilder",
    "Task", "TimeSpan", "bool", "byte", "char", "decimal", "double", "dynamic", "float", "int",
    "long", "nint", "nuint", "object", "sbyte", "short", "string", "uint", "ulong", "ushort"
}));

inline constexpr auto KOTLIN_KEYWORDS = MakeWordTable(std::to_array<std::string_view>({
    "abstract", "actual", "annotation", "as", "break", "by", "catch", "class", "companion", "const",
    "constructor", "continue", "crossinline", "data", "do", "else", "enum", "expect", "external",
    "false", "final", "finally", "for", "fun", "get", "if", "import", "in", "infix", "init",
    "inline", "inner", "interface", "internal", "is", "lateinit", "noinline", "null", "object",
    "open", "operator", "out", "override", "package", "private", "protected", "public", "reified",
    "return", "sealed", "set", "super", "suspend", "tailrec", "this", "throw", "true", "try",
    "typealias", "val", "value", "var", "vararg", "when", "where", "while"
}));

inline constexpr auto KOTLIN_TYPES = MakeWordTable(std::to_array<std::string_view>({
    "Any", "Array", "Boolean", "Byte", "Char", "Double", "Exception", "Float", "Int", "IntArray",
    "List", "Long", "Map", "MutableList", "MutableMap", "MutableSet", "Nothing", "Pair", "Sequence",
    "Set", "Short", "String", "Throwable", "Triple", "Unit"
}));

inline constexpr auto SWIFT_KEYWORDS = MakeWordTable(std::to_array<std::string_view>({
    "Self", "actor", "any", "as", "associatedtype", "async", "await", "break", "case", "catch",
    "class", "continue", "convenience", "default", "defer", "deinit", "didSet", "do", "dynamic",
    "else", "enum", "extension", "fallthrough", "false", "fileprivate", "final", "for", "func",
    "get", "guard", "if", "import", "in", "indirect", "init", "inout", "internal", "is", "lazy",
    "let", "mutating", "nil", "nonmutating", "open", "operator", "optional", "override",
    "precedencegroup", "private", "protocol", "public", "repeat", "required", "rethrows", "return",
    "self", "set", "some", "static", "struct", "subscript", "super", "switch", "throw", "throws",
    "true", "try", "typealias", "unowned", "var", "weak", "where", "while", "willSet"
}));

inline constexpr auto SWIFT_TYPES = MakeWordTable(std::to_array<std::string_view>({
    "Any", "AnyObject", "Array", "Bool", "Character", "Data", "Date", "Dictionary", "Double",
    "Error", "Float", "Int", "Int16", "Int32", "Int64", "Int8", "Never", "Optional", "Result",
    "Set", "String", "UInt", "UInt16", "UInt32", "UInt64", "UInt8", "URL", "Void"
}));

// 大文字小文字を区別しないマッチングのため小文字で格納。
inline constexpr auto PHP_KEYWORDS = MakeWordTable(std::to_array<std::string_view>({
    "abstract", "and", "array", "as", "break", "callable", "case", "catch", "class", "clone",
    "const", "continue", "declare", "default", "do", "echo", "else", "elseif", "empty",
    "enddeclare", "endfor", "endforeach", "endif", "endswitch", "endwhile", "enum", "extends",
    "false", "final", "finally", "fn", "for", "foreach", "function", "global", "goto", "if",
    "implements", "include", "include_once", "instanceof", "insteadof", "interface", "isset",
    "list", "match", "namespace", "new", "null", "or", "parent", "print", "private", "protected",
    "public", "readonly", "require", "require_once", "return", "self", "static", "switch", "throw",
    "trait", "true", "try", "unset", "use", "var", "while", "xor", "yield"
}));

// 大文字小文字を区別しないマッチングのため小文字で格納。
inline constexpr auto PHP_TYPES = MakeWordTable(std::to_array<std::string_view>({
    "bool", "exception", "float", "int", "iterable", "mixed", "never", "object", "string", "void"
}));

inline constexpr auto RUBY_KEYWORDS = MakeWordTable(std::to_array<std::string_view>({
    "BEGIN", "END", "__FILE__", "__LINE__", "__method__", "alias", "and", "begin", "break", "case",
    "class", "def", "defined", "do", "else", "elsif", "end", "ensure", "false", "for", "if", "in",
    "module", "next", "nil", "not", "or", "redo", "rescue", "retry", "return", "self", "super",
    "then", "true", "undef", "unless", "until", "when", "while", "yield"
}));

inline constexpr auto RUBY_TYPES = MakeWordTable(std::to_array<std::string_view>({
    "ArgumentError", "Array", "Class", "Comparable", "Dir", "Enumerable", "Exception", "File",
    "Float", "Hash", "IO", "Integer", "Kernel", "Module", "NilClass", "Object", "Proc", "Range",
    "RuntimeError", "StandardError", "String", "Struct", "Symbol", "Time", "attr_accessor",
    "attr_reader", "attr_writer", "extend", "include", "lambda", "loop", "private", "proc",
    "protected", "public", "puts", "raise", "require", "require_relative"
}));

// 大文字小文字を区別しないマッチングのため小文字で格納。
inline constexpr auto SQL_KEYWORDS = MakeWordTable(std::to_array<std::string_view>({
    "add", "all", "alter", "analyze", "and", "as", "asc", "begin", "between", "by", "cascade",
    "case", "check", "collate", "column", "commit", "conflict", "constraint", "create", "cross",
    "database", "declare", "default", "delete", "desc", "distinct", "do", "drop", "else", "end",
    "escape", "except", "exec", "execute", "exists", "explain", "false", "fetch", "foreign", "from",
    "full", "function", "go", "grant", "group", "having", "if", "ilike", "in", "index", "inner",
    "insert", "intersect", "into", "is", "join", "key", "lateral", "left", "like", "limit", "merge",
    "natural", "next", "not", "nothing", "null", "offset", "on", "only", "or", "order", "outer",
    "over", "partition", "primary", "procedure", "recursive", "references", "replace", "return",
    "returning", "returns", "revoke", "right", "rollback", "rows", "schema", "select", "set",
    "show", "similar", "table", "temp", "temporary", "then", "top", "transaction", "trigger",
    "true", "truncate", "union", "unique", "update", "use", "using", "vacuum", "values", "view",
    "when", "where", "window", "with"
}));

// 大文字小文字を区別しないマッチングのため小文字で格納。
inline constexpr auto SQL_TYPES = MakeWordTable(std::to_array<std::string_view>({
    "bigint", "bigserial", "binary", "blob", "bool", "boolean", "bytea", "char", "clob", "date",
    "datetime", "decimal", "double", "float", "int", "integer", "interval", "json", "jsonb",
    "money", "nchar", "numeric", "nvarchar", "precision", "real", "serial", "smallint", "text",
    "time", "timestamp", "timestamptz", "tinyint", "uuid", "varbinary", "varchar", "xml"
}));

// 大文字小文字を区別しないマッチングのため小文字で格納。
inline constexpr auto DOCKER_KEYWORDS = MakeWordTable(std::to_array<std::string_view>({
    "add", "arg", "cmd", "copy", "entrypoint", "env", "expose", "from", "healthcheck", "label",
    "maintainer", "onbuild", "run", "shell", "stopsignal", "user", "volume", "workdir"
}));

inline constexpr auto MAKE_KEYWORDS = MakeWordTable(std::to_array<std::string_view>({
    "-include", "define", "else", "endef", "endif", "export", "ifdef", "ifeq", "ifndef", "ifneq",
    "include", "override", "private", "sinclude", "undefine", "unexport", "vpath"
}));

inline constexpr auto ZIG_KEYWORDS = MakeWordTable(std::to_array<std::string_view>({
    "addrspace", "align", "allowzero", "and", "anyframe", "anytype", "asm", "async", "await",
    "break", "callconv", "catch", "comptime", "const", "continue", "defer", "else", "enum",
    "errdefer", "error", "export", "extern", "false", "fn", "for", "if", "inline", "linksection",
    "noalias", "noinline", "nosuspend", "null", "opaque", "or", "orelse", "packed", "pub", "resume",
    "return", "struct", "suspend", "switch", "test", "threadlocal", "true", "try", "undefined",
    "union", "unreachable", "usingnamespace", "var", "volatile", "while"
}));

inline constexpr auto ZIG_TYPES = MakeWordTable(std::to_array<std::string_view>({
    "anyerror", "anyopaque", "bool", "c_char", "c_int", "c_long", "c_longlong", "c_short", "c_uint",
    "c_ulong", "c_ulonglong", "c_ushort", "comptime_float", "comptime_int", "f128", "f16", "f32",
    "f64", "f80", "i128", "i16", "i32", "i64", "i8", "isize", "noreturn", "type", "u128", "u16",
    "u32", "u64", "u8", "usize", "void"
}));

inline constexpr auto HASKELL_KEYWORDS = MakeWordTable(std::to_array<std::string_view>({
    "as", "case", "class", "data", "default", "deriving", "do", "else", "family", "forall",
    "foreign", "hiding", "if", "import", "in", "infix", "infixl", "infixr", "instance", "let",
    "mdo", "module", "newtype", "of", "pattern", "proc", "qualified", "rec", "then", "type", "where"
}));

// 小文字で格納し、大文字小文字を区別せずに引く。
inline constexpr auto COMMON_LISP_SPECIAL = MakeWordTable(std::to_array<std::string_view>({
    "and", "block", "case", "catch", "cond", "decf", "declaim", "declare", "defclass",
    "defconstant", "defgeneric", "define-condition", "defmacro", "defmethod", "defpackage",
    "defparameter", "defstruct", "defun", "defvar", "destructuring-bind", "do", "do*", "dolist",
    "dotimes", "ecase", "etypecase", "eval-when", "flet", "function", "go", "handler-bind",
    "handler-case", "if", "in-package", "incf", "labels", "lambda", "let", "let*", "locally",
    "loop", "macrolet", "multiple-value-bind", "or", "pop", "prog1", "prog2", "progn", "push",
    "quote", "restart-case", "return", "return-from", "setf", "setq", "symbol-macrolet", "tagbody",
    "the", "throw", "typecase", "unless", "unwind-protect", "when", "with-open-file", "with-slots"
}));

inline constexpr auto EMACS_LISP_SPECIAL = MakeWordTable(std::to_array<std::string_view>({
    "add-hook", "and", "catch", "cl-case", "cl-defmacro", "cl-defun", "cl-loop", "cond",
    "condition-case", "defalias", "defconst", "defcustom", "defface", "defgroup",
    "define-derived-mode", "define-key", "define-minor-mode", "defmacro", "defsubst", "defun",
    "defvar", "dolist", "dotimes", "function", "if", "interactive", "lambda", "let", "let*", "or",
    "pcase", "pcase-let", "prog1", "prog2", "progn", "provide", "quote", "require",
    "save-excursion", "save-match-data", "save-restriction", "setq", "setq-local", "throw",
    "unless", "unwind-protect", "use-package", "when", "while", "with-current-buffer",
    "with-temp-buffer"
}));

// Common Lisp / Emacs Lisp の定数。
inline constexpr auto LISP_NIL_T = MakeWordTable(std::to_array<std::string_view>({
    "nil", "t"
}));

inline constexpr auto SCHEME_SPECIAL = MakeWordTable(std::to_array<std::string_view>({
    "and", "begin", "case", "case-lambda", "cond", "define", "define-record-type", "define-syntax",
    "define-syntax-rule", "define-values", "delay", "do", "else", "for", "for/fold", "for/list",
    "guard", "if", "lambda", "let", "let*", "let-values", "letrec", "letrec*", "match", "module",
    "not", "or", "parameterize", "provide", "quasiquote", "quote", "require", "set!", "struct",
    "syntax-case", "syntax-rules", "unless", "unquote", "when", "with-handlers", "λ"
}));

// Racket の定数と、[else ...] のように呼び出し位置にならない cond / case の else。#t / #f は字句側で扱う。
inline constexpr auto SCHEME_CONSTANTS = MakeWordTable(std::to_array<std::string_view>({
    "else", "false", "null", "true"
}));

inline constexpr auto CLOJURE_SPECIAL = MakeWordTable(std::to_array<std::string_view>({
    "->", "->>", "and", "as->", "binding", "case", "catch", "comment", "cond", "cond->", "condp",
    "declare", "def", "defmacro", "defmethod", "defmulti", "defn", "defn-", "defonce",
    "defprotocol", "defrecord", "deftype", "do", "doseq", "dotimes", "doto", "extend-protocol",
    "extend-type", "finally", "fn", "for", "if", "if-let", "if-not", "import", "let", "letfn",
    "loop", "new", "ns", "or", "quote", "recur", "reify", "require", "set!", "some->", "some->>",
    "throw", "try", "var", "when", "when-let", "when-not"
}));

inline constexpr auto CLOJURE_CONSTANTS = MakeWordTable(std::to_array<std::string_view>({
    "false", "nil", "true"
}));

// 直後のシンボルが定義名、その次のリストが引数リストになるフォーム（全方言共通）。
inline constexpr auto LISP_DEFINERS = MakeWordTable(std::to_array<std::string_view>({
    "cl-defmacro", "cl-defun", "defgeneric", "define-derived-mode", "define-minor-mode", "defmacro",
    "defmethod", "defmulti", "defn", "defn-", "defsubst", "defun"
}));

// 直後のリストが引数リストか束縛リストで、中のリストを呼び出しと見ないフォーム（全方言共通）。
// Clojure の束縛は [] を使うので影響しない。
inline constexpr auto LISP_PLAIN_LIST_FORMS = MakeWordTable(std::to_array<std::string_view>({
    "cl-flet", "cl-labels", "destructuring-bind", "flet", "if-let", "labels", "lambda", "let",
    "let*", "let*-values", "let-values", "letfn", "letrec", "letrec*", "macrolet",
    "multiple-value-bind", "pcase-let", "symbol-macrolet", "when-let", "λ"
}));

inline constexpr auto LUA_KEYWORDS = MakeWordTable(std::to_array<std::string_view>({
    "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in",
    "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"
}));

// 標準ライブラリと組み込み関数。Neovim 設定で頻出の vim も含める。
inline constexpr auto LUA_TYPES = MakeWordTable(std::to_array<std::string_view>({
    "assert", "collectgarbage", "coroutine", "debug", "dofile", "error", "getmetatable", "io",
    "ipairs", "load", "loadfile", "math", "next", "os", "package", "pairs", "pcall", "print",
    "rawequal", "rawget", "rawlen", "rawset", "require", "select", "self", "setmetatable", "string",
    "table", "tonumber", "tostring", "type", "utf8", "vim", "xpcall"
}));

// 行頭 (と | の後) のコマンド名。よく使う省略形も含める。
inline constexpr auto VIM_COMMANDS = MakeWordTable(std::to_array<std::string_view>({
    "au", "aug", "augroup", "autocmd", "brea", "break", "cal", "call", "cat", "catch", "cd", "colo",
    "colorscheme", "com", "command", "con", "cons", "const", "continue", "def", "delcommand",
    "delfunction", "doautocmd", "e", "ec", "echo", "echoe", "echoerr", "echoh", "echohl", "echom",
    "echomsg", "echon", "edit", "el", "else", "elsei", "elseif", "en", "enddef", "endf", "endfo",
    "endfor", "endfu", "endfun", "endfunc", "endfunction", "endi", "endif", "endt", "endtry",
    "endw", "endwhile", "exe", "exec", "execute", "export", "filet", "filetype", "fina", "final",
    "finally", "fini", "finish", "fo", "for", "fu", "fun", "func", "function", "hi", "highlight",
    "if", "import", "lcd", "let", "lua", "luafile", "norm", "normal", "pa", "packadd", "perl",
    "py3", "python3", "q", "quit", "redr", "redraw", "retu", "return", "ru", "ruby", "runtime",
    "scripte", "scriptencoding", "se", "set", "setg", "setglobal", "setl", "setlocal", "sil",
    "silent", "sl", "sleep", "so", "source", "star", "startinsert", "stopi", "stopinsert", "syn",
    "syntax", "tcd", "try", "unl", "unlet", "up", "update", "var", "vim9script", "w", "wh", "while",
    "winc", "wincmd", "wq", "write", "x"
}));

// 引数がマッピング (キー列) になるコマンド。行末までを式として読まない。
inline constexpr auto VIM_MAP_COMMANDS = MakeWordTable(std::to_array<std::string_view>({
    "ab", "abbreviate", "cab", "cabbrev", "cm", "cmap", "cno", "cnoremap", "cunmap", "iab",
    "iabbrev", "im", "imap", "ino", "inoremap", "iunmap", "lmap", "lnoremap", "map", "nm", "nmap",
    "nn", "nno", "nnoremap", "noremap", "nun", "nunmap", "omap", "ono", "onoremap", "smap",
    "snoremap", "tmap", "tno", "tnoremap", "una", "unabbreviate", "unmap", "vm", "vmap", "vn",
    "vnoremap", "vunmap", "xmap", "xn", "xnoremap", "xunmap"
}));

inline constexpr auto VIM_EXPR_KEYWORDS = MakeWordTable(std::to_array<std::string_view>({
    "abort", "closure", "dict", "false", "in", "is", "isnot", "null", "range", "true"
}));

// Vim9 script の型名。
inline constexpr auto VIM_TYPES = MakeWordTable(std::to_array<std::string_view>({
    "any", "blob", "bool", "channel", "dict", "float", "func", "job", "list", "number", "string",
    "void"
}));

// clang-format on

} // namespace syntax_keywords

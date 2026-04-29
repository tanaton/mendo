// 各言語のキーワード・型名テーブル。syntax.cpp からのみ include される内部ヘッダ。
// ソート済み配列 + std::ranges::binary_search でヒープ確保を完全に排除。
// MakeSorted は consteval なのでソート順はコンパイル時に確定する。
#pragma once
#include <algorithm>
#include <array>
#include <span>
#include <string_view>

namespace syntax_keywords {

using namespace std::literals;

using KeywordTable = std::span<const std::wstring_view>;

template<size_t N>
consteval std::array<std::wstring_view, N> MakeSorted(std::array<std::wstring_view, N> arr)
{
    std::ranges::sort(arr);
    return arr;
}

inline constexpr auto CPP_KEYWORDS = MakeSorted(std::array{
    L"auto"sv, L"break"sv, L"case"sv, L"catch"sv, L"class"sv, L"co_await"sv,
    L"co_return"sv, L"co_yield"sv, L"concept"sv, L"const"sv, L"const_cast"sv,
    L"consteval"sv, L"constexpr"sv, L"constinit"sv, L"continue"sv, L"decltype"sv,
    L"default"sv, L"delete"sv, L"do"sv, L"dynamic_cast"sv, L"else"sv, L"enum"sv,
    L"explicit"sv, L"export"sv, L"extern"sv, L"false"sv, L"final"sv, L"for"sv,
    L"friend"sv, L"goto"sv, L"if"sv, L"import"sv, L"inline"sv, L"module"sv,
    L"mutable"sv, L"namespace"sv, L"new"sv, L"noexcept"sv, L"nullptr"sv,
    L"operator"sv, L"override"sv, L"private"sv, L"protected"sv, L"public"sv,
    L"register"sv, L"reinterpret_cast"sv, L"requires"sv, L"return"sv,
    L"sizeof"sv, L"static"sv, L"static_assert"sv, L"static_cast"sv,
    L"struct"sv, L"switch"sv, L"template"sv, L"this"sv, L"throw"sv, L"true"sv,
    L"try"sv, L"typedef"sv, L"typeid"sv, L"typename"sv, L"union"sv, L"using"sv,
    L"virtual"sv, L"void"sv, L"volatile"sv, L"while"sv,
});

inline constexpr auto CPP_TYPES = MakeSorted(std::array{
    L"BOOL"sv, L"DWORD"sv, L"HANDLE"sv, L"HINSTANCE"sv, L"HRESULT"sv, L"HWND"sv,
    L"LPARAM"sv, L"LRESULT"sv, L"POINT"sv, L"RECT"sv, L"SIZE"sv, L"UINT"sv, L"WPARAM"sv,
    L"array"sv, L"bool"sv, L"char"sv, L"char16_t"sv, L"char32_t"sv, L"char8_t"sv,
    L"double"sv, L"float"sv, L"int"sv, L"int16_t"sv, L"int32_t"sv, L"int64_t"sv,
    L"int8_t"sv, L"long"sv, L"map"sv, L"optional"sv, L"pair"sv, L"ptrdiff_t"sv,
    L"set"sv, L"shared_ptr"sv, L"short"sv, L"signed"sv, L"size_t"sv, L"span"sv,
    L"string"sv, L"string_view"sv, L"tuple"sv, L"uint16_t"sv, L"uint32_t"sv,
    L"uint64_t"sv, L"uint8_t"sv, L"unique_ptr"sv, L"unordered_map"sv, L"unordered_set"sv,
    L"unsigned"sv, L"variant"sv, L"vector"sv, L"wchar_t"sv, L"weak_ptr"sv,
    L"wstring"sv, L"wstring_view"sv,
});

inline constexpr auto PYTHON_KEYWORDS = MakeSorted(std::array{
    L"False"sv, L"None"sv, L"True"sv,
    L"and"sv, L"as"sv, L"assert"sv, L"async"sv, L"await"sv, L"break"sv, L"class"sv,
    L"continue"sv, L"def"sv, L"del"sv, L"elif"sv, L"else"sv, L"except"sv, L"finally"sv,
    L"for"sv, L"from"sv, L"global"sv, L"if"sv, L"import"sv, L"in"sv, L"is"sv,
    L"lambda"sv, L"nonlocal"sv, L"not"sv, L"or"sv, L"pass"sv, L"raise"sv,
    L"return"sv, L"try"sv, L"while"sv, L"with"sv, L"yield"sv,
});

inline constexpr auto PYTHON_TYPES = MakeSorted(std::array{
    L"Exception"sv, L"IOError"sv, L"IndexError"sv, L"KeyError"sv, L"OSError"sv,
    L"RuntimeError"sv, L"StopIteration"sv, L"TypeError"sv, L"ValueError"sv,
    L"bool"sv, L"bytearray"sv, L"bytes"sv, L"classmethod"sv, L"complex"sv,
    L"dict"sv, L"float"sv, L"frozenset"sv, L"int"sv, L"list"sv, L"memoryview"sv,
    L"object"sv, L"property"sv, L"range"sv, L"set"sv, L"staticmethod"sv,
    L"str"sv, L"tuple"sv, L"type"sv,
});

inline constexpr auto JS_KEYWORDS = MakeSorted(std::array{
    L"as"sv, L"async"sv, L"await"sv, L"break"sv, L"case"sv, L"catch"sv, L"class"sv,
    L"const"sv, L"continue"sv, L"debugger"sv, L"default"sv, L"delete"sv, L"do"sv,
    L"else"sv, L"export"sv, L"extends"sv, L"finally"sv, L"for"sv, L"from"sv,
    L"function"sv, L"if"sv, L"import"sv, L"in"sv, L"instanceof"sv, L"let"sv,
    L"new"sv, L"of"sv, L"return"sv, L"static"sv, L"super"sv, L"switch"sv, L"this"sv,
    L"throw"sv, L"try"sv, L"typeof"sv, L"var"sv, L"void"sv, L"while"sv, L"with"sv,
    L"yield"sv,
});

inline constexpr auto JS_TYPES = MakeSorted(std::array{
    L"Array"sv, L"BigInt"sv, L"Boolean"sv, L"Date"sv, L"Error"sv, L"Function"sv,
    L"Infinity"sv, L"JSON"sv, L"Map"sv, L"Math"sv, L"NaN"sv, L"Number"sv,
    L"Object"sv, L"Promise"sv, L"Proxy"sv, L"Reflect"sv, L"RegExp"sv, L"Set"sv,
    L"String"sv, L"Symbol"sv, L"WeakMap"sv, L"WeakSet"sv,
    L"console"sv, L"document"sv, L"false"sv, L"globalThis"sv, L"null"sv, L"true"sv,
    L"undefined"sv, L"window"sv,
});

inline constexpr auto GO_KEYWORDS = MakeSorted(std::array{
    L"break"sv, L"case"sv, L"chan"sv, L"const"sv, L"continue"sv, L"default"sv,
    L"defer"sv, L"else"sv, L"fallthrough"sv, L"for"sv, L"func"sv, L"go"sv,
    L"goto"sv, L"if"sv, L"import"sv, L"interface"sv, L"map"sv, L"package"sv,
    L"range"sv, L"return"sv, L"select"sv, L"struct"sv, L"switch"sv, L"type"sv,
    L"var"sv,
});

inline constexpr auto GO_TYPES = MakeSorted(std::array{
    L"any"sv, L"bool"sv, L"byte"sv, L"comparable"sv, L"complex128"sv, L"complex64"sv,
    L"error"sv, L"false"sv, L"float32"sv, L"float64"sv, L"int"sv, L"int16"sv,
    L"int32"sv, L"int64"sv, L"int8"sv, L"iota"sv, L"nil"sv, L"rune"sv, L"string"sv,
    L"true"sv, L"uint"sv, L"uint16"sv, L"uint32"sv, L"uint64"sv, L"uint8"sv,
    L"uintptr"sv,
});

inline constexpr auto RUST_KEYWORDS = MakeSorted(std::array{
    L"Self"sv,
    L"as"sv, L"async"sv, L"await"sv, L"break"sv, L"const"sv, L"continue"sv,
    L"crate"sv, L"dyn"sv, L"else"sv, L"enum"sv, L"extern"sv, L"false"sv, L"fn"sv,
    L"for"sv, L"if"sv, L"impl"sv, L"in"sv, L"let"sv, L"loop"sv, L"macro_rules"sv,
    L"match"sv, L"mod"sv, L"move"sv, L"mut"sv, L"pub"sv, L"ref"sv, L"return"sv,
    L"self"sv, L"static"sv, L"struct"sv, L"super"sv, L"trait"sv, L"true"sv,
    L"type"sv, L"unsafe"sv, L"use"sv, L"where"sv, L"while"sv, L"yield"sv,
});

inline constexpr auto RUST_TYPES = MakeSorted(std::array{
    L"Arc"sv, L"BTreeMap"sv, L"BTreeSet"sv, L"Box"sv, L"Cell"sv, L"Cow"sv,
    L"Err"sv, L"HashMap"sv, L"HashSet"sv, L"LinkedList"sv, L"None"sv, L"Ok"sv,
    L"Option"sv, L"PhantomData"sv, L"Pin"sv, L"Rc"sv, L"RefCell"sv, L"Result"sv,
    L"Some"sv, L"String"sv, L"Vec"sv, L"VecDeque"sv,
    L"bool"sv, L"char"sv, L"f32"sv, L"f64"sv, L"i128"sv, L"i16"sv, L"i32"sv,
    L"i64"sv, L"i8"sv, L"isize"sv, L"str"sv, L"u128"sv, L"u16"sv, L"u32"sv,
    L"u64"sv, L"u8"sv, L"usize"sv,
});

// TypeScript = JS + TS固有キーワード（マージ済み）
inline constexpr auto TS_KEYWORDS = MakeSorted(std::array{
    L"abstract"sv, L"as"sv, L"asserts"sv, L"async"sv, L"await"sv, L"break"sv,
    L"case"sv, L"catch"sv, L"class"sv, L"const"sv, L"continue"sv, L"debugger"sv,
    L"declare"sv, L"default"sv, L"delete"sv, L"do"sv, L"else"sv, L"enum"sv,
    L"export"sv, L"extends"sv, L"finally"sv, L"for"sv, L"from"sv, L"function"sv,
    L"if"sv, L"implements"sv, L"import"sv, L"in"sv, L"infer"sv, L"instanceof"sv,
    L"interface"sv, L"is"sv, L"keyof"sv, L"let"sv, L"module"sv, L"namespace"sv,
    L"new"sv, L"of"sv, L"override"sv, L"readonly"sv, L"return"sv, L"satisfies"sv,
    L"static"sv, L"super"sv, L"switch"sv, L"this"sv, L"throw"sv, L"try"sv,
    L"type"sv, L"typeof"sv, L"var"sv, L"void"sv, L"while"sv, L"with"sv, L"yield"sv,
});

inline constexpr auto TS_TYPES = MakeSorted(std::array{
    L"Array"sv, L"Awaited"sv, L"BigInt"sv, L"Boolean"sv, L"Capitalize"sv,
    L"ConstructorParameters"sv, L"Date"sv, L"Error"sv, L"Exclude"sv, L"Extract"sv,
    L"Function"sv, L"Infinity"sv, L"InstanceType"sv, L"JSON"sv, L"Lowercase"sv,
    L"Map"sv, L"Math"sv, L"NaN"sv, L"NonNullable"sv, L"Number"sv, L"Object"sv,
    L"Omit"sv, L"Parameters"sv, L"Partial"sv, L"Pick"sv, L"Promise"sv, L"Proxy"sv,
    L"Readonly"sv, L"Record"sv, L"Reflect"sv, L"RegExp"sv, L"Required"sv,
    L"ReturnType"sv, L"Set"sv, L"String"sv, L"Symbol"sv, L"ThisType"sv,
    L"Uncapitalize"sv, L"Uppercase"sv, L"WeakMap"sv, L"WeakSet"sv,
    L"any"sv, L"bigint"sv, L"boolean"sv, L"console"sv, L"document"sv, L"false"sv,
    L"globalThis"sv, L"never"sv, L"null"sv, L"number"sv, L"object"sv, L"string"sv,
    L"symbol"sv, L"true"sv, L"undefined"sv, L"unknown"sv, L"window"sv,
});

inline constexpr auto BASH_KEYWORDS = MakeSorted(std::array{
    L"break"sv, L"case"sv, L"continue"sv, L"declare"sv, L"do"sv, L"done"sv,
    L"elif"sv, L"else"sv, L"esac"sv, L"eval"sv, L"exec"sv, L"exit"sv, L"export"sv,
    L"fi"sv, L"for"sv, L"function"sv, L"if"sv, L"in"sv, L"local"sv, L"readonly"sv,
    L"return"sv, L"select"sv, L"set"sv, L"shift"sv, L"source"sv, L"then"sv,
    L"time"sv, L"trap"sv, L"typeset"sv, L"unset"sv, L"until"sv, L"while"sv,
});

inline constexpr auto BASH_TYPES = MakeSorted(std::array{
    L"alias"sv, L"builtin"sv, L"cd"sv, L"command"sv, L"echo"sv, L"false"sv,
    L"getopts"sv, L"let"sv, L"mapfile"sv, L"printf"sv, L"pwd"sv, L"read"sv,
    L"readarray"sv, L"test"sv, L"true"sv, L"type"sv, L"unalias"sv, L"which"sv,
});

// PowerShell（大文字小文字を区別しないマッチングのためキーワードは小文字で格納）
inline constexpr auto PWSH_KEYWORDS = MakeSorted(std::array{
    L"begin"sv, L"break"sv, L"catch"sv, L"class"sv, L"continue"sv, L"data"sv,
    L"do"sv, L"dynamicparam"sv, L"else"sv, L"elseif"sv, L"end"sv, L"enum"sv,
    L"exit"sv, L"filter"sv, L"finally"sv, L"for"sv, L"foreach"sv, L"from"sv,
    L"function"sv, L"hidden"sv, L"if"sv, L"in"sv, L"inlinescript"sv, L"param"sv,
    L"process"sv, L"return"sv, L"static"sv, L"switch"sv, L"throw"sv, L"trap"sv,
    L"try"sv, L"until"sv, L"using"sv, L"while"sv, L"workflow"sv,
});

inline constexpr auto PWSH_TYPES = MakeSorted(std::array{
    L"array"sv, L"bool"sv, L"byte"sv, L"char"sv, L"datetime"sv, L"decimal"sv,
    L"double"sv, L"false"sv, L"float"sv, L"hashtable"sv, L"int"sv, L"long"sv,
    L"null"sv, L"regex"sv, L"scriptblock"sv, L"string"sv, L"timespan"sv,
    L"true"sv, L"void"sv, L"xml"sv,
});

// Cmd（大文字小文字を区別しないマッチングのためキーワードは小文字で格納）
inline constexpr auto CMD_KEYWORDS = MakeSorted(std::array{
    L"call"sv, L"defined"sv, L"do"sv, L"echo"sv, L"else"sv, L"endlocal"sv,
    L"equ"sv, L"errorlevel"sv, L"exist"sv, L"exit"sv, L"for"sv, L"geq"sv,
    L"goto"sv, L"gtr"sv, L"if"sv, L"in"sv, L"leq"sv, L"lss"sv, L"neq"sv,
    L"not"sv, L"off"sv, L"on"sv, L"pause"sv, L"rem"sv, L"set"sv, L"setlocal"sv,
});

inline constexpr auto CMD_TYPES = MakeSorted(std::array{
    L"attrib"sv, L"cls"sv, L"color"sv, L"copy"sv, L"del"sv, L"dir"sv, L"find"sv,
    L"findstr"sv, L"ipconfig"sv, L"md"sv, L"mkdir"sv, L"more"sv, L"move"sv,
    L"net"sv, L"netsh"sv, L"ping"sv, L"rd"sv, L"reg"sv, L"ren"sv, L"rename"sv,
    L"rmdir"sv, L"robocopy"sv, L"sc"sv, L"sort"sv, L"start"sv, L"taskkill"sv,
    L"tasklist"sv, L"title"sv, L"type"sv, L"ver"sv, L"xcopy"sv,
});

// JSON / JSONC: リテラル値のみ。型名は該当概念がない。
inline constexpr auto JSON_KEYWORDS = MakeSorted(std::array{
    L"false"sv, L"null"sv, L"true"sv,
});

} // namespace syntax_keywords

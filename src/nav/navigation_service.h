#pragma once
#include <string>
#include <string_view>
#include <memory_resource>

// リンククリックの結果を表す値型。
// URLスキーム検証とアンカー判定のみを行い、副作用は持たない。
struct LinkClickResult {
    enum class Type { None, Anchor, ExternalUrl };
    Type type = Type::None;
    std::pmr::wstring target;
};

// リンククリック処理。URLの種類を判定して結果を返す。副作用は起こさない。
LinkClickResult HandleLinkClick(std::wstring_view url);

// ShellExecuteW に渡しても安全な URL スキームか判定する。
// http / https / mailto のみ許可し、file://, javascript: などはブロック。
bool IsSafeUrlScheme(std::wstring_view url) noexcept;

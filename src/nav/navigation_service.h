#pragma once
#include <string>
#include <string_view>
#include <memory_resource>

struct LinkClickResult {
    enum class Type { None, Anchor, ExternalUrl };
    Type type = Type::None;
    std::pmr::wstring target;
};

LinkClickResult HandleLinkClick(std::wstring_view url);
bool IsSafeUrlScheme(std::wstring_view url) noexcept;

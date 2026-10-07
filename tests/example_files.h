#pragma once
#include <filesystem>
#include <fstream>
#include <memory_resource>
#include <sstream>
#include <string>
#include <string_view>

// "example/xxx.md" 形式の相対パスを、cwd に依存せずソースツリーの example/ から開く。
// MENDO_EXAMPLE_DIR (CMake が u8 リテラルで渡す絶対パス) を基準に解決し、日本語を含む
// UTF-8 パスも std::filesystem::path 経由で正しく扱う。define が無い場合は cwd 相対で開く。
// 開けなければ空文字列を返す (呼び出し側で GTEST_SKIP する)。
inline std::pmr::string ReadExampleFileBytes(std::string_view path)
{
    std::filesystem::path full;
#ifdef MENDO_EXAMPLE_DIR
    std::string_view rel = path;
    constexpr std::string_view prefix = "example/";
    if (rel.starts_with(prefix)) {
        rel.remove_prefix(prefix.size());
    }
    full = std::filesystem::path(MENDO_EXAMPLE_DIR) / std::filesystem::path(rel);
#else
    full = std::filesystem::path(path);
#endif
    std::ifstream file(full, std::ios::binary);
    if (!file) {
        return {};
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    const std::string s = ss.str();
    return std::pmr::string{ s.data(), s.size() };
}

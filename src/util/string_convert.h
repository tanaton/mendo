#pragma once
#include "profiler.h"
#include <limits>
#include <string>
#include <string_view>
#include <memory_resource>
#include <windows.h>

namespace string_convert {

inline void Utf8ToWide(std::string_view utf8, std::pmr::wstring& out)
{
    MENDO_PROFILE("Utf8ToWide");
    if (utf8.empty()) {
        out.clear();
        return;
    }
    // Windows API の cb*Char は int なので int の最大値を超える入力は拒否する
    if (utf8.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        out.clear();
        return;
    }
    // wchar 数 <= UTF-8 byte 数 が常に成立するため、上限確保で API 1 回呼び出し
    out.resize_and_overwrite(utf8.size(), [utf8](wchar_t* buf, size_t count) -> size_t {
        const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), buf, static_cast<int>(count));
        return n > 0 ? static_cast<size_t>(n) : 0;
    });
    MENDO_STATF("Utf8ToWide(utf8): utf8=%zu wide.size=%zu wide.cap=%zu", utf8.size(), out.size(), out.capacity());
}

inline std::pmr::wstring Utf8ToWide(std::string_view utf8)
{
    std::pmr::wstring result;
    Utf8ToWide(utf8, result);
    return result;
}

// 先頭の UTF-8 BOM (EF BB BF) を取り除いた string_view を返す。
inline std::string_view StripUtf8Bom(std::string_view utf8) noexcept
{
    constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";
    if (utf8.starts_with(kUtf8Bom)) {
        utf8.remove_prefix(kUtf8Bom.size());
    }
    return utf8;
}

// std::string / std::pmr::string 両対応の WideToUtf8。
template <typename Alloc>
inline void WideToUtf8(std::wstring_view wide, std::basic_string<char, std::char_traits<char>, Alloc>& out)
{
    if (wide.empty()) {
        out.clear();
        return;
    }
    // wide.size() * 3 のオーバーフローと cb*Char の int 制限を同時にガード
    if (wide.size() > static_cast<size_t>(std::numeric_limits<int>::max()) / 3) {
        out.clear();
        return;
    }
    // UTF-8 byte 数 <= UTF-16 wchar 数 * 3 が常に成立するため、上限確保で API 1 回呼び出し
    out.resize_and_overwrite(wide.size() * 3, [wide](char* buf, size_t count) -> size_t {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), buf, static_cast<int>(count), nullptr, nullptr);
        return n > 0 ? static_cast<size_t>(n) : 0;
    });
}

inline std::string WideToUtf8(std::wstring_view wide)
{
    std::string result;
    WideToUtf8(wide, result);
    return result;
}

} // namespace string_convert

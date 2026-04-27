#include "anchor_slug.h"
#include "ascii_util.h"

std::pmr::wstring GenerateAnchorId(std::wstring_view text)
{
    std::pmr::wstring slug;
    slug.reserve(text.size());
    for (wchar_t c : text) {
        const wchar_t lower = ascii_util::ToLowerAscii(c);
        if ((lower >= L'a' && lower <= L'z') || (lower >= L'0' && lower <= L'9') || lower == L'-' || lower == L'_') {
            slug += lower;
        }
        else if (c == L' ' || c == L'\t') {
            slug += L'-';
        }
        // CJK文字: そのまま保持するが、句読点・記号はスキップ
        else if (c >= 0x3000) {
            bool skip = false;
            // CJK記号と句読点 (U+3000-U+303F): 、。「」【】〈〉 等
            if (c <= 0x303F) {
                skip = true;
            }
            // 全角ASCII対応の句読点
            else if (c >= 0xFF01 && c <= 0xFF0F) {
                skip = true; // ！＂＃…（）＊＋，－．／
            }
            else if (c >= 0xFF1A && c <= 0xFF20) {
                skip = true; // ：；＜＝＞？＠
            }
            else if (c >= 0xFF3B && c <= 0xFF40) {
                skip = true; // ［＼］＾＿｀
            }
            else if (c >= 0xFF5B && c <= 0xFF65) {
                skip = true; // ｛｜｝～…･
            }
            if (!skip) {
                slug += c;
            }
        }
        // その他の文字: スキップ
    }
    return slug;
}

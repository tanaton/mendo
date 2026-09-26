#pragma once
#include "win_handle.h"
#include "string_convert.h"
#include <windows.h>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>

// win_handle.h と責務を分離するためにこのヘッダにまとめる。

class ClipboardSession {
public:
    explicit ClipboardSession(HWND hwnd) noexcept
        : open_(::OpenClipboard(hwnd) != FALSE)
    {
        if (open_) {
            ::EmptyClipboard();
        }
    }
    ~ClipboardSession()
    {
        if (open_) {
            ::CloseClipboard();
        }
    }
    ClipboardSession(const ClipboardSession&) = delete;
    ClipboardSession& operator=(const ClipboardSession&) = delete;

    explicit operator bool() const noexcept
    {
        return open_;
    }

private:
    bool open_;
};

// EmptyClipboard より前にバッファを確保できるよう、構築と SetClipboardData を分離する。
// 確保失敗時に既存クリップボードを破壊しないための土台 (WriteClipboardDiagram 参照)。
template <typename CharT>
inline UniqueGlobalMem BuildGlobalZeroTerminated(std::basic_string_view<CharT> text) noexcept
{
    // text.size() + 1 が SIZE_MAX/sizeof(CharT) を超えると bytes 計算がオーバーフローする。
    // GlobalAlloc は size_t 受けでも実用上数 GB が上限なので、UINT_MAX を実用上限とする。
    if (text.size() > std::numeric_limits<UINT>::max() / sizeof(CharT) - 1) {
        return {};
    }
    return AllocGlobalFilled((text.size() + 1) * sizeof(CharT), [text](void* p) noexcept {
        auto* dest = static_cast<CharT*>(p);
        std::char_traits<CharT>::copy(dest, text.data(), text.size());
        dest[text.size()] = CharT{};
        return true;
    });
}

inline bool CommitClipboardGlobal(UINT format, UniqueGlobalMem mem) noexcept
{
    if (format == 0 || !mem) {
        return false;
    }
    if (!SetClipboardData(format, mem.get())) {
        return false;
    }
    mem.release();
    return true;
}


// UTF-8 を GlobalAlloc 先へ直接 UTF-16 変換する。巨大な全選択コピーで中間 wstring
// (UTF-8 byte 数ぶんの上限確保) とそのコピーを持たないため。失敗時は空。
inline UniqueGlobalMem BuildGlobalWideFromUtf8(std::string_view utf8) noexcept
{
    if (utf8.empty() || utf8.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int src_len = static_cast<int>(utf8.size());
    const int wide_len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), src_len, nullptr, 0);
    if (wide_len <= 0) {
        return {};
    }
    return AllocGlobalFilled((static_cast<size_t>(wide_len) + 1) * sizeof(wchar_t), [&](void* p) noexcept {
        auto* dst = static_cast<wchar_t*>(p);
        const int written = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), src_len, dst, wide_len);
        dst[wide_len] = L'\0';
        return written == wide_len;
    });
}

// 変換に失敗したら EmptyClipboard で既存内容を破壊しないよう、確保成功後にセッションを開く。
inline void WriteClipboardText(HWND hwnd, std::string_view text_utf8) noexcept
{
    auto mem = BuildGlobalWideFromUtf8(text_utf8);
    if (!mem) {
        return;
    }
    ClipboardSession session(hwnd);
    if (!session) {
        return;
    }
    CommitClipboardGlobal(CF_UNICODETEXT, std::move(mem));
}

// HTML Format 仕様: https://learn.microsoft.com/windows/win32/dataxchg/html-clipboard-format
// ヘッダ内の各オフセットは UTF-8 バイト位置で 10 桁ゼロ埋め。
namespace cf_html_detail {

inline constexpr std::string_view kBeforeStartHtml = "Version:0.9\r\nStartHTML:";
inline constexpr std::string_view kBeforeEndHtml = "\r\nEndHTML:";
inline constexpr std::string_view kBeforeStartFragment = "\r\nStartFragment:";
inline constexpr std::string_view kBeforeEndFragment = "\r\nEndFragment:";
inline constexpr std::string_view kHeaderSuffix = "\r\n";
inline constexpr std::string_view kDigitPlaceholder = "0000000000";
inline constexpr std::string_view kHtmlPrefix = "<html>\r\n<body>\r\n<!--StartFragment-->";
inline constexpr std::string_view kHtmlSuffix = "<!--EndFragment-->\r\n</body>\r\n</html>";

inline constexpr size_t kStartHtmlDigits = kBeforeStartHtml.size();
inline constexpr size_t kEndHtmlDigits = kStartHtmlDigits + kDigitPlaceholder.size() + kBeforeEndHtml.size();
inline constexpr size_t kStartFragmentDigits = kEndHtmlDigits + kDigitPlaceholder.size() + kBeforeStartFragment.size();
inline constexpr size_t kEndFragmentDigits = kStartFragmentDigits + kDigitPlaceholder.size() + kBeforeEndFragment.size();
inline constexpr size_t kHeaderSize = kEndFragmentDigits + kDigitPlaceholder.size() + kHeaderSuffix.size();

} // namespace cf_html_detail

constexpr size_t CfHtmlPayloadSize(size_t fragment_size) noexcept
{
    using namespace cf_html_detail;
    return kHeaderSize + kHtmlPrefix.size() + fragment_size + kHtmlSuffix.size();
}

// dst に CfHtmlPayloadSize(fragment.size()) バイトの CF_HTML ペイロードを書く (NUL 終端は含まない)。
inline void WriteCfHtmlPayload(char* dst, std::string_view fragment_utf8) noexcept
{
    using namespace cf_html_detail;
    char* p = dst;
    const auto put = [&p](std::string_view s) noexcept {
        std::char_traits<char>::copy(p, s.data(), s.size());
        p += s.size();
    };
    put(kBeforeStartHtml);
    put(kDigitPlaceholder);
    put(kBeforeEndHtml);
    put(kDigitPlaceholder);
    put(kBeforeStartFragment);
    put(kDigitPlaceholder);
    put(kBeforeEndFragment);
    put(kDigitPlaceholder);
    put(kHeaderSuffix);
    const size_t start_html = static_cast<size_t>(p - dst);
    put(kHtmlPrefix);
    const size_t start_fragment = static_cast<size_t>(p - dst);
    put(fragment_utf8);
    const size_t end_fragment = static_cast<size_t>(p - dst);
    put(kHtmlSuffix);
    const size_t end_html = static_cast<size_t>(p - dst);

    const auto write_offset = [dst](size_t digit_offset, size_t value) noexcept {
        char buf[11];
        std::snprintf(buf, sizeof(buf), "%010zu", value);
        std::char_traits<char>::copy(dst + digit_offset, buf, 10);
    };
    write_offset(kStartHtmlDigits, start_html);
    write_offset(kEndHtmlDigits, end_html);
    write_offset(kStartFragmentDigits, start_fragment);
    write_offset(kEndFragmentDigits, end_fragment);
}


// CF_HTML ペイロードを GlobalAlloc 先に直接組み立てる (中間 std::string のコピーを持たない)。
inline UniqueGlobalMem BuildGlobalCfHtml(std::string_view fragment_utf8) noexcept
{
    const size_t size = CfHtmlPayloadSize(fragment_utf8.size());
    return AllocGlobalFilled(size + 1, [&](void* p) noexcept {
        auto* dst = static_cast<char*>(p);
        WriteCfHtmlPayload(dst, fragment_utf8);
        dst[size] = '\0';
        return true;
    });
}

// CF_DIB (32bpp トップダウン BGRA) のバイト数: BITMAPINFOHEADER + ピクセル列。
// 桁あふれ時は 0 を返す。色テーブルは 32bpp では不要。
inline size_t DibTotalBytes(UINT width, UINT height) noexcept
{
    if (width == 0 || height == 0) {
        return 0;
    }
    const size_t stride = static_cast<size_t>(width) * 4; // 32bpp は常に 4 バイト境界
    if (height > (std::numeric_limits<size_t>::max() - sizeof(BITMAPINFOHEADER)) / stride) {
        return 0;
    }
    return sizeof(BITMAPINFOHEADER) + stride * height;
}

// dst 先頭に CF_DIB 用 BITMAPINFOHEADER を書き込む。続くピクセル領域 (dst + sizeof(header))
// に呼び出し側がトップダウン 32bpp BGRA を width*4*height バイト書く。
// CF_DIB を載せれば Windows が CF_BITMAP / CF_DIBV5 を自動合成するため広く貼り付け可能。
inline void WriteDibHeader(void* dst, UINT width, UINT height) noexcept
{
    auto* bih = static_cast<BITMAPINFOHEADER*>(dst);
    *bih = {};
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = static_cast<LONG>(width);
    bih->biHeight = -static_cast<LONG>(height); // 負 = トップダウン
    bih->biPlanes = 1;
    bih->biBitCount = 32;
    bih->biCompression = BI_RGB;
    bih->biSizeImage = static_cast<DWORD>(static_cast<size_t>(width) * 4 * height);
}

// ダイアグラム (Mermaid/LaTeX) をクリップボードへコピーする統合書き込み。
// "image/svg+xml": Office 2016+ や Inkscape がベクタ画像として認識 (Mermaid のみ svg 非空)。
// CF_DIB:          画像。Paint / チャット / Office などへの貼り付け用。両ダイアグラム共通の基本形式。
// CF_UNICODETEXT:  SVG マークアップ原文のテキストフォールバック。
// 並び順 = 優先度。ベクタを優先する貼り付け先のため SVG を先頭に積む。
// 戻り値: 画像 (CF_DIB) が載れば true。画像は両ダイアグラムが約束する基本形式なので、
//         成功判定はこれで行う (svg/テキストはベストエフォートの付加形式)。
// WideToUtf8 が確保失敗で送出し得るため noexcept にはしない。
inline bool WriteClipboardDiagram(HWND hwnd, UniqueGlobalMem dib, std::wstring_view svg_text)
{
    // EmptyClipboard より前に各バッファを確保する (HTML/SVG 経路と同じ「揃ってから開く」方針)。
    UniqueGlobalMem svg_unicode;
    UniqueGlobalMem svg_utf8_mem;
    if (!svg_text.empty()) {
        svg_unicode = BuildGlobalZeroTerminated<wchar_t>(svg_text);
        const std::string svg_utf8 = string_convert::WideToUtf8(svg_text);
        if (!svg_utf8.empty()) {
            svg_utf8_mem = BuildGlobalZeroTerminated<char>(std::string_view(svg_utf8));
        }
    }
    if (!dib && !svg_unicode && !svg_utf8_mem) {
        return false;
    }

    ClipboardSession session(hwnd);
    if (!session) {
        return false;
    }

    if (svg_utf8_mem) {
        static const UINT cf_svg = RegisterClipboardFormatW(L"image/svg+xml");
        CommitClipboardGlobal(cf_svg, std::move(svg_utf8_mem));
    }
    bool dib_committed = false;
    if (dib) {
        dib_committed = CommitClipboardGlobal(CF_DIB, std::move(dib));
    }
    if (svg_unicode) {
        CommitClipboardGlobal(CF_UNICODETEXT, std::move(svg_unicode));
    }
    return dib_committed;
}

// plain_text_utf8: 書式付きに対応していないアプリ向けのフォールバック (UTF-8)。
inline void WriteClipboardHtml(HWND hwnd, std::string_view fragment_utf8, std::string_view plain_text_utf8) noexcept
{
    // EmptyClipboard で既存内容を破壊しないよう、ペイロードを揃えてからセッションを開く。
    UniqueGlobalMem html = fragment_utf8.empty() ? UniqueGlobalMem{} : BuildGlobalCfHtml(fragment_utf8);
    UniqueGlobalMem plain = BuildGlobalWideFromUtf8(plain_text_utf8);
    if (!html && !plain) {
        return;
    }
    ClipboardSession session(hwnd);
    if (!session) {
        return;
    }

    if (html) {
        static const UINT cf_html = RegisterClipboardFormatW(L"HTML Format");
        CommitClipboardGlobal(cf_html, std::move(html));
    }
    if (plain) {
        CommitClipboardGlobal(CF_UNICODETEXT, std::move(plain));
    }
}

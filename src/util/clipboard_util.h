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
    const size_t bytes = (text.size() + 1) * sizeof(CharT);
    UniqueGlobalMem hMem{ GlobalAlloc(GMEM_MOVEABLE, bytes) };
    if (!hMem) {
        return {};
    }
    auto* dest = static_cast<CharT*>(GlobalLock(hMem.get()));
    if (!dest) {
        return {};
    }
    std::char_traits<CharT>::copy(dest, text.data(), text.size());
    dest[text.size()] = CharT{};
    GlobalUnlock(hMem.get());
    return hMem;
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

template <typename CharT>
inline bool SetClipboardZeroTerminated(UINT format, std::basic_string_view<CharT> text) noexcept
{
    // format==0 と確保失敗の判定は CommitClipboardGlobal 側に集約する。
    return CommitClipboardGlobal(format, BuildGlobalZeroTerminated<CharT>(text));
}

// Utf8ToWide 失敗時に EmptyClipboard で既存内容を破壊しないよう、変換成功後にセッションを開く。
inline void WriteClipboardText(HWND hwnd, std::string_view text_utf8) noexcept
{
    std::pmr::wstring text_wide;
    string_convert::Utf8ToWide(text_utf8, text_wide);
    if (text_wide.empty()) {
        return;
    }
    ClipboardSession session(hwnd);
    if (!session) {
        return;
    }
    SetClipboardZeroTerminated<wchar_t>(CF_UNICODETEXT, text_wide);
}

// HTML Format 仕様: https://learn.microsoft.com/windows/win32/dataxchg/html-clipboard-format
// ヘッダ内の各オフセットは UTF-8 バイト位置で 10 桁ゼロ埋め。
inline std::string BuildCfHtmlPayload(std::string_view fragment_utf8)
{
    constexpr std::string_view kBeforeStartHtml = "Version:0.9\r\nStartHTML:";
    constexpr std::string_view kBeforeEndHtml = "\r\nEndHTML:";
    constexpr std::string_view kBeforeStartFragment = "\r\nStartFragment:";
    constexpr std::string_view kBeforeEndFragment = "\r\nEndFragment:";
    constexpr std::string_view kHeaderSuffix = "\r\n";
    constexpr std::string_view kDigitPlaceholder = "0000000000";
    constexpr std::string_view kHtmlPrefix = "<html>\r\n<body>\r\n<!--StartFragment-->";
    constexpr std::string_view kHtmlSuffix = "<!--EndFragment-->\r\n</body>\r\n</html>";

    constexpr size_t kStartHtmlDigits = kBeforeStartHtml.size();
    constexpr size_t kEndHtmlDigits = kStartHtmlDigits + kDigitPlaceholder.size() + kBeforeEndHtml.size();
    constexpr size_t kStartFragmentDigits = kEndHtmlDigits + kDigitPlaceholder.size() + kBeforeStartFragment.size();
    constexpr size_t kEndFragmentDigits = kStartFragmentDigits + kDigitPlaceholder.size() + kBeforeEndFragment.size();
    constexpr size_t kHeaderSize = kEndFragmentDigits + kDigitPlaceholder.size() + kHeaderSuffix.size();

    std::string payload;
    payload.reserve(kHeaderSize + kHtmlPrefix.size() + fragment_utf8.size() + kHtmlSuffix.size());
    payload.append(kBeforeStartHtml).append(kDigitPlaceholder);
    payload.append(kBeforeEndHtml).append(kDigitPlaceholder);
    payload.append(kBeforeStartFragment).append(kDigitPlaceholder);
    payload.append(kBeforeEndFragment).append(kDigitPlaceholder);
    payload.append(kHeaderSuffix);
    const size_t start_html = payload.size();
    payload.append(kHtmlPrefix);
    const size_t start_fragment = payload.size();
    payload.append(fragment_utf8);
    const size_t end_fragment = payload.size();
    payload.append(kHtmlSuffix);
    const size_t end_html = payload.size();

    auto write_offset = [&payload](size_t digit_offset, size_t value) noexcept {
        char buf[11];
        std::snprintf(buf, sizeof(buf), "%010zu", value);
        std::char_traits<char>::copy(payload.data() + digit_offset, buf, 10);
    };
    write_offset(kStartHtmlDigits, start_html);
    write_offset(kEndHtmlDigits, end_html);
    write_offset(kStartFragmentDigits, start_fragment);
    write_offset(kEndFragmentDigits, end_fragment);
    return payload;
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
    // EmptyClipboard で既存内容を破壊しないよう、ペイロードが 1 つも揃わなければセッションを開かない。
    std::pmr::wstring plain_wide;
    string_convert::Utf8ToWide(plain_text_utf8, plain_wide);
    if (fragment_utf8.empty() && plain_wide.empty()) {
        return;
    }
    ClipboardSession session(hwnd);
    if (!session) {
        return;
    }

    if (!fragment_utf8.empty()) {
        const std::string payload = BuildCfHtmlPayload(fragment_utf8);
        static const UINT cf_html = RegisterClipboardFormatW(L"HTML Format");
        SetClipboardZeroTerminated<char>(cf_html, payload);
    }

    if (!plain_wide.empty()) {
        SetClipboardZeroTerminated<wchar_t>(CF_UNICODETEXT, plain_wide);
    }
}

#pragma once
#include <windows.h>
#include <string_view>
#include <string>
#include <cstdio>
#include "string_convert.h"

// ポリシーベースの汎用 RAII リソースラッパー。
// Traits は type, invalid(), close(type) を定義する。
template <typename Traits>
class UniqueResource {
    using handle_t = typename Traits::type;

public:
    UniqueResource() noexcept = default;
    explicit UniqueResource(handle_t h) noexcept : handle_(h)
    {}
    ~UniqueResource()
    {
        reset();
    }

    UniqueResource(const UniqueResource&) = delete;
    UniqueResource& operator=(const UniqueResource&) = delete;
    UniqueResource(UniqueResource&& other) noexcept : handle_(other.release())
    {}
    UniqueResource& operator=(UniqueResource&& other) noexcept
    {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    explicit operator bool() const noexcept
    {
        return handle_ != Traits::invalid();
    }
    handle_t get() const noexcept
    {
        return handle_;
    }

    handle_t release() noexcept
    {
        handle_t h = handle_;
        handle_ = Traits::invalid();
        return h;
    }

    void reset(handle_t h = Traits::invalid()) noexcept
    {
        if (handle_ != Traits::invalid()) {
            Traits::close(handle_);
        }
        handle_ = h;
    }

private:
    handle_t handle_ = Traits::invalid();
};

// CloseHandle で解放、無効値 INVALID_HANDLE_VALUE（CreateFileW 等）
struct HandleTraits {
    using type = HANDLE;
    static type invalid() noexcept
    {
        return INVALID_HANDLE_VALUE;
    }
    static void close(type h) noexcept
    {
        CloseHandle(h);
    }
};
using UniqueHandle = UniqueResource<HandleTraits>;

// CloseHandle で解放、無効値 nullptr（CreateEventW 等）
struct EventHandleTraits {
    using type = HANDLE;
    static type invalid() noexcept
    {
        return nullptr;
    }
    static void close(type h) noexcept
    {
        CloseHandle(h);
    }
};
using UniqueEventHandle = UniqueResource<EventHandleTraits>;

// CloseHandle で解放、無効値 nullptr（CreateFileMappingW）
struct FileMappingTraits {
    using type = HANDLE;
    static type invalid() noexcept
    {
        return nullptr;
    }
    static void close(type h) noexcept
    {
        CloseHandle(h);
    }
};
using UniqueFileMapping = UniqueResource<FileMappingTraits>;

// UnmapViewOfFile で解放、無効値 nullptr（MapViewOfFile）
struct MapViewTraits {
    using type = LPVOID;
    static type invalid() noexcept
    {
        return nullptr;
    }
    static void close(type p) noexcept
    {
        UnmapViewOfFile(p);
    }
};
using UniqueMapView = UniqueResource<MapViewTraits>;

// FindClose で解放（FindFirstFileW）
struct FindHandleTraits {
    using type = HANDLE;
    static type invalid() noexcept
    {
        return INVALID_HANDLE_VALUE;
    }
    static void close(type h) noexcept
    {
        FindClose(h);
    }
};
using UniqueFindHandle = UniqueResource<FindHandleTraits>;

// GlobalFree で解放（GlobalAlloc）。
// SetClipboardData / CreateStreamOnHGlobal への所有権移譲時は release() を使う。
struct GlobalMemTraits {
    using type = HGLOBAL;
    static type invalid() noexcept
    {
        return nullptr;
    }
    static void close(type h) noexcept
    {
        GlobalFree(h);
    }
};
using UniqueGlobalMem = UniqueResource<GlobalMemTraits>;

// クリップボードを RAII で開閉するセッションガード。
// コンストラクタで OpenClipboard + EmptyClipboard を行い、
// デストラクタで CloseClipboard を呼ぶ。OpenClipboard が失敗した場合は
// `if (session)` が false になり、EmptyClipboard / CloseClipboard は呼ばない。
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

// 末尾 NUL 文字を付加した text を format 形式でクリップボードに登録する。
// CharT は char または wchar_t を想定。OpenClipboard / EmptyClipboard /
// CloseClipboard の呼び出しは呼び出し側の責務（ClipboardSession を使うと簡潔）。
// 戻り値: SetClipboardData まで成功したら true。
template <typename CharT>
inline bool SetClipboardZeroTerminated(UINT format, std::basic_string_view<CharT> text) noexcept
{
    if (format == 0) {
        return false;
    }
    const size_t bytes = (text.size() + 1) * sizeof(CharT);
    UniqueGlobalMem hMem{ GlobalAlloc(GMEM_MOVEABLE, bytes) };
    if (!hMem) {
        return false;
    }
    auto* dest = static_cast<CharT*>(GlobalLock(hMem.get()));
    if (!dest) {
        return false;
    }
    std::char_traits<CharT>::copy(dest, text.data(), text.size());
    dest[text.size()] = CharT{};
    GlobalUnlock(hMem.get());
    if (!SetClipboardData(format, hMem.get())) {
        return false;
    }
    hMem.release();
    return true;
}

// クリップボードにテキストを書き込む共通ユーティリティ。
// App::SetClipboardText と SideEffectExecutor の両方から使用される。
inline void WriteClipboardText(HWND hwnd, std::wstring_view text) noexcept
{
    if (text.empty()) {
        return;
    }
    ClipboardSession session(hwnd);
    if (!session) {
        return;
    }
    SetClipboardZeroTerminated<wchar_t>(CF_UNICODETEXT, text);
}

// CF_HTML 形式のクリップボード用ペイロードを構築する。
// HTML Format 仕様: https://learn.microsoft.com/windows/win32/dataxchg/html-clipboard-format
// fragment_utf8 は <!--StartFragment--> と <!--EndFragment--> の間に挟まれる UTF-8 HTML。
// ヘッダ内の各オフセットは UTF-8 バイト位置で 10 桁ゼロ埋め。
inline std::string BuildCfHtmlPayload(std::string_view fragment_utf8)
{
    constexpr std::string_view kBeforeStartHtml = "Version:0.9\r\nStartHTML:";
    constexpr std::string_view kBeforeEndHtml = "\r\nEndHTML:";
    constexpr std::string_view kBeforeStartFragment = "\r\nStartFragment:";
    constexpr std::string_view kBeforeEndFragment = "\r\nEndFragment:";
    constexpr std::string_view kHeaderSuffix = "\r\n";
    constexpr std::string_view kDigitPlaceholder = "0000000000";
    constexpr std::string_view kHtmlPrefix =
        "<html>\r\n<body>\r\n<!--StartFragment-->";
    constexpr std::string_view kHtmlSuffix =
        "<!--EndFragment-->\r\n</body>\r\n</html>";

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
        std::char_traits<char>::copy(&payload[digit_offset], buf, 10);
    };
    write_offset(kStartHtmlDigits, start_html);
    write_offset(kEndHtmlDigits, end_html);
    write_offset(kStartFragmentDigits, start_fragment);
    write_offset(kEndFragmentDigits, end_fragment);
    return payload;
}

// SVG をクリップボードに書き込む。
// "image/svg+xml": Office (Word/Excel/PowerPoint 2016+) の「形式を選択して貼り付け → SVG」
//                  および Inkscape などのベクタ編集アプリがベクタ画像として認識する。
// CF_UNICODETEXT:  テキストエディタへの貼り付けフォールバック（SVG マークアップ原文）。
// 戻り値: いずれか 1 つ以上のフォーマットが SetClipboardData まで成功したら true。
//        OpenClipboard / SetClipboardData が全滅したら false（呼び出し側で失敗トースト表示用）。
inline bool WriteClipboardSvg(HWND hwnd, std::wstring_view svg_text) noexcept
{
    if (svg_text.empty()) {
        return false;
    }
    ClipboardSession session(hwnd);
    if (!session) {
        return false;
    }

    bool any_set = false;

    const std::string svg_utf8 = string_convert::WideToUtf8(svg_text);
    if (!svg_utf8.empty()) {
        static const UINT cf_svg = RegisterClipboardFormatW(L"image/svg+xml");
        any_set |= SetClipboardZeroTerminated<char>(cf_svg, svg_utf8);
    }

    any_set |= SetClipboardZeroTerminated<wchar_t>(CF_UNICODETEXT, svg_text);

    return any_set;
}

// クリップボードに CF_HTML（書式付き）と CF_UNICODETEXT（プレーンテキスト）を同時に書き込む。
// fragment_html: <!--StartFragment--> と <!--EndFragment--> の間に入る HTML 断片（Wide 文字列）。
// plain_text:    書式付きに対応していないアプリ向けのフォールバック。
inline void WriteClipboardHtml(HWND hwnd, std::wstring_view fragment_html, std::wstring_view plain_text) noexcept
{
    if (fragment_html.empty() && plain_text.empty()) {
        return;
    }
    ClipboardSession session(hwnd);
    if (!session) {
        return;
    }

    if (!fragment_html.empty()) {
        const std::string fragment_utf8 = string_convert::WideToUtf8(fragment_html);
        const std::string payload = BuildCfHtmlPayload(fragment_utf8);
        const UINT cf_html = RegisterClipboardFormatW(L"HTML Format");
        SetClipboardZeroTerminated<char>(cf_html, payload);
    }

    if (!plain_text.empty()) {
        SetClipboardZeroTerminated<wchar_t>(CF_UNICODETEXT, plain_text);
    }
}

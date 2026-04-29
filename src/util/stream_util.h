#pragma once
#include "win_handle.h"
#include <wrl/client.h>
#include <objidl.h>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <vector>

// COM IStream 周りの共通ヘルパー。
// WIC デコード入力や WebView2 CapturePreview 出力などで使い回される。
namespace stream_util {

// IStream の先頭から全バイトを読み出す。サイズ検証と Read/Seek の HRESULT を確認し、
// 部分読込やサイズ不正は空ベクタを返す失敗扱いにする。
inline std::pmr::vector<uint8_t> ReadAllBytes(IStream* stream)
{
    if (!stream) {
        return {};
    }

    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) {
        return {};
    }

    const auto size64 = stat.cbSize.QuadPart;
    if (size64 <= 0 || static_cast<uint64_t>(size64) > std::numeric_limits<ULONG>::max()) {
        return {};
    }
    const auto size = static_cast<size_t>(size64);

    const LARGE_INTEGER zero{};
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) {
        return {};
    }

    std::pmr::vector<uint8_t> data(size);
    ULONG read = 0;
    const HRESULT hr = stream->Read(data.data(), static_cast<ULONG>(size), &read);
    if (FAILED(hr) || read != size) {
        return {};
    }
    return data;
}

// HGLOBAL 上のメモリストリームを作成し、与えられたバイト列を書き込んで先頭に巻き戻す。
// size == 0 は空ストリームを要求する正当なユースケース（WebView2 CapturePreview の
// 書き込み先バッファ用途）として許容するが、size > 0 && data == nullptr は呼び出し
// 側バグなので nullptr を返して失敗させる。
inline Microsoft::WRL::ComPtr<IStream> CreateMemoryStream(const void* data, size_t size)
{
    if (size > 0 && !data) {
        return nullptr;
    }
    if (size > std::numeric_limits<ULONG>::max()) {
        return nullptr;
    }

    Microsoft::WRL::ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) {
        return nullptr;
    }
    if (size > 0) {
        ULONG written = 0;
        if (FAILED(stream->Write(data, static_cast<ULONG>(size), &written)) || written != size) {
            return nullptr;
        }
        const LARGE_INTEGER zero{};
        if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) {
            return nullptr;
        }
    }
    return stream;
}

// ファイルハンドルから size バイトを HGLOBAL に直接読み込んで IStream を返す。
// 大きな画像ファイルで一時バッファへのコピーを避けるため、ReadFile を HGLOBAL にロックしたまま呼ぶ。
inline Microsoft::WRL::ComPtr<IStream> CreateMemoryStreamFromFile(HANDLE file, size_t size)
{
    if (!file || file == INVALID_HANDLE_VALUE || size == 0 || size > std::numeric_limits<ULONG>::max()) {
        return nullptr;
    }

    UniqueGlobalMem hMem{ GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(size)) };
    if (!hMem) {
        return nullptr;
    }

    void* ptr = GlobalLock(hMem.get());
    if (!ptr) {
        return nullptr;
    }
    DWORD bytes_read = 0;
    const BOOL ok = ReadFile(file, ptr, static_cast<DWORD>(size), &bytes_read, nullptr);
    GlobalUnlock(hMem.get());
    if (!ok || bytes_read != size) {
        return nullptr;
    }

    Microsoft::WRL::ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(hMem.get(), TRUE, &stream)) || !stream) {
        return nullptr;
    }
    hMem.release();
    return stream;
}

} // namespace stream_util

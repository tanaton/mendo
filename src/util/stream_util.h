#pragma once
#include "file_io.h"
#include "win_handle.h"
#include <wrl/client.h>
#include <objidl.h>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace stream_util {

inline bool SeekToBegin(IStream* stream) noexcept
{
    const LARGE_INTEGER zero{};
    return SUCCEEDED(stream->Seek(zero, STREAM_SEEK_SET, nullptr));
}

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
        if (FAILED(stream->Write(data, static_cast<ULONG>(size), &written)) || written != size || !SeekToBegin(stream.Get())) {
            return nullptr;
        }
    }
    return stream;
}

// 一時バッファへのコピーを避けるため、fill(void* dst) に HGLOBAL をロックしたまま直接書き込ませる。
template <class Fill>
Microsoft::WRL::ComPtr<IStream> CreateHGlobalStream(size_t size, Fill&& fill)
{
    if (size == 0 || size > std::numeric_limits<ULONG>::max()) {
        return nullptr;
    }

    UniqueGlobalMem hMem = AllocGlobalFilled(size, std::forward<Fill>(fill));
    if (!hMem) {
        return nullptr;
    }

    Microsoft::WRL::ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(hMem.get(), TRUE, &stream)) || !stream) {
        return nullptr;
    }
    hMem.release();
    return stream;
}

inline Microsoft::WRL::ComPtr<IStream> CreateMemoryStreamFromFile(HANDLE file, size_t size)
{
    if (!file || file == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    return CreateHGlobalStream(size, [&](void* dst) { return ReadExact(file, dst, size); });
}

} // namespace stream_util

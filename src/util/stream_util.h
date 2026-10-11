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

} // namespace stream_util

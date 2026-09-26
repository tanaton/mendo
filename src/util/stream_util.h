#pragma once
#include "file_io.h"
#include "win_handle.h"
#include <wrl/client.h>
#include <objidl.h>
#include <compressapi.h>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <span>
#include <utility>
#include <vector>

namespace stream_util {

inline std::pmr::vector<uint8_t> ReadStreamToEnd(IStream* stream)
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

struct DecompressorTraits {
    using type = DECOMPRESSOR_HANDLE;
    static type invalid() noexcept
    {
        return nullptr;
    }
    static void close(type h) noexcept
    {
        CloseDecompressor(h);
    }
};
using UniqueDecompressor = UniqueResource<DecompressorTraits>;

// Compression API のバッファモード (MSZIP) で圧縮されたデータをメモリに展開する。失敗時は空。
inline std::pmr::vector<uint8_t> DecompressMszip(std::span<const std::byte> compressed)
{
    if (compressed.empty()) {
        return {};
    }
    DECOMPRESSOR_HANDLE raw = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_MSZIP, nullptr, &raw)) {
        return {};
    }
    const UniqueDecompressor decompressor{ raw };
    SIZE_T size = 0;
    if (!Decompress(decompressor.get(), compressed.data(), compressed.size(), nullptr, 0, &size)
        && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return {};
    }
    std::pmr::vector<uint8_t> out(size);
    SIZE_T written = 0;
    if (!Decompress(decompressor.get(), compressed.data(), compressed.size(), out.data(), size, &written) || written != size) {
        return {};
    }
    return out;
}

// Compression API のバッファモード (MSZIP) で圧縮されたデータを展開する。
inline Microsoft::WRL::ComPtr<IStream> CreateMemoryStreamFromMszip(std::span<const std::byte> compressed)
{
    if (compressed.empty()) {
        return nullptr;
    }

    DECOMPRESSOR_HANDLE raw = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_MSZIP, nullptr, &raw)) {
        return nullptr;
    }
    const UniqueDecompressor decompressor{ raw };

    // 出力バッファ無しで呼ぶとヘッダに記録された展開後サイズが返る
    SIZE_T size = 0;
    if (!Decompress(decompressor.get(), compressed.data(), compressed.size(), nullptr, 0, &size)
        && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return nullptr;
    }

    return CreateHGlobalStream(size, [&](void* dst) {
        SIZE_T written = 0;
        return Decompress(decompressor.get(), compressed.data(), compressed.size(), dst, size, &written) && written == size;
    });
}

} // namespace stream_util

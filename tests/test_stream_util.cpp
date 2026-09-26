#include <gtest/gtest.h>
#include "stream_util.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace {

struct CompressorTraits {
    using type = COMPRESSOR_HANDLE;
    static type invalid() noexcept
    {
        return nullptr;
    }
    static void close(type h) noexcept
    {
        CloseCompressor(h);
    }
};

// cmake/mszip.ps1 と同じバッファモードで圧縮する
std::vector<std::byte> CompressMszip(std::span<const std::byte> src)
{
    COMPRESSOR_HANDLE raw = nullptr;
    if (!CreateCompressor(COMPRESS_ALGORITHM_MSZIP, nullptr, &raw)) {
        return {};
    }
    const UniqueResource<CompressorTraits> compressor{ raw };

    SIZE_T size = 0;
    Compress(compressor.get(), src.data(), src.size(), nullptr, 0, &size);
    std::vector<std::byte> dst(size);
    if (!Compress(compressor.get(), src.data(), src.size(), dst.data(), dst.size(), &size)) {
        return {};
    }
    dst.resize(size);
    return dst;
}

// MSZIP のブロック境界 (32KB) を跨ぐサイズにする
std::vector<std::byte> MakeSampleData()
{
    std::string text;
    for (int i = 0; i < 20000; i++) {
        text += "line " + std::to_string(i) + ": graph TD; A-->B;\n";
    }
    const auto bytes = std::as_bytes(std::span{ text });
    return { bytes.begin(), bytes.end() };
}

} // namespace

TEST(StreamUtilMszip, RoundTrip)
{
    const auto original = MakeSampleData();
    const auto compressed = CompressMszip(original);
    ASSERT_FALSE(compressed.empty());
    ASSERT_LT(compressed.size(), original.size());

    const auto restored = stream_util::DecompressMszip(compressed);
    EXPECT_TRUE(std::ranges::equal(std::as_bytes(std::span{ restored }), original));
}

TEST(StreamUtilMszip, EmptyInputReturnsEmpty)
{
    EXPECT_TRUE(stream_util::DecompressMszip({}).empty());
}

TEST(StreamUtilMszip, NonCompressedInputReturnsEmpty)
{
    const std::array<std::byte, 1024> data{};
    EXPECT_TRUE(stream_util::DecompressMszip(data).empty());
}

// ヘッダ上の展開後サイズは取得できるが本体が欠けているケース
TEST(StreamUtilMszip, TruncatedInputReturnsEmpty)
{
    const auto compressed = CompressMszip(MakeSampleData());
    ASSERT_GT(compressed.size(), 64u);
    EXPECT_TRUE(stream_util::DecompressMszip(std::span{ compressed }.first(compressed.size() / 2)).empty());
}

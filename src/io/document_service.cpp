#include "document_service.h"
#include "file_io.h"
#include "profiler.h"

namespace {

// 同期ロードの上限。これを超えるサイズはバックグラウンドスレッドにオフロードする。
// I/O + パースの典型コストが UI スレッドで <16ms に収まる経験値。
constexpr DWORD kAsyncLoadBytes = 64 * 1024;

// ローディングアニメーション (スピナー) を表示する下限。
// これより小さいファイルはアニメーション表示前に読み終わり、ちらつきの原因になる。
constexpr DWORD kLoadingAnimBytes = 16 * 1024 * 1024;

} // namespace

std::expected<Document, FileLoadError> DocumentService::LoadFile(const std::pmr::wstring& path, std::stop_token stop_token)
{
    // I/O 前にも check しないと、呼び出し時点で既にキャンセル済みでも無駄に同期 I/O を実行する。
    if (stop_token.stop_requested()) {
        return std::unexpected(FileLoadError::Cancelled);
    }
    auto result = FileLoader::LoadFile(path);
    if (!result) {
        return std::unexpected(result.error());
    }
    if (stop_token.stop_requested()) {
        return std::unexpected(FileLoadError::Cancelled);
    }
    MENDO_PROFILE("Document::FromMarkdown");
    auto doc = Document::FromMarkdown(std::move(result->text), result->byte_size, path, stop_token);
    if (stop_token.stop_requested()) {
        return std::unexpected(FileLoadError::Cancelled);
    }
    return doc;
}

// 仮想パスや存在しないパスは「大きい」扱いにする (非同期ロード経路で失敗を検出させるため)。
bool DocumentService::IsAsyncLoadCandidate(std::optional<uint64_t> size) noexcept
{
    return !size || *size > kAsyncLoadBytes;
}

bool DocumentService::ShouldShowLoadingAnimation(std::optional<uint64_t> size) noexcept
{
    return !size || *size > kLoadingAnimBytes;
}

bool DocumentService::IsAsyncLoadCandidate(const std::pmr::wstring& path) noexcept
{
    return IsAsyncLoadCandidate(QueryFileSize(path.c_str()));
}

bool DocumentService::ShouldShowLoadingAnimation(const std::pmr::wstring& path) noexcept
{
    return ShouldShowLoadingAnimation(QueryFileSize(path.c_str()));
}

#include "document_service.h"
#include "app_constants.h"
#include "file_io.h"
#include "file_loader.h"
#include "profiler.h"

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
bool DocumentService::IsLargerThan(const std::pmr::wstring& path, DWORD threshold) noexcept
{
    const auto size = QueryFileSize(path.c_str());
    return !size || *size > threshold;
}

bool DocumentService::IsAsyncLoadCandidate(const std::pmr::wstring& path) noexcept
{
    return IsLargerThan(path, app_threshold::ASYNC_LOAD_BYTES);
}

bool DocumentService::ShouldShowLoadingAnimation(const std::pmr::wstring& path) noexcept
{
    return IsLargerThan(path, app_threshold::LOADING_ANIM_BYTES);
}

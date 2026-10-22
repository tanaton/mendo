#include "file_load_service.h"
#include "file_io.h"
#include "profiler.h"
#include "theme.h"

void FileLoadService::StartAsyncLoad(TaskScheduler& scheduler, HWND hwnd, UINT msg_id, Theme theme, float content_width,
                                     std::shared_ptr<const std::pmr::string> reload_base)
{
    // 進行中の読み込み (preload を含む) は Start が世代を進めて置き換える。
    coordinator_.Start(scheduler, loading_path_, hwnd, msg_id, std::move(theme), content_width, std::move(reload_base));
}

void FileLoadService::StartPreloadAsync(TaskScheduler& scheduler, std::pmr::wstring path, Theme theme)
{
    loading_path_ = path;
    coordinator_.Start(scheduler, std::move(path), nullptr, 0, std::move(theme));
}

namespace {

// 同期ロードの上限。これを超えるサイズはバックグラウンドスレッドにオフロードする。
// I/O + パースの典型コストが UI スレッドで <16ms に収まる経験値。
constexpr DWORD kAsyncLoadBytes = 64 * 1024;

// ローディングアニメーション (スピナー) を表示する下限。
// これより小さいファイルはアニメーション表示前に読み終わり、ちらつきの原因になる。
constexpr DWORD kLoadingAnimBytes = 16 * 1024 * 1024;

} // namespace

std::expected<Document, FileLoadError> DocumentService::LoadFile(const std::pmr::wstring& path)
{
    auto result = FileLoader::LoadFile(path);
    if (!result) {
        return std::unexpected(result.error());
    }
    MENDO_PROFILE("Document::FromMarkdown");
    return Document::FromMarkdown(std::move(result->text), result->byte_size, path);
}

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

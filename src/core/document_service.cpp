#include "document_service.h"
#include "file_loader.h"
#include "profiler.h"

std::expected<Document, FileLoadError> DocumentService::LoadFile(const std::pmr::wstring& path)
{
    auto result = FileLoader::LoadFile(path);
    if (!result) {
        return std::unexpected(result.error());
    }
    MENDO_PROFILE("Document::FromMarkdown");
    return Document::FromMarkdown(std::move(result->text), result->byte_size, path);
}

void DocumentService::StartWatching(const std::pmr::wstring& path, FileWatcher::ChangeCallback cb)
{
    watcher_.StartWatching(path, std::move(cb));
}

void DocumentService::StopWatching() noexcept
{
    watcher_.StopWatching();
}

void DocumentService::CheckForChanges()
{
    watcher_.CheckForChanges();
}

void DocumentService::ResumeWatching()
{
    watcher_.ResumeWatching();
}

// 仮想パスや存在しないパスは「大きい」扱いにする (非同期ロード経路で失敗を検出させるため)。
bool DocumentService::IsLargerThan(const std::pmr::wstring& path, DWORD threshold) noexcept
{
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr) && attr.nFileSizeHigh == 0 && attr.nFileSizeLow <= threshold) {
        return false;
    }
    return true;
}

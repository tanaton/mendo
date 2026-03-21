#pragma once
#include "document.h"
#include "file_loader.h"

class DocumentService {
public:
    explicit DocumentService(FileLoader& loader) noexcept : loader_(loader) {}

    // ファイルを読み込み、Document を構築。成功時 true。
    bool LoadFile(std::wstring_view path, Document& doc);

    // 現在のファイルを再読み込み
    bool ReloadFile(Document& doc);

    // ファイル監視
    void StartWatching(std::wstring_view path, FileLoader::ChangeCallback cb);
    void StopWatching() noexcept;
    void CheckForChanges();

    // 大きいファイルかどうか（ローディングアニメ判定用）
    static bool NeedsLoadingAnimation(std::wstring_view path) noexcept;

private:
    FileLoader& loader_;
};

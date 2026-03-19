#pragma once
#include "document.h"
#include "file_loader.h"

class DocumentService {
public:
    explicit DocumentService(FileLoader& loader) : loader_(loader) {}

    // ファイルを読み込み、Document を構築。成功時 true。
    bool LoadFile(const std::wstring& path, Document& doc);

    // 現在のファイルを再読み込み
    bool ReloadFile(Document& doc);

    // ファイル監視
    void StartWatching(const std::wstring& path, FileLoader::ChangeCallback cb);
    void StopWatching();

    // 大きいファイルかどうか（ローディングアニメ判定用）
    static bool NeedsLoadingAnimation(const std::wstring& path);

private:
    FileLoader& loader_;
};

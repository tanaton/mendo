#pragma once
#include "document.h"
#include "file_loader.h"
#include <expected>
#include <stop_token>

class DocumentService {
public:
    static std::expected<Document, FileLoadError> LoadFile(const std::pmr::wstring& path);
    static std::expected<Document, FileLoadError> LoadFile(const std::pmr::wstring& path, std::stop_token stop_token);

    // 非同期ロード経路に乗せる対象か。app_threshold::ASYNC_LOAD_BYTES を超えるサイズ。
    // ファイル取得失敗時 (アクセス不能/仮想パス等) も true を返し、async 経路で reject させる。
    static bool IsAsyncLoadCandidate(const std::pmr::wstring& path) noexcept;
    // ローディングスピナーを表示すべきサイズ (LOADING_ANIM_BYTES 超過)。
    static bool ShouldShowLoadingAnimation(const std::pmr::wstring& path) noexcept;

private:
    // しきい値定数は app_constants.h::app_threshold。両 public API の共通実装。
    static bool IsLargerThan(const std::pmr::wstring& path, DWORD threshold) noexcept;
};

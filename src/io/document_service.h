#pragma once
#include "document.h"
#include "file_loader.h"
#include <cstdint>
#include <expected>
#include <optional>

class DocumentService {
public:
    static std::expected<Document, FileLoadError> LoadFile(const std::pmr::wstring& path);

    // 非同期ロード経路に乗せる対象か (同期ロードの上限を超えるサイズ)。
    // ファイル取得失敗時 (アクセス不能/仮想パス等) も true を返し、async 経路で reject させる。
    static bool IsAsyncLoadCandidate(const std::pmr::wstring& path) noexcept;
    // ローディングスピナーを表示すべきサイズか。
    static bool ShouldShowLoadingAnimation(const std::pmr::wstring& path) noexcept;
    // size は QueryFileSize の結果。取得失敗 (nullopt) は上限超え扱い。
    static bool IsAsyncLoadCandidate(std::optional<uint64_t> size) noexcept;
    static bool ShouldShowLoadingAnimation(std::optional<uint64_t> size) noexcept;
};

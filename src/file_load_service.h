#pragma once
#include "document_service.h"
#include "document.h"
#include "layout_cache.h"
#include <string>

// ファイル読み込みのオーケストレーションとローディングアニメーション状態を管理。
// Win32非依存 — 完全にテスト可能。
class FileLoadService {
public:
    explicit constexpr FileLoadService(DocumentService& doc_service) noexcept : doc_service_(doc_service) {}

    // ---- ローディングアニメーション状態 ----

    constexpr bool IsLoading() const noexcept { return loading_; }
    constexpr float GetLoadingAngle() const noexcept { return loading_angle_; }

    // ファイルのローディングアニメーションを開始。
    void StartLoading(std::wstring_view path);

    // ローディングアニメーションを停止。
    void StopLoading() noexcept;

    // ローディングアニメーションを1フレーム進める。
    void TickLoadingAnimation() noexcept;

    // ---- ファイル読み込み ----

    // 保存されたローディングパスを使ってファイル読み込みを実行。
    // 成功時にtrueを返す。呼び出し元はdocからディレクトリ/ファイルパスを参照すること。
    bool ExecuteLoad(Document& doc, LayoutCache& cache);

    // 現在のファイルをリロード。
    bool ExecuteReload(Document& doc, LayoutCache& cache);

    // ---- パスアクセス ----

    constexpr std::wstring_view GetLoadingPath() const noexcept { return loading_path_; }
    constexpr void SetLoadingPath(std::wstring_view path) { loading_path_ = path; }

private:
    DocumentService& doc_service_;
    bool loading_ = false;
    float loading_angle_ = 0.0f;
    std::pmr::wstring loading_path_;
};

#pragma once
#include "async_load_coordinator.h"
#include "async_load_result.h"
#include "document_service.h"
#include "document.h"
#include "file_loader.h"
#include "layout_cache.h"
#include "loading_animation.h"
#include "preloader.h"
#include "task_scheduler.h"
#include <string>
#include <optional>
#include <expected>

struct Theme;

// ファイル読み込みのオーケストレーション。
// 内部に LoadingAnimation / Preloader / AsyncLoadCoordinator を持ち、
// UI から見える API はそれらを束ねた薄い facade。preload と StartAsyncLoad の
// 結果ストレージは互いに独立で、TakeAsyncResult/TakeAsyncError が両者を順に確認する。
class FileLoadService {
public:
    explicit FileLoadService(DocumentService& doc_service) noexcept : doc_service_(doc_service)
    {}

    // ---- ローディングアニメーション状態 ----

    constexpr bool IsLoading() const noexcept
    {
        return animation_.IsActive();
    }
    // スピナー非表示でも非同期パースが進行中なら true。ライブリロードのバースト時に
    // 重複スケジューリングを抑制するためのフラグ。preload 経路は preloader_ が状態を持つ。
    bool IsAsyncLoading() const noexcept
    {
        return coordinator_.IsActive() || preloader_.IsActive();
    }
    constexpr float GetLoadingAngle() const noexcept
    {
        return animation_.GetAngle();
    }

    void StartLoading(std::pmr::wstring path);
    // 既に SetLoadingPath 済み (preload 経由など) の状態でアニメーションだけ起動する。
    void BeginLoadingAnimation() noexcept
    {
        animation_.Begin();
    }
    void StopLoading() noexcept;
    void TickLoadingAnimation() noexcept
    {
        animation_.Tick();
    }

    // ---- ファイル読み込み ----

    std::expected<void, FileLoadError> ExecuteLoad(Document& doc, LayoutCache& cache);

    // ---- 非同期ファイル読み込み ----

    void StartAsyncLoad(TaskScheduler& scheduler, HWND hwnd, UINT msg_id, const Theme& theme);
    // preload / async どちらの完了結果でも取り出す (preload 優先)。
    std::optional<AsyncLoadResult> TakeAsyncResult();
    // 直近の非同期ロードが返したエラー (失敗時のみ有効)。
    // OnParseComplete の null パスで取り出してトースト表示に使う。
    std::optional<FileLoadError> TakeAsyncError() noexcept;
    void CancelAsyncLoad() noexcept
    {
        coordinator_.Cancel();
    }

    // ---- 起動時 preload (App::Init 前から走らせる経路) ----

    // hwnd なしで I/O + パースをワーカースレッドに投入する。
    void StartPreloadAsync(std::pmr::wstring path);

    using PreloadAttachResult = Preloader::AttachResult;

    // App::Init 末尾で呼ぶ。preload の状態に応じて以下のいずれかを行う:
    //   完了済み: Join (= AppliedSync)。呼び出し側で OnParseComplete を発火。
    //   未完了:   worker に hwnd を渡して PostMessage を解禁 (= AttachedAsync)。
    PreloadAttachResult AttachOrApplyPreload(HWND hwnd, UINT msg_id)
    {
        return preloader_.AttachOrApply(hwnd, msg_id);
    }

    // ---- パスアクセス ----

    constexpr const std::pmr::wstring& GetLoadingPath() const noexcept
    {
        return loading_path_;
    }
    constexpr void SetLoadingPath(std::pmr::wstring path) noexcept
    {
        loading_path_ = std::move(path);
    }

private:
    DocumentService& doc_service_;
    LoadingAnimation animation_;
    std::pmr::wstring loading_path_;
    AsyncLoadCoordinator coordinator_;
    Preloader preloader_;
};

#pragma once
#include "layout_cache.h"
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <windows.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

using Microsoft::WRL::ComPtr;

class ImageLoader {
public:
    using Callback = void(*)(void*);

    ~ImageLoader();

    void Init(ID2D1RenderTarget* rt);
    void SetRenderTarget(ID2D1RenderTarget* rt) noexcept { render_target_ = rt; }

    // 非同期ワーカースレッドを起動する。hwnd に msg_id メッセージがポストされる。
    void InitAsync(HWND hwnd, UINT msg_id);

    // 画像ファイルを同期的に読み込む（テスト用に残す）。
    bool LoadImage(const std::wstring& abs_path, DiagramEntry& out);

    // キャッシュのみを参照する。I/O は行わない。
    bool GetCachedImage(const std::wstring& abs_path, DiagramEntry& out) const;

    // 非同期読み込みをキューに追加する。重複パスは無視される。
    void RequestLoadAsync(const std::wstring& abs_path,
        Callback on_complete, void* user_data);

    // UI スレッドから呼び出す: 完了した WIC デコード結果を D2D ビットマップに変換し、
    // キャッシュに格納する。バッチ内の最後の結果のコールバックのみを1回発火する
    // （呼び出し側が全ノードを再スキャンする設計のため）。
    void ProcessCompletedDecodes();

    // 保留中のリクエストと完了結果をすべて破棄する。
    void CancelPending();

    void ClearCache() noexcept { cache_.clear(); }

    // ワーカースレッドを停止し、join する。
    void Shutdown();

private:
    struct CachedImage {
        ComPtr<ID2D1Bitmap> bitmap;
        float width = 0.0f;
        float height = 0.0f;
    };

    struct PendingRequest {
        std::wstring path;
        Callback on_complete = nullptr;
        void* user_data = nullptr;
    };

    struct DecodeResult {
        std::wstring path;
        ComPtr<IWICFormatConverter> converter;
        float width = 0.0f;
        float height = 0.0f;
        Callback on_complete = nullptr;
        void* user_data = nullptr;
        bool success = false;
    };

    void WorkerLoop();

    ComPtr<IWICImagingFactory> wic_factory_;
    ID2D1RenderTarget* render_target_ = nullptr;
    std::unordered_map<std::wstring, CachedImage> cache_;

    // 非同期ワーカー
    HWND hwnd_ = nullptr;
    UINT msg_id_ = 0;
    std::thread worker_thread_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<PendingRequest> request_queue_;
    std::unordered_set<std::wstring> pending_paths_;
    std::atomic<bool> shutdown_flag_{ false };

    std::mutex result_mutex_;
    std::vector<DecodeResult> completed_;
};

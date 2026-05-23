#pragma once
#include "async_load_result.h"
#include "file_loader.h"
#include <windows.h>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

// 起動時にウィンドウ生成と並列で I/O + パースを走らせる専用クラス。
// hwnd が解禁されるまで worker は cv.wait で待機し、AttachOrApply() の通知で
// PostMessage を発火して通常の async 完了経路に合流する。
//
// FileLoadService の StartAsyncLoad とはストレージを共有しない。preload 完了結果は
// 内部の sink に保持し、TakeResult/TakeError で取り出す。
class Preloader {
public:
    Preloader() = default;
    ~Preloader();

    Preloader(const Preloader&) = delete;
    Preloader& operator=(const Preloader&) = delete;

    // Theme 不在のため EstimateNodeHeights をスキップ (heights_estimated=false)。
    void Start(std::pmr::wstring path);

    enum class AttachResult {
        None,           // preload 未起動 → 呼び出し側は何もしない
        AppliedSync,    // 同期取り込み完了 → 呼び出し側は OnParseComplete 相当を実行
        AttachedAsync,  // worker に hwnd 通知済み → 呼び出し側は必要なら anim を起動
    };

    // App::Init 末尾で呼ぶ。preload の状態に応じて以下のいずれかを行う:
    //   完了済み: Join (= AppliedSync)。呼び出し側で OnParseComplete を発火。
    //   未完了:   worker に hwnd を渡して PostMessage を解禁 (= AttachedAsync)。
    AttachResult AttachOrApply(HWND hwnd, UINT msg_id);

    bool IsActive() const noexcept
    {
        return ctx_ != nullptr;
    }

    std::optional<AsyncLoadResult> TakeResult();
    std::optional<FileLoadError> TakeError();

private:
    // worker は cv.wait で hwnd セットを待ってから PostMessage する。
    // main 側の早期終了時は aborted=true + cv.notify_all でデッドロックを避ける。
    struct Context {
        std::mutex mtx;
        std::condition_variable cv;
        HWND hwnd = nullptr;
        UINT msg_id = 0;
        bool aborted = false;

        void SignalAbort();
    };

    bool IsDoneLocked() const;
    void Join();
    // Take{Result,Error} 共通の末尾。`taken` が true なら worker は PostMessage 直後に
    // return しているので即 join できる。ctx_ も解放してまとめて IsActive() を false にする。
    void FinalizeIfDrained(bool taken);
    template <class Opt>
    Opt TakeFromSink(Opt& sink);

    // 宣言順: thread_ を ctx_ より前に置く。reverse-destruction で
    // ctx_ → thread_ (join) → result_/error_ の mutex の順に破壊され、
    // worker が result_/error_ を触るのは join 完了より前に保証される。
    std::jthread thread_;
    std::shared_ptr<Context> ctx_;

    mutable std::mutex sink_mutex_;
    std::optional<AsyncLoadResult> result_;
    std::optional<FileLoadError> error_;
};

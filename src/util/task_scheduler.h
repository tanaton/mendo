#pragma once
#include <algorithm>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>

// Mermaid / 画像デコード等で共有する scheduler のワーカー数。コア数の半分を 2〜4 に収める。
constexpr int SharedWorkerCount(unsigned int cores) noexcept
{
    return std::clamp(static_cast<int>(cores) / 2, 2, 4);
}

// レイアウト計測用 scheduler のワーカー数。
// 上限 8: ワーカーごとの ISOLATED ファクトリで本数なりに速くなるが (計測値は DWriteTextMeasurer)、ファクトリ 1 つで約 0.4MB 増える。
// 16 本は test.md 表示時の commit を約 11% 増やし、ふだんの計測は可視範囲の数十ノードで 8 本でも余るので上げない。
// 下限 2 は hardware_concurrency()==0 や 1 コア環境でも並列計測の枠組みを保つため。
constexpr int LayoutWorkerCount(unsigned int cores) noexcept
{
    constexpr unsigned kMinLayoutWorkers = 2u;
    constexpr unsigned kMaxLayoutWorkers = 8u;
    return static_cast<int>(std::clamp<unsigned>(
        cores > 0 ? cores - 1 : kMinLayoutWorkers, kMinLayoutWorkers, kMaxLayoutWorkers));
}

// ワーカーは COM を初期化せずに起動する。COM を使うタスクは EnsureWorkerCom を呼ぶ。
// Post() はスレッドセーフ。Shutdown() はUIスレッドから呼び出す。
// ワーカーは初回 Post まで起動しない。小さい文書だけを開くセッションでスレッドを作らずに済む。
class TaskScheduler {
public:
    // Shutdown した後は使い直せない。
    explicit TaskScheduler(int thread_count) noexcept;
    ~TaskScheduler();

    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;

    // Shutdown 後だけ false を返し、タスクは実行されない。呼び出し側は結果を待つ者のいない終了時のこととして無視してよい。
    // キューに上限を置かない。数式の結果などタスクごとに 1 回返す約束があり、捨てると待つ側が固まる。
    bool Post(std::move_only_function<void()> task)
    {
        return PostImpl(std::move(task), /*front=*/false);
    }
    // 待っているタスクより先に実行させる。
    void PostFront(std::move_only_function<void()> task)
    {
        PostImpl(std::move(task), /*front=*/true);
    }

    void Shutdown();

    // ワーカー上ならそのスレッドを一度だけ MTA に入れる。ワーカー以外では何もしない。
    // WIC など COM を使う処理の直前で呼ぶ。
    // 起動時に全ワーカーで初期化すると、プロセス初回の COM 初期化 (約 1.6ms) を
    // COM を使わない preload のパースまで待たされる。
    static void EnsureWorkerCom() noexcept;

    // 起動前でも指定した数を返す (ParallelFor の分割数に使うため)。Shutdown 後は 0。
    size_t WorkerCount() const noexcept
    {
        return thread_count_;
    }

private:
    bool PostImpl(std::move_only_function<void()> task, bool front);
    // mutex_ を保持して呼ぶ。
    void StartWorkersLocked();
    void WorkerLoop();

    size_t thread_count_;
    std::vector<std::thread> workers_;
    std::deque<std::move_only_function<void()>> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    // 読み書きはすべて mutex_ の下で行う。
    bool shutdown_ = false;
};

// count が min_count 以上なら obj を worker で破棄する。投函できなければ (Shutdown 後・確保の失敗) ここで破棄する。
// 渡すものは D2D の参照を持たないこと (確保元のヒープはスレッド安全)。
template <class T>
void DisposeOnWorkerIfLarge(TaskScheduler& scheduler, size_t count, size_t min_count, T&& obj) noexcept
{
    if (count < min_count) {
        return;
    }
    try {
        scheduler.Post([o = std::forward<T>(obj)] {});
    } catch (...) {
    }
}

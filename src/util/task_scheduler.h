#pragma once
#include <algorithm>
#include <vector>
#include <deque>
#include <memory_resource>
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
// Post() はスレッドセーフ。Init() / Shutdown() はUIスレッドから呼び出す。
// ワーカーは初回 Post まで起動しない。小さい文書だけを開くセッションでスレッドを作らずに済む。
class TaskScheduler {
public:
    // Why: 異常系（巨大ドキュメントの大量画像/Mermaid 要求）でキューが青天井に
    // 膨らむのを防ぎ、move_only_function キャプチャによるヒープ消費を制限する。
    static constexpr size_t MAX_PENDING_TASKS = 1024;

    TaskScheduler() = default;
    ~TaskScheduler();

    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;

    void Init(int thread_count);

    // 呼び出し側は戻り値を無視してもよい（破棄されたタスクは単に未実行）。
    bool Post(std::move_only_function<void()> task)
    {
        return PostImpl(std::move(task), /*front=*/false);
    }
    // 待っているタスクより先に実行させる。上限 (MAX_PENDING_TASKS) は見ない。Shutdown 後だけ false。
    bool PostFront(std::move_only_function<void()> task)
    {
        return PostImpl(std::move(task), /*front=*/true);
    }

    void Shutdown();

    // ワーカー上ならそのスレッドを一度だけ MTA に入れる。ワーカー以外では何もしない。
    // WIC など COM を使う処理の直前で呼ぶ。
    // 起動時に全ワーカーで初期化すると、プロセス初回の COM 初期化 (約 1.6ms) を
    // COM を使わない preload のパースまで待たされる。
    static void EnsureWorkerCom() noexcept;

    // 起動前でも Init で指定した数を返す (ParallelFor の分割数に使うため)。Init 前 / Shutdown 後は 0。
    size_t WorkerCount() const noexcept
    {
        return thread_count_;
    }

private:
    bool PostImpl(std::move_only_function<void()> task, bool front);
    // mutex_ を保持して呼ぶ。
    void StartWorkersLocked();
    void WorkerLoop();

    size_t thread_count_ = 0;
    std::vector<std::thread> workers_;
    std::pmr::deque<std::move_only_function<void()>> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    // 読み書きはすべて mutex_ の下で行う。
    bool shutdown_ = false;
};

#pragma once
#include "win_handle.h"
#include "worker_latch.h"
#include <functional>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

class TaskScheduler;

// ReadDirectoryChangesW の通知列に監視対象 (name、または空でなければ 8.3 短縮名 short_name。大小無視) の
// 更新 (削除・改名元以外) が含まれるか。空の buf はカーネルが内容を捨てたバッファ溢れなので取りこぼし回避で true。
// 切り詰め・破損したエントリに当たったらそこで打ち切り、buf の外は読まない。
[[nodiscard]] bool NotifyBufferHasTargetChange(std::span<const std::byte> buf, std::wstring_view name, std::wstring_view short_name) noexcept;

// 親フォルダと、その上で発行した ReadDirectoryChangesW の読み取り。カーネルが overlapped と buf に書くので動かさず、
// ヒープに置いて持ち回る。読み取りを発行したまま壊すと、取り消して書き終わるのを待つ。
struct DirectoryChangeRead {
    DirectoryChangeRead() = default;
    ~DirectoryChangeRead();
    DirectoryChangeRead(const DirectoryChangeRead&) = delete;
    DirectoryChangeRead& operator=(const DirectoryChangeRead&) = delete;

    // 次の読み取りを発行する。発行できなければ false。
    bool Begin() noexcept;
    // 発行した読み取りの結果を待たずに確かめる。Ready なら届いた通知列を notified に入れる (次の Begin まで有効。空なら
    // バッファ溢れ)。Failed と Ready の後は読み取りが発行されていないので、続けるなら Begin を呼ぶ。
    enum class Poll { Waiting, Failed, Ready };
    Poll Take(std::span<const std::byte>& notified) noexcept;

    UniqueHandle dir;
    UniqueNullHandle event;
    OVERLAPPED overlapped{};
    // 溢れると貯まった通知が捨てられ (0 バイトで返り、変更として扱う)、無関係な変更の多いフォルダで読み直しが増える。
    // ネットワーク越しで許される上限の 64KB にする。
    static constexpr size_t BUF_SIZE = 64 * 1024;
    alignas(DWORD) char buf[BUF_SIZE]{};
    bool pending = false;
};

// 監視の対象。ReadDirectoryChangesW は最初に呼んだ後の変更を貯めるので、読み込みの worker がファイルを読む前に作り
// (OpenWatchTarget)、UI スレッドの StartWatching に渡す。読んでから監視を始めるまでの保存も、ほかの変更と同じく通知で届く。
// 短縮名の問い合わせと親フォルダを開くことは、ネットワーク上のファイルではパスの要素ごとに往復が入る
// (ループバックの SMB でもローカルの約 5 倍、約 1ms) ので、これも worker で済ませる。
struct FileWatchTarget {
    std::wstring filename;
    // 通知は 8.3 短縮名で来ることがある (未規定)。長い名前と一致しない場合のみ持つ。
    std::wstring short_filename;
    // 親フォルダを開けないか読み取りを発行できなければ null で、監視しない。
    std::unique_ptr<DirectoryChangeRead> read;
};
[[nodiscard]] FileWatchTarget OpenWatchTarget(const std::wstring& file_path);

class FileWatcher {
public:
    // disposer があれば、やめた監視の読み取りをその worker で取り消して閉じる。取り消しは読み取りが終わるのを待ち、
    // フォルダを閉じるのもネットワーク越しでは往復が入るので、UI スレッドですると切断されたドライブで固まる。
    // 無いか投函できなければ (Shutdown 後) 呼んだスレッドで閉じる。
    explicit FileWatcher(TaskScheduler* disposer = nullptr) noexcept : disposer_(disposer)
    {}
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    // 変更のたびに呼ぶ。読み直しの最中かどうかは呼ばれた側 (ReloadScheduler) が見る。
    using ChangeCallback = std::move_only_function<void()>;
    // 今の監視をやめ、target を監視する。target.read が無ければ監視しない。
    void StartWatching(FileWatchTarget target, ChangeCallback callback);
    void StopWatching() noexcept;
    void CheckForChanges();

    HANDLE GetEventHandle() const noexcept
    {
        return target_.read && target_.read->pending ? target_.read->event.get() : nullptr;
    }

private:
    FileWatchTarget target_;
    ChangeCallback on_change_;
    TaskScheduler* disposer_ = nullptr;
};

// 本文中の画像のファイルの監視。画像のあるフォルダごとに読み取りを 1 つ発行し、通知の名前を登録した画像と照らす。
// 更新時刻を定期的に見る形は、画像の枚数ぶんの問い合わせがネットワーク上で往復になるので採らなかった。
// 文書のフォルダは FileWatcher も監視しているが、読み取りは共用しない (対象の差し替えの時期が違う。増えるのはハンドルと
// バッファ 64KB)。
//
// フォルダを開く・短縮名を長い名前に直す・読み取りを取り消して閉じる、はどれも worker で行い、UI スレッドではファイルに
// 触れない (文書の監視と同じ。切断されたネットワークドライブの画像を参照する文書で、フォルダごとに数秒〜数十秒
// ウィンドウが固まる)。Watch・Clear・EventHandles・CheckForChanges・IsOpening は UI スレッドから呼ぶ。
class ImageFolderWatcher {
public:
    // 監視するフォルダの数の上限。待つハンドルはメッセージループの MsgWaitForMultipleObjects に足すので増やしすぎない。
    // 超えた分のフォルダの画像は、保持範囲を出て戻るまで更新が出ない (監視が無かった頃と同じ)。
    static constexpr size_t kMaxFolders = 8;
    // EventHandles が返すイベントの数の上限。フォルダごとの読み取りと、worker の結果が届いたことを知らせる 1 つ。
    static constexpr size_t kMaxEvents = kMaxFolders + 1;

    // abs_path の画像を監視に足す。初めてのフォルダなら scheduler の worker で開いて読み取りを発行する (フォルダごとに 1 回)。
    // 開き終えるまで IsOpening が真になる。その間は画像を読まずに待てば、読んでから後の更新を取りこぼさない。
    // まだ無いファイルも、置かれたら知らせる。scheduler が無ければ監視しない。
    void Watch(const std::wstring& abs_path, TaskScheduler* scheduler);
    // abs_path のフォルダを開いている最中か。開き終えたら (開けなくても) CheckForChanges が true を返す。
    bool IsOpening(std::wstring_view abs_path) const noexcept;
    // 監視を全部やめる。開いている最中のフォルダは、開き終えた読み取りを受け取らずに捨てる。
    void Clear() noexcept;
    // 待つイベントを out に入れ、その数を返す。out は kMaxEvents 以上。
    size_t EventHandles(std::span<HANDLE> out) const noexcept;
    // 届いた通知を調べ、更新された画像 (Watch に渡したパス) を changed に足す。同じパスが重なる (1 回の保存で通知が続く)。
    // 通知が溢れたフォルダは、登録した画像を全部足す。開き終えたフォルダがあれば true (待たせていた画像を読ませる)。
    bool CheckForChanges(std::vector<std::wstring>& changed);

private:
    struct Folder {
        // worker の結果の宛先。Clear の後に届いた結果を、同じフォルダを開き直したものと取り違えない。
        uint64_t id = 0;
        std::wstring dir;
        bool opening = false;
        // 開けなかったフォルダは null のまま残し、画像のたびに開き直さない。
        std::unique_ptr<DirectoryChangeRead> read;
        std::vector<std::wstring> paths;
    };
    // worker の結果。opened ならフォルダを開いた結果 (read は開けなければ null)、そうでなければ短縮名を直した長い名前
    // (直せなければ空)。
    struct WorkerResult {
        uint64_t folder_id = 0;
        bool opened = false;
        std::unique_ptr<DirectoryChangeRead> read;
        std::wstring long_name;
    };
    // worker が結果を置く受け箱。worker のタスクが共有して持つので、監視より長生きできる。
    // 知らせは投函ではなくイベントにする (フォルダの通知と同じ待ちに載せる)。
    struct Inbox {
        Inbox();
        void Put(WorkerResult result);
        std::vector<WorkerResult> Take();

        PostedInbox<WorkerResult> results;
        // 手動リセット。空の受け箱に 1 件目が入ったら立ち、取り出す前に倒す。
        UniqueNullHandle event;
    };

    // task を worker で走らせる。結果は必ず 1 つ置かせる。投函できなければ false。
    bool PostWork(std::move_only_function<void(Inbox&)> task);
    // 読み取りを発行したままの解放は、取り消して書き終わるのを待つので worker でする。
    template <class T>
    void DisposeOnWorker(T obj) noexcept;
    // folder の画像のうち、ファイル名が name のものを changed に足す。あれば true。
    static bool AddNamed(const Folder& folder, std::wstring_view name, std::vector<std::wstring>& changed);

    std::vector<Folder> folders_;
    std::shared_ptr<Inbox> inbox_;
    TaskScheduler* scheduler_ = nullptr;
    uint64_t next_folder_id_ = 1;
    // 投函して、まだ結果を受け取っていないタスクの数。
    size_t awaited_results_ = 0;
};

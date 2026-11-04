#pragma once
#include "utility.h"
#include "worker_latch.h"
#include <cstdint>
#include <mutex>
#include <optional>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>

class TaskScheduler;

// Mermaidダイアグラムの描画命令 (UTF-8) をファイルに永続化するキャッシュ。
// 幅は mermaid_util::QuantizeWidth で量子化し、コード・テーマの組み合わせごとに保持する。
// 書き出しはTaskSchedulerを介してバックグラウンドスレッドで非同期に行う。
// インデックスはメモリ上で管理し、SaveIndex()でディスクに書き出す。複数のウィンドウ (プロセス) で同じフォルダを使うので、
// 書く直前に読み直して、他のプロセスが先に書いた索引にこのプロセスの変更を重ねる。
// すべてのパブリックメソッドはUIスレッドから呼び出す必要がある
// （StoreAsyncのタスク投入のみスレッドセーフ）。
class MermaidFileCache {
public:
    // index.bin の先頭。
    static constexpr uint32_t MAGIC = 0x4D454D43u; // "MEMC"
    // 索引の書式か図の描き方を変えたら上げる。違う版のキャッシュはフォルダごと捨てる。
    // 2: 1 つずつ描いた図と数式も、WebView2 が 1px 広く撮った分を切り落とす。表示の大きさは切り出した画素数から決める。
    //    ヘッダの読まれない dpr を消す。
    // 3: 図を mermaid.js から pele に、数式を mermaid 同梱の KaTeX から npm の KaTeX に替えた。図は描画命令で置く。
    // 4: 描画命令の層に数式のラベルの数を足した。インライン数式の組の PNG は置かなくなった。
    // 5: WebView2 でキャプチャした図の PNG を置かなくなった。キーに DPR を混ぜない。
    // 6: ファイルの拡張子を .png から .drawing にした。
    static constexpr uint32_t VERSION = 6;

    MermaidFileCache() = default;
    ~MermaidFileCache();

    MermaidFileCache(const MermaidFileCache&) = delete;
    MermaidFileCache& operator=(const MermaidFileCache&) = delete;

    struct CacheEntry {
        float css_width = 0.0f;
        float css_height = 0.0f;
    };

    // 索引の読み込みは初回利用まで遅らせ、図を含まない文書の起動を塞がない。
    void Init(TaskScheduler& scheduler);
    struct Hit {
        uint64_t key = 0;
        CacheEntry entry;
    };
    // 図の寸法を、幅に縛られない結果 (max_width より狭いときだけ使える) を先に、無ければ幅つきで引き、当たったキーと寸法を返す。
    // touch なら当たった項目の LRU を更新する (使えない幅なしの項目では更新しない)。寸法だけ見るときは更新しない。
    std::optional<Hit> Find(uint64_t width_free_key, uint64_t sized_key, float max_width, bool touch);
    // ファイルの読み込み (ReadAllBytes) は呼び出し側が任意のスレッドで行い、失敗したら OnReadFailed で UI スレッドに戻して反映する。
    std::filesystem::path FilePath(uint64_t key) const;
    // ファイルが確実に存在しない場合のみインデックスから除く (共有違反などの一時エラーは保持)。
    void OnReadFailed(uint64_t key, DWORD read_error);
    void StoreAsync(uint64_t key, float css_width, float css_height, std::shared_ptr<const ByteBuffer> data);
    void SaveIndex();
    // 索引とファイルをすべて消す。書き込み中のファイルは着地後に worker が消す。
    void Clear();
    // 前の版が図の描画に使った WebView2 のデータ (設定フォルダの WebView2Data) が残っている。
    bool HasLegacyData() const;
    // それを worker で消す。利用者がキャッシュの削除を選んだときだけ呼ぶ (Clear は版の違う索引を読んだときにも呼ぶ)。
    void RemoveLegacyDataAsync();
    void Shutdown();
    void SetCacheDir(const std::filesystem::path& dir);
    void SetLimits(size_t max_entries, uint64_t max_total_size);

    size_t EntryCount()
    {
        return Index().entries.size();
    }
    uint64_t TotalSize()
    {
        return Index().total_size;
    }

private:
    static constexpr size_t DEFAULT_MAX_ENTRIES = 4096;
    static constexpr uint64_t DEFAULT_MAX_TOTAL_SIZE = 1ULL * 1024 * 1024 * 1024; // 1GB

    struct IndexEntry {
        float css_width = 0.0f;
        float css_height = 0.0f;
        uint32_t file_size = 0;
        int64_t last_used = 0;
    };

    // 追い出しは満杯時だけなので、順序構造を持たず last_used の線形走査で最古を探す。
    struct IndexState {
        using Map = std::unordered_map<uint64_t, IndexEntry>;
        Map entries;
        uint64_t total_size = 0;
        // last_used に積む単調増加シーケンス。ms 時刻だと連続 Lookup で衝突して順序が付かない。
        int64_t lru_seq = 0;
        // 読み込んだか書いた時点の lru_seq。last_used がこれより大きいエントリは、その後に足したか使ったもの。
        int64_t saved_seq = 0;
        // 読み込んだか書いた後に除いたキー。他のプロセスが書いた索引に重ねるときに、そちらからも除く。
        std::unordered_set<uint64_t> removed;
        // 終了時に変更の無い索引を書き戻さないためのフラグ。
        bool dirty = false;

        // 変更系はすべて dirty を立てる (読み込み直後と書いた直後だけ MarkSaved が戻す)。
        void Add(uint64_t key, IndexEntry entry);
        void Remove(Map::iterator it);
        void MarkSaved() noexcept
        {
            saved_seq = lru_seq;
            removed.clear();
            dirty = false;
        }
        int64_t NextLruSeq() noexcept
        {
            return ++lru_seq;
        }
        void Touch(Map::iterator it) noexcept
        {
            it->second.last_used = NextLruSeq();
            dirty = true;
        }
    };

    std::filesystem::path GetIndexPath() const;
    std::filesystem::path GetLegacyDataPath() const;
    // 初回呼び出しで index.bin を読み込む唯一の入口。
    IndexState& Index();
    // 違う版か、ヘッダの壊れた索引なら nullopt。索引が無いときは空の索引。
    // 読めないか短いときは、strict なら nullopt、そうでなければ空の索引。呼び出し側が InterProcessFileLock を持つ。
    std::optional<IndexState> LoadIndex(bool strict) const;
    // 他のプロセスが先に書いた索引を読み直し、このプロセスの変更を重ねる。読めないか使えないなら nullopt。
    std::optional<IndexState> MergeWithSavedIndex();
    // new_entries 件・new_bytes バイトを足せるまで、古いものから追い出す。
    void EvictIfNeeded(IndexState& index, size_t new_entries, uint64_t new_bytes);
    // 前の版の Clear が別名にして消し損ねたフォルダ (<cache_dir_>.deleting-*) を worker で消す。次の版で除く。
    void RemoveDoomedDirsAsync();
    // path に書く前に呼ぶ。親ディレクトリを作り、Clear が消すよう控えた同じ名前を控えから外す (書いた後に消されない)。
    // どのスレッドからでも呼べる。
    void PrepareWrite(const std::filesystem::path& path);
    void RemoveDoomedFilesAsync();
    void RemoveDoomedFiles();

    std::filesystem::path cache_dir_;

    // 未読み込みは nullopt。
    std::optional<IndexState> index_;

    size_t max_entries_ = DEFAULT_MAX_ENTRIES;
    uint64_t max_total_size_ = DEFAULT_MAX_TOTAL_SIZE;

    TaskScheduler* scheduler_ = nullptr;

    mutable std::mutex pending_mutex_;
    // 書き込み中のキー → 書き込み中に追い出されたか (true なら worker が書き込み後にファイルを消す)。
    std::unordered_map<uint64_t, bool> pending_writes_;
    // Clear が控えて、worker がまだ消していないファイル名。消す間はロックを持つので、
    // pending_mutex_ (UI の StoreAsync も取る) とは分ける。
    std::mutex doomed_mutex_;
    std::unordered_set<std::wstring> doomed_files_;

    // Shutdown / dtor で worker 完了を待つ。scheduler_ 共有 worker から self を参照する race を排除する。
    WorkerLatch latch_;
};

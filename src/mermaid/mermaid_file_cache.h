#pragma once
#include "utility.h"
#include "worker_latch.h"
#include <cstdint>
#include <mutex>
#include <optional>
#include <filesystem>
#include <unordered_map>
#include <windows.h>

class TaskScheduler;

// Mermaidダイアグラムの描画済みPNGをファイルに永続化するキャッシュ。
// 幅は mermaid_util::QuantizeWidth で量子化し、コード・テーマの組み合わせごとにPNGを保持する。
// PNG書き出しはTaskSchedulerを介してバックグラウンドスレッドで非同期に行う。
// インデックスはメモリ上で管理し、SaveIndex()でディスクに書き出す。
// すべてのパブリックメソッドはUIスレッドから呼び出す必要がある
// （StoreAsyncのタスク投入のみスレッドセーフ）。
class MermaidFileCache {
public:
    MermaidFileCache() = default;
    ~MermaidFileCache();

    MermaidFileCache(const MermaidFileCache&) = delete;
    MermaidFileCache& operator=(const MermaidFileCache&) = delete;

    struct CacheEntry {
        float css_width = 0.0f;
        float css_height = 0.0f;
    };

    // 索引の読み込みは初回利用まで遅らせ、図を含まない文書の起動を塞がない。
    void Init(float current_dpr, TaskScheduler& scheduler);
    // 以降の参照・保存のキーに混ぜる DPR を差し替える。別の DPR のエントリは LRU で淘汰させる。
    void SetDpr(float dpr) noexcept
    {
        current_dpr_ = dpr;
    }
    struct Hit {
        uint64_t key = 0;
        CacheEntry entry;
    };
    // 図の寸法を、幅に縛られない結果 (max_width より狭いときだけ使える) を先に、無ければ幅つきで引き、当たったキーと寸法を返す。
    // touch なら当たった項目の LRU を更新する (使えない幅なしの項目では更新しない)。寸法だけ見るときは更新しない。
    std::optional<Hit> Find(uint64_t width_free_key, uint64_t sized_key, float max_width, bool touch);
    // PNG の読み込み (ReadAllBytes) は呼び出し側が任意のスレッドで行い、失敗したら OnReadFailed で UI スレッドに戻して反映する。
    // キャッシュのディレクトリが無ければ空。
    std::filesystem::path PngPath(uint64_t key) const
    {
        return GetPngPath(InternalKey(key));
    }
    // ファイルが確実に存在しない場合のみインデックスから除く (共有違反などの一時エラーは保持)。
    void OnReadFailed(uint64_t key, DWORD read_error);
    // png_data は表示側 (メモリキャッシュ / DiagramEntry) と共有し、書き込み用にコピーしない。
    void StoreAsync(uint64_t key, float css_width, float css_height, PngBytes png_data);
    void SaveIndex();
    // 索引と PNG をすべて消す。書き込み中の PNG は着地後に worker が消す。
    void Clear();
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

#ifdef MENDO_TESTING
    // テスト用: 外部キーから実ファイル名（PNG）を導く。
    // 内部で DPR を mix した内部キーを使うため。
    constexpr uint64_t InternalKeyForTest(uint64_t external_key) const noexcept
    {
        return InternalKey(external_key);
    }
#endif

private:
    static constexpr uint32_t MAGIC = 0x4D454D43u; // "MEMC"
    static constexpr uint32_t VERSION = 1;
    static constexpr size_t DEFAULT_MAX_ENTRIES = 4096;
    static constexpr uint64_t DEFAULT_MAX_TOTAL_SIZE = 1ULL * 1024 * 1024 * 1024; // 1GB

    struct IndexEntry {
        float css_width = 0.0f;
        float css_height = 0.0f;
        uint32_t png_size = 0;
        int64_t last_used = 0;
    };

    // 追い出しは満杯時だけなので、順序構造を持たず last_used の線形走査で最古を探す。
    struct IndexState {
        using Map = std::unordered_map<uint64_t, IndexEntry>;
        Map entries;
        uint64_t total_size = 0;
        // last_used に積む単調増加シーケンス。ms 時刻だと連続 Lookup で衝突して順序が付かない。
        int64_t lru_seq = 0;
        // 終了時に変更の無い索引を書き戻さないためのフラグ。
        bool dirty = false;

        // 変更系はすべて dirty を立てる (読み込み直後だけ LoadIndex が戻す)。
        void Add(uint64_t key, IndexEntry entry);
        void Remove(Map::iterator it) noexcept;
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

    std::filesystem::path GetPngPath(uint64_t key) const;
    std::filesystem::path GetIndexPath() const;
    // 初回呼び出しで index.bin を読み込む唯一の入口。
    IndexState& Index();
    IndexState LoadIndex() const;
    void EvictIfNeeded(IndexState& index, uint32_t new_png_size);
    // Clear が別名にして消すフォルダ (<cache_dir_>.deleting-*) を worker ですべて消す。
    // 消している途中で終了したり、中のファイルを開かれていて消し損ねたりしたものも拾う。
    void RemoveDoomedDirsAsync();

    // current_dpr_ を mix した内部キーを返す。DPR ごとにエントリを分離して
    // DPI 変更/モニタ切替時の全消去を避ける。
    constexpr uint64_t InternalKey(uint64_t external_key) const noexcept
    {
        // DPR を 1/100 単位で量子化（典型値 100/125/150/175/200）。
        const uint32_t dpr_q = static_cast<uint32_t>(current_dpr_ * 100.0f + 0.5f);
        return external_key ^ (static_cast<uint64_t>(dpr_q) * 0x9E3779B97F4A7C15ULL);
    }

    std::filesystem::path cache_dir_;
    float current_dpr_ = 0.0f;

    // 未読み込みは nullopt。
    std::optional<IndexState> index_;

    size_t max_entries_ = DEFAULT_MAX_ENTRIES;
    uint64_t max_total_size_ = DEFAULT_MAX_TOTAL_SIZE;

    TaskScheduler* scheduler_ = nullptr;

    mutable std::mutex pending_mutex_;
    // 書き込み中のキー → 書き込み中に追い出されたか (true なら worker が書き込み後に PNG を消す)。
    std::unordered_map<uint64_t, bool> pending_writes_;

    // Shutdown / dtor で worker 完了を待つ。scheduler_ 共有 worker から self を参照する race を排除する。
    WorkerLatch latch_;
};

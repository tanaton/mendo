#include <gtest/gtest.h>
#include "mermaid_file_cache.h"
#include "file_io.h"
#include "task_scheduler.h"
#include "mermaid_util.h"
#include "test_helpers.h"
#include <filesystem>
#include <future>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>

// ダミーPNGデータを生成する（ファイルキャッシュの単体テスト用、有効なPNGである必要はない）。
static PngBytes MakeDummyPng(size_t size = 1024)
{
    std::pmr::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<uint8_t>(i & 0xFF);
    }
    return std::make_shared<const std::pmr::vector<uint8_t>>(std::move(data));
}

using PngBlob = std::pmr::vector<uint8_t>;

// 製品側は LookupPath の後の読み込みを worker で行う。テストでは同期的に読む。
static bool LookupAndRead(MermaidFileCache& cache, uint64_t key, MermaidFileCache::CacheEntry& entry, PngBlob& png)
{
    std::filesystem::path path;
    if (!cache.LookupPath(key, entry, path)) {
        return false;
    }
    DWORD read_error = 0;
    png = ReadAllBytes(path.c_str(), &read_error);
    if (png.empty()) {
        cache.OnReadFailed(key, read_error);
        return false;
    }
    return true;
}

// 件数の上限だけを効かせるテスト用。
constexpr uint64_t kNoSizeLimit = 1ULL * 1024 * 1024 * 1024;

struct RawIndexRecord {
    uint64_t key = 0;
    float css_width = 0.0f;
    float css_height = 0.0f;
    uint32_t png_size = 0;
    int64_t last_used = 0;
};

// index.bin の書式 (ヘッダ 16 バイト + レコード 28 バイト) でバイト列を組み立てる。
// declared_count はヘッダに書く件数で、records の数と食い違わせて壊れた索引を作れる。
static std::string BuildRawIndex(uint32_t declared_count, std::initializer_list<RawIndexRecord> records)
{
    std::string buf;
    const auto put = [&buf](const auto& v) { buf.append(reinterpret_cast<const char*>(&v), sizeof(v)); };
    put(uint32_t{ 0x4D454D43u }); // "MEMC"
    put(uint32_t{ 1 });
    put(1.0f);
    put(declared_count);
    for (const auto& r : records) {
        put(r.key);
        put(r.css_width);
        put(r.css_height);
        put(r.png_size);
        put(r.last_used);
    }
    return buf;
}

class MermaidFileCacheTest : public TempDirTestBase {
protected:
    void SetUp() override
    {
        cache_.SetCacheDir(temp_dir_);
        scheduler_.Init(2);
    }

    void TearDown() override
    {
        scheduler_.Shutdown();
    }

    void InitCache(float dpr = 1.0f)
    {
        cache_.Init(dpr, scheduler_);
    }

    // SaveIndex + スケジューラ再起動 → 再ロードのサイクルを実行する
    void FlushAndReopen(float dpr = 1.0f)
    {
        cache_.SaveIndex();
        scheduler_.Shutdown();
        scheduler_.Init(2);
        cache_.Init(dpr, scheduler_);
    }

    // 内部キーから PNG ファイル名を組み立てる (MermaidFileCache::GetPngPath と同じ書式)。
    std::filesystem::path PngPathOf(uint64_t key) const
    {
        wchar_t name[24];
        swprintf_s(name, L"%016llx.png", cache_.InternalKeyForTest(key));
        return temp_dir_ / name;
    }

    // 書き込みを着地させてから、同じディレクトリを別インスタンスで開く。
    std::unique_ptr<MermaidFileCache> OpenFreshCache(float dpr = 1.0f)
    {
        scheduler_.Shutdown();
        scheduler_.Init(2);
        auto fresh = std::make_unique<MermaidFileCache>();
        fresh->SetCacheDir(temp_dir_);
        fresh->Init(dpr, scheduler_);
        return fresh;
    }

    // 書き込み中に追い出されたキーの PNG は、着地後に消えて孤立ファイルとして残らない。
    // 同じ図が文書に複数あると、同じキーの保存が書き込み中に重なる (key1_stores > 1)。
    void ExpectEvictionDuringPendingWriteLeavesNoOrphanPng(int key1_stores)
    {
        scheduler_.Shutdown();
        cache_.SetLimits(2, kNoSizeLimit);
        InitCache();
        scheduler_.Init(1);

        // 唯一の worker を塞ぎ、以降の書き込みを確実に in-flight のまま止める。
        std::promise<void> release;
        std::shared_future<void> released = release.get_future().share();
        ASSERT_TRUE(scheduler_.Post([released] { released.wait(); }));

        for (int k = 0; k < key1_stores; ++k) {
            cache_.StoreAsync(1, 100.0f, 50.0f, MakeDummyPng(100));
        }
        cache_.StoreAsync(2, 100.0f, 50.0f, MakeDummyPng(100));
        cache_.StoreAsync(3, 100.0f, 50.0f, MakeDummyPng(100)); // key=1 を追い出す
        EXPECT_EQ(cache_.EntryCount(), 2u);

        release.set_value();
        scheduler_.Shutdown();

        EXPECT_FALSE(std::filesystem::exists(PngPathOf(1)));
        EXPECT_TRUE(std::filesystem::exists(PngPathOf(2)));
        EXPECT_TRUE(std::filesystem::exists(PngPathOf(3)));

        scheduler_.Init(2);
    }

    TaskScheduler scheduler_;
    MermaidFileCache cache_;
};

// ═══════════════════════════════════════════════
// 基本的な格納・検索
// ═══════════════════════════════════════════════

TEST_F(MermaidFileCacheTest, StoreAndLookupRoundTrip)
{
    InitCache();

    uint64_t key = 12345;
    auto png = MakeDummyPng(512);
    cache_.StoreAsync(key, 400.0f, 300.0f, png);

    // SaveIndex + Shutdown → 再Init で永続化された状態から読み込み
    FlushAndReopen();

    MermaidFileCache::CacheEntry entry;
    PngBlob out;
    EXPECT_TRUE(LookupAndRead(cache_, key, entry, out));
    EXPECT_EQ(entry.css_width, 400.0f);
    EXPECT_EQ(entry.css_height, 300.0f);
    EXPECT_EQ(out, *png);
}

TEST_F(MermaidFileCacheTest, LookupMissReturnsFalse)
{
    InitCache();

    MermaidFileCache::CacheEntry entry;
    PngBlob out;
    EXPECT_FALSE(LookupAndRead(cache_, 99999, entry, out));
}

TEST_F(MermaidFileCacheTest, StoreUpdatesEntryCount)
{
    InitCache();

    EXPECT_EQ(cache_.EntryCount(), 0u);

    cache_.StoreAsync(1, 100.0f, 50.0f, MakeDummyPng(256));
    EXPECT_EQ(cache_.EntryCount(), 1u);

    cache_.StoreAsync(2, 200.0f, 100.0f, MakeDummyPng(256));
    EXPECT_EQ(cache_.EntryCount(), 2u);
}

TEST_F(MermaidFileCacheTest, StoreSameKeyUpdatesEntry)
{
    InitCache();

    cache_.StoreAsync(1, 100.0f, 50.0f, MakeDummyPng(256));
    EXPECT_EQ(cache_.EntryCount(), 1u);
    EXPECT_EQ(cache_.TotalSize(), 256u);

    // 同じキーで別サイズのデータを格納
    cache_.StoreAsync(1, 200.0f, 100.0f, MakeDummyPng(512));
    EXPECT_EQ(cache_.EntryCount(), 1u);
    EXPECT_EQ(cache_.TotalSize(), 512u);
}

TEST_F(MermaidFileCacheTest, TotalSizeTracking)
{
    InitCache();

    cache_.StoreAsync(1, 100.0f, 50.0f, MakeDummyPng(1000));
    cache_.StoreAsync(2, 200.0f, 100.0f, MakeDummyPng(2000));
    EXPECT_EQ(cache_.TotalSize(), 3000u);
}

// ═══════════════════════════════════════════════
// ファイルが存在しない場合のLookup
// ═══════════════════════════════════════════════

TEST_F(MermaidFileCacheTest, LookupCleansUpStaleIndexEntry)
{
    InitCache();

    cache_.StoreAsync(42, 100.0f, 50.0f, MakeDummyPng(256));
    EXPECT_EQ(cache_.EntryCount(), 1u);

    // ファイル書き出し完了 → SaveIndex → 再Init
    FlushAndReopen();

    // PNGファイルを手動削除してからLookup（内部キーでファイル名を計算）
    std::filesystem::remove(PngPathOf(42ULL));

    MermaidFileCache::CacheEntry entry;
    PngBlob out;
    EXPECT_FALSE(LookupAndRead(cache_, 42, entry, out));
    // 古いインデックスエントリが削除されていること
    EXPECT_EQ(cache_.EntryCount(), 0u);
}

// LookupPath はファイルを読まずにパスと寸法を返し、読み込み失敗は OnReadFailed で反映する。
TEST_F(MermaidFileCacheTest, LookupPathAndOnReadFailed)
{
    InitCache();
    cache_.StoreAsync(42, 100.0f, 50.0f, MakeDummyPng(256));
    FlushAndReopen();

    MermaidFileCache::CacheEntry entry;
    std::filesystem::path path;
    ASSERT_TRUE(cache_.LookupPath(42, entry, path));
    EXPECT_FLOAT_EQ(entry.css_width, 100.0f);
    EXPECT_FLOAT_EQ(entry.css_height, 50.0f);
    EXPECT_TRUE(std::filesystem::exists(path));

    // 共有違反などの一時エラーではエントリを保持する。
    cache_.OnReadFailed(42, ERROR_SHARING_VIOLATION);
    EXPECT_EQ(cache_.EntryCount(), 1u);

    cache_.OnReadFailed(42, ERROR_FILE_NOT_FOUND);
    EXPECT_EQ(cache_.EntryCount(), 0u);
    EXPECT_FALSE(cache_.LookupPath(42, entry, path));
}

// ═══════════════════════════════════════════════
// LRU削除 — エントリ数上限
// ═══════════════════════════════════════════════

TEST_F(MermaidFileCacheTest, EvictsOldestWhenMaxEntriesExceeded)
{
    cache_.SetLimits(3, kNoSizeLimit);
    InitCache();

    // 3エントリを格納
    cache_.StoreAsync(1, 100.0f, 50.0f, MakeDummyPng(100));
    cache_.StoreAsync(2, 100.0f, 50.0f, MakeDummyPng(100));
    cache_.StoreAsync(3, 100.0f, 50.0f, MakeDummyPng(100));
    EXPECT_EQ(cache_.EntryCount(), 3u);

    // 4つ目を追加 → 最古のエントリ(key=1)が削除される
    cache_.StoreAsync(4, 100.0f, 50.0f, MakeDummyPng(100));
    EXPECT_EQ(cache_.EntryCount(), 3u);

    // key=1が削除されていることを永続化後にも確認
    FlushAndReopen();

    MermaidFileCache::CacheEntry entry;
    PngBlob out;
    EXPECT_FALSE(LookupAndRead(cache_, 1, entry, out));
    EXPECT_TRUE(LookupAndRead(cache_, 2, entry, out));
    EXPECT_TRUE(LookupAndRead(cache_, 3, entry, out));
    EXPECT_TRUE(LookupAndRead(cache_, 4, entry, out));
}

// ═══════════════════════════════════════════════
// LRU削除 — 合計サイズ上限
// ═══════════════════════════════════════════════

TEST_F(MermaidFileCacheTest, EvictsWhenMaxSizeExceeded)
{
    // 合計サイズ上限を500バイトに設定
    cache_.SetLimits(100, 500);
    InitCache();

    cache_.StoreAsync(1, 100.0f, 50.0f, MakeDummyPng(200));
    cache_.StoreAsync(2, 100.0f, 50.0f, MakeDummyPng(200));
    EXPECT_EQ(cache_.EntryCount(), 2u);
    EXPECT_EQ(cache_.TotalSize(), 400u);

    // 300バイト追加 → 合計700 > 500 → 最古エントリを削除
    cache_.StoreAsync(3, 100.0f, 50.0f, MakeDummyPng(300));
    // key=1が削除されて合計500バイト以内になる
    EXPECT_LE(cache_.TotalSize(), 500u);
    EXPECT_EQ(cache_.EntryCount(), 2u);
}

// ═══════════════════════════════════════════════
// DPR不一致によるキャッシュクリア
// ═══════════════════════════════════════════════

TEST_F(MermaidFileCacheTest, DprMismatchSeparatesEntries)
{
    InitCache(1.0f);

    cache_.StoreAsync(1, 100.0f, 50.0f, MakeDummyPng(256));
    cache_.StoreAsync(2, 200.0f, 100.0f, MakeDummyPng(256));
    EXPECT_EQ(cache_.EntryCount(), 2u);
    cache_.SaveIndex();

    // 異なる DPR で再初期化 → エントリは index 上に残るが、別キー扱いとなり
    // 同じ external key での Lookup はミスになる（DPR ごとに独立してキャッシュ）。
    auto fresh = OpenFreshCache(2.0f);
    EXPECT_EQ(fresh->EntryCount(), 2u);

    MermaidFileCache::CacheEntry entry;
    PngBlob out;
    EXPECT_FALSE(LookupAndRead(*fresh, 1, entry, out));
    EXPECT_FALSE(LookupAndRead(*fresh, 2, entry, out));
}

TEST_F(MermaidFileCacheTest, SameDprPreservesCache)
{
    InitCache(1.5f);

    cache_.StoreAsync(1, 100.0f, 50.0f, MakeDummyPng(256));
    cache_.SaveIndex();

    // 同じDPRで再初期化 → エントリが保持される
    EXPECT_EQ(OpenFreshCache(1.5f)->EntryCount(), 1u);
}

// ═══════════════════════════════════════════════
// SaveIndex / LoadIndex 永続化
// ═══════════════════════════════════════════════

TEST_F(MermaidFileCacheTest, SaveAndLoadIndexRoundTrip)
{
    InitCache(1.0f);

    cache_.StoreAsync(100, 640.0f, 480.0f, MakeDummyPng(1024));
    cache_.StoreAsync(200, 320.0f, 240.0f, MakeDummyPng(512));
    cache_.SaveIndex();

    auto fresh = OpenFreshCache();
    EXPECT_EQ(fresh->EntryCount(), 2u);

    MermaidFileCache::CacheEntry entry;
    PngBlob out;
    EXPECT_TRUE(LookupAndRead(*fresh, 100, entry, out));
    EXPECT_EQ(entry.css_width, 640.0f);
    EXPECT_EQ(entry.css_height, 480.0f);
    EXPECT_EQ(out.size(), 1024u);

    EXPECT_TRUE(LookupAndRead(*fresh, 200, entry, out));
    EXPECT_EQ(entry.css_width, 320.0f);
    EXPECT_EQ(entry.css_height, 240.0f);
    EXPECT_EQ(out.size(), 512u);
}

TEST_F(MermaidFileCacheTest, InitDoesNotTouchDisk)
{
    const auto dir = temp_dir_ / L"lazy";
    cache_.SetCacheDir(dir);
    InitCache(1.0f);
    EXPECT_FALSE(std::filesystem::exists(dir));
}

TEST_F(MermaidFileCacheTest, SaveIndexBeforeFirstUseKeepsIndex)
{
    InitCache(1.0f);
    cache_.StoreAsync(100, 640.0f, 480.0f, MakeDummyPng(1024));
    FlushAndReopen(1.0f);

    // 一度も参照しないまま終了しても既存の索引を空で上書きしないこと
    FlushAndReopen(1.0f);
    EXPECT_EQ(cache_.EntryCount(), 1u);
}

TEST_F(MermaidFileCacheTest, SaveIndexSkipsWriteWhenUnchanged)
{
    InitCache(1.0f);
    cache_.StoreAsync(100, 640.0f, 480.0f, MakeDummyPng(1024));
    FlushAndReopen(1.0f);
    ASSERT_EQ(cache_.EntryCount(), 1u);

    // 読み込んだだけで変更していなければ書き出さない
    const auto index_path = temp_dir_ / L"index.bin";
    ASSERT_TRUE(std::filesystem::remove(index_path));
    MermaidFileCache::CacheEntry entry;
    EXPECT_TRUE(cache_.LookupDimensions(100, entry));
    cache_.SaveIndex();
    EXPECT_FALSE(std::filesystem::exists(index_path));
}

// 壊れた索引はクラッシュせずに空として扱う。
TEST_F(MermaidFileCacheTest, LoadIndexIgnoresBrokenFile)
{
    struct Case {
        const char* label;
        std::string bytes;
    };
    // 5 件と宣言したうえで、1 件目のレコードの途中 (28 バイト中 10 バイト) で切る。
    auto truncated = BuildRawIndex(5, { { .key = 999, .css_width = 100.0f } });
    truncated.resize(16 + 10);
    const Case kCases[] = {
        { "ZeroBytes", "" },
        { "InvalidMagic", std::string("\xEF\xBE\xAD\xDE", 4) },
        { "TruncatedRecord", std::move(truncated) },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.label);
        WriteTempFile(L"index.bin", c.bytes);
        EXPECT_EQ(OpenFreshCache()->EntryCount(), 0u);
    }
}

TEST_F(MermaidFileCacheTest, LoadIndexSkipsCorruptedEntries)
{
    WriteTempFile(L"index.bin", BuildRawIndex(4, {
        // css_width が負
        { 1, -100.0f, 50.0f, 256, 1000 },
        // png_size が 0
        { 2, 100.0f, 50.0f, 0, 1000 },
        // css_width が NaN
        { 3, std::numeric_limits<float>::quiet_NaN(), 100.0f, 256, 1000 },
        // 正常
        { 4, 200.0f, 100.0f, 512, 1000 },
    }));

    const auto fresh = OpenFreshCache();
    EXPECT_EQ(fresh->EntryCount(), 1u);
    EXPECT_EQ(fresh->TotalSize(), 512u);
}

// ═══════════════════════════════════════════════
// 非同期書き出し
// ═══════════════════════════════════════════════

TEST_F(MermaidFileCacheTest, AsyncWriteCreatesFile)
{
    InitCache();

    auto png = MakeDummyPng(2048);
    cache_.StoreAsync(777, 500.0f, 400.0f, png);

    // スケジューラをシャットダウンしてキューを完全に処理する
    scheduler_.Shutdown();

    // PNGファイルが存在すること（内部キーでファイル名を計算）
    EXPECT_TRUE(std::filesystem::exists(PngPathOf(777ULL)));

    // TearDownのためにスケジューラを再起動
    scheduler_.Init(2);
}

// ═══════════════════════════════════════════════
// Initなしでの呼び出し
// ═══════════════════════════════════════════════

TEST_F(MermaidFileCacheTest, OperationsWithoutInitAreNoOp)
{
    // Initを呼ばない状態で各メソッドがクラッシュしないこと
    MermaidFileCache uninit;
    uninit.SetCacheDir(temp_dir_);

    MermaidFileCache::CacheEntry entry;
    PngBlob out;
    EXPECT_FALSE(LookupAndRead(uninit, 1, entry, out));

    uninit.SaveIndex();
    uninit.Shutdown();
}

// ═══════════════════════════════════════════════
// LRU — Lookupが使用順を更新する
// ═══════════════════════════════════════════════

TEST_F(MermaidFileCacheTest, LookupRefreshesTimestamp)
{
    cache_.SetLimits(2, kNoSizeLimit);
    InitCache();

    // key=1を最初に格納
    cache_.StoreAsync(1, 100.0f, 50.0f, MakeDummyPng(100));

    // key=2を格納（key=1より新しい使用順）
    cache_.StoreAsync(2, 100.0f, 50.0f, MakeDummyPng(100));
    FlushAndReopen();

    // key=1をLookupして使用順を更新（key=2より新しくなる）
    MermaidFileCache::CacheEntry entry;
    PngBlob out;
    EXPECT_TRUE(LookupAndRead(cache_, 1, entry, out));

    // key=3を追加 → key=2が最古なのでkey=2が削除される
    cache_.StoreAsync(3, 100.0f, 50.0f, MakeDummyPng(100));
    EXPECT_EQ(cache_.EntryCount(), 2u);

    // 永続化後にも確認
    FlushAndReopen();

    EXPECT_TRUE(LookupAndRead(cache_, 1, entry, out));
    EXPECT_FALSE(LookupAndRead(cache_, 2, entry, out));
    EXPECT_TRUE(LookupAndRead(cache_, 3, entry, out));
}

// ═══════════════════════════════════════════════
// LookupDimensions — ディスクI/O無しのサイズ取得
// ═══════════════════════════════════════════════

TEST_F(MermaidFileCacheTest, LookupDimensionsReturnsStoredSize)
{
    InitCache();

    cache_.StoreAsync(1, 400.0f, 300.0f, MakeDummyPng(256));

    MermaidFileCache::CacheEntry entry;
    EXPECT_TRUE(cache_.LookupDimensions(1, entry));
    EXPECT_EQ(entry.css_width, 400.0f);
    EXPECT_EQ(entry.css_height, 300.0f);
}

TEST_F(MermaidFileCacheTest, LookupDimensionsMissReturnsFalse)
{
    InitCache();

    MermaidFileCache::CacheEntry entry;
    EXPECT_FALSE(cache_.LookupDimensions(99999, entry));
}

TEST_F(MermaidFileCacheTest, LookupDimensionsSurvivesPersistence)
{
    InitCache();

    cache_.StoreAsync(42, 800.0f, 600.0f, MakeDummyPng(512));
    FlushAndReopen();

    MermaidFileCache::CacheEntry entry;
    EXPECT_TRUE(cache_.LookupDimensions(42, entry));
    EXPECT_EQ(entry.css_width, 800.0f);
    EXPECT_EQ(entry.css_height, 600.0f);
}

TEST_F(MermaidFileCacheTest, LookupDimensionsDoesNotUpdateLru)
{
    // LookupDimensionsはLRU使用順を更新しないことを確認する。
    // エビクション対象の選択に影響しないため、推定専用の軽量パスとして安全。
    cache_.SetLimits(2, kNoSizeLimit);
    InitCache();

    cache_.StoreAsync(1, 100.0f, 50.0f, MakeDummyPng(100));
    cache_.StoreAsync(2, 200.0f, 100.0f, MakeDummyPng(100));
    EXPECT_EQ(cache_.EntryCount(), 2u);

    // key=1をLookupDimensionsで参照（LRUは更新されないはず）
    MermaidFileCache::CacheEntry entry;
    EXPECT_TRUE(cache_.LookupDimensions(1, entry));

    // key=3を追加 → LRU未更新のkey=1が最古として削除されるはず
    cache_.StoreAsync(3, 300.0f, 150.0f, MakeDummyPng(100));
    EXPECT_EQ(cache_.EntryCount(), 2u);

    EXPECT_FALSE(cache_.LookupDimensions(1, entry));
    EXPECT_TRUE(cache_.LookupDimensions(2, entry));
    EXPECT_TRUE(cache_.LookupDimensions(3, entry));
}

// ═══════════════════════════════════════════════
// Pending-write integrity (Medium-5 回帰)
// ═══════════════════════════════════════════════

// StoreAsync 直後で PNG が未着地のまま Lookup された時、index が
// stale 扱いで消されないこと。修正前は orphan PNG（書き込み完了後に
// インデックスが指していない PNG）を生み出す原因になっていた。
TEST_F(MermaidFileCacheTest, LookupDuringPendingWriteKeepsIndex)
{
    // scheduler_ を Shutdown → InitCache → Init(1) で再構成し、
    // StoreAsync の直後にワーカーが PNG 書き込みを完了する前に
    // Lookup を走らせる。タイミング上 in-flight を捕捉できないことも
    // あるが、その場合は LookupDimensions が成功するだけで assertion は
    // 破綻しない（pending でも完了でも index は残っているべき、という
    // 契約を検証する）。
    scheduler_.Shutdown();
    InitCache();
    scheduler_.Init(1);

    const uint64_t key = 0x1234567890abcdefULL;
    cache_.StoreAsync(key, 100.0f, 80.0f, MakeDummyPng(64));

    // Lookup を即座に呼ぶ（タイミングによってはまだ書き込み完了前）。
    MermaidFileCache::CacheEntry entry;
    PngBlob out;
    LookupAndRead(cache_, key, entry, out);

    // 書き込み in-flight 中なら index は残されているべき。
    // タスクが先行して完了した場合は LookupDimensions も成功する。
    EXPECT_TRUE(cache_.LookupDimensions(key, entry))
        << "pending write 中に index が消されている（orphan PNG の温床）";
}

TEST_F(MermaidFileCacheTest, EvictionDuringPendingWriteLeavesNoOrphanPng)
{
    ExpectEvictionDuringPendingWriteLeavesNoOrphanPng(1);
}

TEST_F(MermaidFileCacheTest, EvictionDuringDuplicatePendingWritesLeavesNoOrphanPng)
{
    ExpectEvictionDuringPendingWriteLeavesNoOrphanPng(2);
}

// ═══════════════════════════════════════════════
// Clear (設定ダイアログのキャッシュ削除)
// ═══════════════════════════════════════════════

// 索引に無い PNG も含めてフォルダごと消える。消す作業は worker に回り、移したフォルダも残らない。次に開いても空のまま。
TEST_F(MermaidFileCacheTest, ClearRemovesEntriesAndFiles)
{
    // 消すフォルダの移し先 (隣) まで temp_dir_ の中に収める。
    const auto dir = temp_dir_ / L"cache";
    cache_.SetCacheDir(dir);
    InitCache();
    cache_.StoreAsync(1, 100.0f, 50.0f, MakeDummyPng(100));
    cache_.StoreAsync(2, 100.0f, 50.0f, MakeDummyPng(200));
    FlushAndReopen();
    ASSERT_EQ(cache_.EntryCount(), 2u);
    const auto orphan = dir / L"0123456789abcdef.png";
    ASSERT_TRUE(WriteAllBytes(orphan, "x", 1));

    cache_.Clear();

    EXPECT_EQ(cache_.EntryCount(), 0u);
    EXPECT_EQ(cache_.TotalSize(), 0u);
    EXPECT_FALSE(std::filesystem::exists(orphan));
    EXPECT_FALSE(std::filesystem::exists(dir / L"index.bin"));

    FlushAndReopen();
    EXPECT_EQ(cache_.EntryCount(), 0u);
    EXPECT_TRUE(std::filesystem::is_empty(temp_dir_)) << "移したフォルダが残っている";
}

// 書き込み中の PNG は着地後に消え、消した後に孤立ファイルとして現れない。
TEST_F(MermaidFileCacheTest, ClearDuringPendingWriteLeavesNoPng)
{
    scheduler_.Shutdown();
    InitCache();
    scheduler_.Init(1);

    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    ASSERT_TRUE(scheduler_.Post([released] { released.wait(); }));
    cache_.StoreAsync(1, 100.0f, 50.0f, MakeDummyPng(100));

    cache_.Clear();
    release.set_value();
    scheduler_.Shutdown();

    EXPECT_FALSE(std::filesystem::exists(PngPathOf(1)));
    EXPECT_EQ(cache_.EntryCount(), 0u);
    scheduler_.Init(2);
}

// 消した後に保存した図は通常どおり残る。
TEST_F(MermaidFileCacheTest, StoreAfterClearPersists)
{
    InitCache();
    cache_.StoreAsync(1, 100.0f, 50.0f, MakeDummyPng(100));
    cache_.Clear();
    cache_.StoreAsync(2, 100.0f, 50.0f, MakeDummyPng(100));
    cache_.SaveIndex();

    const auto fresh = OpenFreshCache();
    MermaidFileCache::CacheEntry entry;
    PngBlob out;
    EXPECT_TRUE(LookupAndRead(*fresh, 2, entry, out));
    EXPECT_FALSE(fresh->LookupDimensions(1, entry));
}

// 図の解像度は描画するワーカーの DPR で決まるので、キーもその DPR に追従する。
// Init 時の DPR に固定すると、別の DPR で描いた PNG を引いて図がぼやける・ずれる。
TEST_F(MermaidFileCacheTest, SetDprSwitchesKeySpace)
{
    InitCache(1.0f);
    cache_.StoreAsync(42, 400.0f, 300.0f, MakeDummyPng(64));
    const uint64_t key_at_1x = cache_.InternalKeyForTest(42);
    MermaidFileCache::CacheEntry entry;
    ASSERT_TRUE(cache_.LookupDimensions(42, entry));

    cache_.SetDpr(2.0f);
    EXPECT_NE(cache_.InternalKeyForTest(42), key_at_1x);
    EXPECT_FALSE(cache_.LookupDimensions(42, entry));

    cache_.SetDpr(1.0f);
    EXPECT_EQ(cache_.InternalKeyForTest(42), key_at_1x);
    EXPECT_TRUE(cache_.LookupDimensions(42, entry));
}

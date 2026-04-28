#include <gtest/gtest.h>
#include "mermaid_file_cache.h"
#include "task_scheduler.h"
#include "mermaid_util.h"
#include <filesystem>
#include <fstream>
#include <cstring>
#include <memory>
#include <thread>
#include <chrono>

// ダミーPNGデータを生成する（ファイルキャッシュの単体テスト用、有効なPNGである必要はない）。
static std::pmr::vector<uint8_t> MakeDummyPng(size_t size = 1024)
{
    std::pmr::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<uint8_t>(i & 0xFF);
    }
    return data;
}

class MermaidFileCacheTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        // テストごとに一意のディレクトリを使用（並列実行時の衝突回避）
        auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        std::string test_name = info->name();
        std::wstring wname(test_name.begin(), test_name.end());
        temp_dir_ = std::filesystem::temp_directory_path() / L"mendo_test_cache" / wname;
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
        cache_.SetCacheDir(temp_dir_);
        scheduler_.Init(2);
    }

    void TearDown() override
    {
        scheduler_.Shutdown();
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
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
        cache_.SetCacheDir(temp_dir_);
        scheduler_.Init(2);
        cache_.Init(dpr, scheduler_);
    }

    // 再ロード用の新しいキャッシュを作成
    std::unique_ptr<MermaidFileCache> CreateFreshCache()
    {
        auto fresh = std::make_unique<MermaidFileCache>();
        fresh->SetCacheDir(temp_dir_);
        return fresh;
    }

    std::filesystem::path temp_dir_;
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
    MermaidFileCache::PngBlob out;
    EXPECT_TRUE(cache_.Lookup(key, entry, out));
    EXPECT_EQ(entry.css_width, 400.0f);
    EXPECT_EQ(entry.css_height, 300.0f);
    ASSERT_EQ(out.size, png.size());
    EXPECT_EQ(std::memcmp(out.data.get(), png.data(), out.size), 0);
}

TEST_F(MermaidFileCacheTest, LookupMissReturnsFalse)
{
    InitCache();

    MermaidFileCache::CacheEntry entry;
    MermaidFileCache::PngBlob out;
    EXPECT_FALSE(cache_.Lookup(99999, entry, out));
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
    wchar_t name[24];
    swprintf_s(name, L"%016llx.png", cache_.InternalKeyForTest(42ULL));
    std::filesystem::remove(temp_dir_ / name);

    MermaidFileCache::CacheEntry entry;
    MermaidFileCache::PngBlob out;
    EXPECT_FALSE(cache_.Lookup(42, entry, out));
    // 古いインデックスエントリが削除されていること
    EXPECT_EQ(cache_.EntryCount(), 0u);
}

// ═══════════════════════════════════════════════
// LRU削除 — エントリ数上限
// ═══════════════════════════════════════════════

TEST_F(MermaidFileCacheTest, EvictsOldestWhenMaxEntriesExceeded)
{
    cache_.SetLimits(3, 1ULL * 1024 * 1024 * 1024);
    InitCache();

    // 3エントリを格納（タイムスタンプの差を確保）
    cache_.StoreAsync(1, 100.0f, 50.0f, MakeDummyPng(100));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    cache_.StoreAsync(2, 100.0f, 50.0f, MakeDummyPng(100));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    cache_.StoreAsync(3, 100.0f, 50.0f, MakeDummyPng(100));
    EXPECT_EQ(cache_.EntryCount(), 3u);

    // 4つ目を追加 → 最古のエントリ(key=1)が削除される
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    cache_.StoreAsync(4, 100.0f, 50.0f, MakeDummyPng(100));
    EXPECT_EQ(cache_.EntryCount(), 3u);

    // key=1が削除されていることを永続化後にも確認
    FlushAndReopen();

    MermaidFileCache::CacheEntry entry;
    MermaidFileCache::PngBlob out;
    EXPECT_FALSE(cache_.Lookup(1, entry, out));
    EXPECT_TRUE(cache_.Lookup(2, entry, out));
    EXPECT_TRUE(cache_.Lookup(3, entry, out));
    EXPECT_TRUE(cache_.Lookup(4, entry, out));
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
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    cache_.StoreAsync(2, 100.0f, 50.0f, MakeDummyPng(200));
    EXPECT_EQ(cache_.EntryCount(), 2u);
    EXPECT_EQ(cache_.TotalSize(), 400u);

    // 300バイト追加 → 合計700 > 500 → 最古エントリを削除
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
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

    // インデックスを保存してスケジューラを停止
    cache_.SaveIndex();
    scheduler_.Shutdown();

    // 異なる DPR で再初期化 → エントリは index 上に残るが、別キー扱いとなり
    // 同じ external key での Lookup はミスになる（DPR ごとに独立してキャッシュ）。
    auto fresh = CreateFreshCache();
    scheduler_.Init(2);
    fresh->Init(2.0f, scheduler_);
    EXPECT_EQ(fresh->EntryCount(), 2u);

    MermaidFileCache::CacheEntry entry;
    MermaidFileCache::PngBlob out;
    EXPECT_FALSE(fresh->Lookup(1, entry, out));
    EXPECT_FALSE(fresh->Lookup(2, entry, out));
}

TEST_F(MermaidFileCacheTest, SameDprPreservesCache)
{
    InitCache(1.5f);

    cache_.StoreAsync(1, 100.0f, 50.0f, MakeDummyPng(256));
    cache_.SaveIndex();
    scheduler_.Shutdown();

    // 同じDPRで再初期化 → エントリが保持される
    auto fresh = CreateFreshCache();
    scheduler_.Init(2);
    fresh->Init(1.5f, scheduler_);
    EXPECT_EQ(fresh->EntryCount(), 1u);
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
    scheduler_.Shutdown();

    // 新しいキャッシュオブジェクトで読み込み
    auto fresh = CreateFreshCache();
    scheduler_.Init(2);
    fresh->Init(1.0f, scheduler_);
    EXPECT_EQ(fresh->EntryCount(), 2u);

    MermaidFileCache::CacheEntry entry;
    MermaidFileCache::PngBlob out;
    EXPECT_TRUE(fresh->Lookup(100, entry, out));
    EXPECT_EQ(entry.css_width, 640.0f);
    EXPECT_EQ(entry.css_height, 480.0f);
    EXPECT_EQ(out.size, 1024u);

    EXPECT_TRUE(fresh->Lookup(200, entry, out));
    EXPECT_EQ(entry.css_width, 320.0f);
    EXPECT_EQ(entry.css_height, 240.0f);
    EXPECT_EQ(out.size, 512u);
}

TEST_F(MermaidFileCacheTest, LoadIndexWithInvalidMagicIgnoresFile)
{
    InitCache();
    scheduler_.Shutdown();

    // 不正なインデックスファイルを作成
    auto index_path = temp_dir_ / L"index.bin";
    {
        std::ofstream ofs(index_path, std::ios::binary);
        uint32_t bad_magic = 0xDEADBEEF;
        ofs.write(reinterpret_cast<const char*>(&bad_magic), 4);
    }

    auto fresh = CreateFreshCache();
    scheduler_.Init(2);
    fresh->Init(1.0f, scheduler_);
    EXPECT_EQ(fresh->EntryCount(), 0u);
}

TEST_F(MermaidFileCacheTest, LoadIndexWithTruncatedEntries)
{
    InitCache();
    scheduler_.Shutdown();

    // 正しいヘッダー + 途中で切れたエントリを書き込む
    auto index_path = temp_dir_ / L"index.bin";
    {
        std::ofstream ofs(index_path, std::ios::binary);
        uint32_t magic = 0x4D454D43u;
        uint32_t version = 1;
        float dpr = 1.0f;
        uint32_t count = 5; // 5エントリあると宣言するが実データは途中まで
        ofs.write(reinterpret_cast<const char*>(&magic), 4);
        ofs.write(reinterpret_cast<const char*>(&version), 4);
        ofs.write(reinterpret_cast<const char*>(&dpr), 4);
        ofs.write(reinterpret_cast<const char*>(&count), 4);
        // 1エントリ分だけ書いて打ち切り（28バイト中の10バイトだけ）
        uint64_t key = 999;
        ofs.write(reinterpret_cast<const char*>(&key), 8);
        float w = 100.0f;
        ofs.write(reinterpret_cast<const char*>(&w), 2); // 意図的に不完全
    }

    // クラッシュせずに空のインデックスとして扱われること
    auto fresh = CreateFreshCache();
    scheduler_.Init(2);
    fresh->Init(1.0f, scheduler_);
    EXPECT_EQ(fresh->EntryCount(), 0u);
}

TEST_F(MermaidFileCacheTest, LoadIndexSkipsCorruptedEntries)
{
    InitCache();
    scheduler_.Shutdown();

    // 正しいヘッダー + 不正なフィールド値を持つエントリ
    auto index_path = temp_dir_ / L"index.bin";
    {
        std::ofstream ofs(index_path, std::ios::binary);
        uint32_t magic = 0x4D454D43u;
        uint32_t version = 1;
        float dpr = 1.0f;
        uint32_t count = 3;
        ofs.write(reinterpret_cast<const char*>(&magic), 4);
        ofs.write(reinterpret_cast<const char*>(&version), 4);
        ofs.write(reinterpret_cast<const char*>(&dpr), 4);
        ofs.write(reinterpret_cast<const char*>(&count), 4);

        auto write_entry = [&](uint64_t key, float w, float h, uint32_t sz, int64_t t) {
            ofs.write(reinterpret_cast<const char*>(&key), 8);
            ofs.write(reinterpret_cast<const char*>(&w), 4);
            ofs.write(reinterpret_cast<const char*>(&h), 4);
            ofs.write(reinterpret_cast<const char*>(&sz), 4);
            ofs.write(reinterpret_cast<const char*>(&t), 8);
        };

        // エントリ1: css_widthが負 → スキップされる
        write_entry(1, -100.0f, 50.0f, 256, 1000);
        // エントリ2: png_sizeが0 → スキップされる
        write_entry(2, 100.0f, 50.0f, 0, 1000);
        // エントリ3: 正常 → 読み込まれる
        write_entry(3, 200.0f, 100.0f, 512, 1000);
    }

    auto fresh = CreateFreshCache();
    scheduler_.Init(2);
    fresh->Init(1.0f, scheduler_);
    // 不正な2エントリはスキップされ、正常な1エントリのみ読み込まれる
    EXPECT_EQ(fresh->EntryCount(), 1u);
    EXPECT_EQ(fresh->TotalSize(), 512u);
}

TEST_F(MermaidFileCacheTest, LoadIndexWithNaNWidthSkipsEntry)
{
    InitCache();
    scheduler_.Shutdown();

    auto index_path = temp_dir_ / L"index.bin";
    {
        std::ofstream ofs(index_path, std::ios::binary);
        uint32_t magic = 0x4D454D43u;
        uint32_t version = 1;
        float dpr = 1.0f;
        uint32_t count = 1;
        ofs.write(reinterpret_cast<const char*>(&magic), 4);
        ofs.write(reinterpret_cast<const char*>(&version), 4);
        ofs.write(reinterpret_cast<const char*>(&dpr), 4);
        ofs.write(reinterpret_cast<const char*>(&count), 4);

        uint64_t key = 42;
        float nan_val = std::numeric_limits<float>::quiet_NaN();
        float h = 100.0f;
        uint32_t sz = 256;
        int64_t t = 1000;
        ofs.write(reinterpret_cast<const char*>(&key), 8);
        ofs.write(reinterpret_cast<const char*>(&nan_val), 4);
        ofs.write(reinterpret_cast<const char*>(&h), 4);
        ofs.write(reinterpret_cast<const char*>(&sz), 4);
        ofs.write(reinterpret_cast<const char*>(&t), 8);
    }

    auto fresh = CreateFreshCache();
    scheduler_.Init(2);
    fresh->Init(1.0f, scheduler_);
    EXPECT_EQ(fresh->EntryCount(), 0u);
}

TEST_F(MermaidFileCacheTest, LoadIndexWithZeroBytesFile)
{
    InitCache();
    scheduler_.Shutdown();

    // 0バイトのインデックスファイル
    auto index_path = temp_dir_ / L"index.bin";
    { std::ofstream ofs(index_path, std::ios::binary); }

    auto fresh = CreateFreshCache();
    scheduler_.Init(2);
    fresh->Init(1.0f, scheduler_);
    EXPECT_EQ(fresh->EntryCount(), 0u);
}

// ═══════════════════════════════════════════════
// ClearAll
// ═══════════════════════════════════════════════

TEST_F(MermaidFileCacheTest, ClearAllRemovesEverything)
{
    InitCache();

    cache_.StoreAsync(1, 100.0f, 50.0f, MakeDummyPng(256));
    cache_.StoreAsync(2, 200.0f, 100.0f, MakeDummyPng(256));

    // ファイル書き出し完了 → SaveIndex → 再Init
    FlushAndReopen();
    EXPECT_EQ(cache_.EntryCount(), 2u);

    cache_.ClearAll();
    EXPECT_EQ(cache_.EntryCount(), 0u);
    EXPECT_EQ(cache_.TotalSize(), 0u);

    // ファイルも削除されていること
    MermaidFileCache::CacheEntry entry;
    MermaidFileCache::PngBlob out;
    EXPECT_FALSE(cache_.Lookup(1, entry, out));
    EXPECT_FALSE(cache_.Lookup(2, entry, out));
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
    wchar_t name[24];
    swprintf_s(name, L"%016llx.png", cache_.InternalKeyForTest(777ULL));
    EXPECT_TRUE(std::filesystem::exists(temp_dir_ / name));

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
    MermaidFileCache::PngBlob out;
    EXPECT_FALSE(uninit.Lookup(1, entry, out));

    uninit.SaveIndex();
    uninit.ClearAll();
    uninit.Shutdown();
}

// ═══════════════════════════════════════════════
// LRU — Lookupがタイムスタンプを更新する
// ═══════════════════════════════════════════════

TEST_F(MermaidFileCacheTest, LookupRefreshesTimestamp)
{
    cache_.SetLimits(2, 1ULL * 1024 * 1024 * 1024);
    InitCache();

    // key=1を最初に格納
    cache_.StoreAsync(1, 100.0f, 50.0f, MakeDummyPng(100));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // key=2を格納（key=1より新しいタイムスタンプ）
    cache_.StoreAsync(2, 100.0f, 50.0f, MakeDummyPng(100));
    scheduler_.Shutdown();

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    cache_.SetCacheDir(temp_dir_);
    cache_.SetLimits(2, 1ULL * 1024 * 1024 * 1024);
    cache_.SaveIndex();
    scheduler_.Init(2);
    cache_.Init(1.0f, scheduler_);

    // key=1をLookupしてタイムスタンプを更新（key=2より新しくなる）
    MermaidFileCache::CacheEntry entry;
    MermaidFileCache::PngBlob out;
    EXPECT_TRUE(cache_.Lookup(1, entry, out));

    // key=3を追加 → key=2が最古なのでkey=2が削除される
    cache_.StoreAsync(3, 100.0f, 50.0f, MakeDummyPng(100));
    EXPECT_EQ(cache_.EntryCount(), 2u);

    // 永続化後にも確認
    FlushAndReopen();

    EXPECT_TRUE(cache_.Lookup(1, entry, out));
    EXPECT_FALSE(cache_.Lookup(2, entry, out));
    EXPECT_TRUE(cache_.Lookup(3, entry, out));
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
    // LookupDimensionsはLRUタイムスタンプを更新しないことを確認する。
    // エビクション対象の選択に影響しないため、推定専用の軽量パスとして安全。
    cache_.SetLimits(2, 1ULL * 1024 * 1024 * 1024);
    InitCache();

    cache_.StoreAsync(1, 100.0f, 50.0f, MakeDummyPng(100));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    cache_.StoreAsync(2, 200.0f, 100.0f, MakeDummyPng(100));
    EXPECT_EQ(cache_.EntryCount(), 2u);

    // key=1をLookupDimensionsで参照（LRUは更新されないはず）
    MermaidFileCache::CacheEntry entry;
    EXPECT_TRUE(cache_.LookupDimensions(1, entry));

    // key=3を追加 → LRU未更新のkey=1が最古として削除されるはず
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
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
    MermaidFileCache::PngBlob out;
    cache_.Lookup(key, entry, out);

    // 書き込み in-flight 中なら index は残されているべき。
    // タスクが先行して完了した場合は LookupDimensions も成功する。
    EXPECT_TRUE(cache_.LookupDimensions(key, entry))
        << "pending write 中に index が消されている（orphan PNG の温床）";
}

// ClearAll 後、in-flight だった書き込みタスクが PNG ファイルを
// 「復活」させないこと（generation 再確認のリグレッションガード）。
TEST_F(MermaidFileCacheTest, ClearAllPreventsResurrectionOfInFlightWrites)
{
    InitCache();

    // 多数の StoreAsync を投入して in-flight を作りやすくする
    constexpr int N = 20;
    for (int i = 0; i < N; ++i) {
        cache_.StoreAsync(static_cast<uint64_t>(0xA000 + i), 100.0f, 80.0f, MakeDummyPng(256));
    }
    cache_.ClearAll();

    // すべてのバックグラウンドタスクを完了させる
    scheduler_.Shutdown();

    // 物理ファイルが残っていないこと（generation チェックで write がスキップされた）
    int leftover = 0;
    if (std::filesystem::exists(temp_dir_)) {
        for (const auto& dirent : std::filesystem::directory_iterator(temp_dir_)) {
            const auto ext = dirent.path().extension();
            if (ext == L".png") {
                ++leftover;
            }
        }
    }
    EXPECT_EQ(leftover, 0)
        << "ClearAll 後に in-flight の write が PNG を復活させている";

    // TearDown のためにスケジューラを再起動
    scheduler_.Init(2);
}

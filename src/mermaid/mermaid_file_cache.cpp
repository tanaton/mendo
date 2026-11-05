#include "mermaid_file_cache.h"
#include "mermaid_util.h"
#include "task_scheduler.h"
#include "file_io.h"
#include "utility.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace {

#pragma pack(push, 1)
struct IndexHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
};

struct IndexRecord {
    uint64_t key;
    float css_width;
    float css_height;
    uint32_t file_size;
    int64_t last_used;
};
#pragma pack(pop)

static_assert(sizeof(IndexHeader) == 12);
static_assert(sizeof(IndexRecord) == 28);

constexpr std::wstring_view kFileExtension = L".drawing";
constexpr std::wstring_view kIndexFileName = L"index.bin";
constexpr std::wstring_view kIndexLockKind = L"mermaid-index";

std::vector<std::filesystem::path> ListEntries(const std::filesystem::path& dir)
{
    std::vector<std::filesystem::path> entries;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        entries.push_back(it->path());
    }
    return entries;
}

} // namespace

MermaidFileCache::~MermaidFileCache()
{
    Shutdown();
}

void MermaidFileCache::SetCacheDir(const std::filesystem::path& dir)
{
    cache_dir_ = dir;
}

void MermaidFileCache::SetLimits(size_t max_entries, uint64_t max_total_size)
{
    max_entries_ = max_entries;
    max_total_size_ = max_total_size;
}

std::filesystem::path MermaidFileCache::FilePath(uint64_t key) const
{
    wchar_t name[32];
    const auto r = std::format_to_n(name, std::ranges::size(name) - 1, L"{:016x}{}", key, kFileExtension);
    return cache_dir_ / std::wstring_view{ name, static_cast<size_t>(r.out - name) };
}

std::filesystem::path MermaidFileCache::GetIndexPath() const
{
    return cache_dir_ / kIndexFileName;
}

void MermaidFileCache::Init(TaskScheduler& scheduler)
{
    scheduler_ = &scheduler;
    index_.reset();
}

MermaidFileCache::IndexState& MermaidFileCache::Index()
{
    if (!index_) {
        auto loaded = [this] {
            const InterProcessFileLock lock(kIndexLockKind, GetIndexPath());
            return LoadIndex(/* strict = */ false);
        }();
        if (loaded) {
            index_ = std::move(*loaded);
        }
        else {
            // 違う版 (古い描き方) や壊れた索引のファイルは引けないので、索引に載らないまま残さない。
            Clear();
        }
    }
    return *index_;
}

std::optional<MermaidFileCache::IndexState> MermaidFileCache::LoadIndex(bool strict) const
{
    IndexState index;
    DWORD read_error = ERROR_SUCCESS;
    const auto buf = ReadAllBytes(GetIndexPath().c_str(), &read_error);
    const size_t buf_size = buf.size();
    if (buf_size < sizeof(IndexHeader)) {
        if (strict && !IsFileMissingError(read_error)) {
            return std::nullopt;
        }
        return index;
    }

    const uint8_t* p = buf.data();
    IndexHeader header;
    std::memcpy(&header, p, sizeof(header));
    p += sizeof(header);

    if (header.magic != MAGIC || header.version != VERSION || header.count > DEFAULT_MAX_ENTRIES * 2) {
        return std::nullopt;
    }

    index.entries.reserve(header.count);

    for (uint32_t i = 0; i < header.count; ++i) {
        if (static_cast<size_t>(p - buf.data()) + sizeof(IndexRecord) > buf_size) {
            break;
        }

        IndexRecord record;
        std::memcpy(&record, p, sizeof(record));
        p += sizeof(record);

        if (record.css_width <= 0.0f || record.css_height <= 0.0f ||
            !std::isfinite(record.css_width) || !std::isfinite(record.css_height) ||
            record.file_size == 0) {
            continue;
        }

        index.Add(record.key, { record.css_width, record.css_height, record.file_size, record.last_used });
    }
    index.MarkSaved();
    return index;
}

std::optional<MermaidFileCache::IndexState> MermaidFileCache::MergeWithSavedIndex()
{
    auto merged = LoadIndex(/* strict = */ true);
    if (!merged) {
        return std::nullopt;
    }
    for (const uint64_t key : index_->removed) {
        if (const auto it = merged->entries.find(key); it != merged->entries.end()) {
            merged->Remove(it);
        }
    }
    // 2 つのプロセスの last_used は同じ値から数え始めるので、そのまま混ぜると順序が付かない。
    // このプロセスで足したか使ったエントリは、その順を保ったまま読み直した索引のどれよりも新しくする。
    const int64_t shift = merged->lru_seq - index_->saved_seq;
    for (const auto& [key, entry] : index_->entries) {
        if (entry.last_used > index_->saved_seq) {
            merged->Add(key, { entry.css_width, entry.css_height, entry.file_size, entry.last_used + shift });
        }
    }
    EvictIfNeeded(*merged, 0, 0);
    return merged;
}

void MermaidFileCache::IndexState::Add(uint64_t key, IndexEntry entry)
{
    const auto [it, inserted] = entries.try_emplace(key, entry);
    if (!inserted) {
        total_size -= std::min<uint64_t>(total_size, it->second.file_size);
        it->second = entry;
    }
    total_size += entry.file_size;
    lru_seq = std::max(lru_seq, entry.last_used);
    dirty = true;
}

void MermaidFileCache::IndexState::Remove(Map::iterator it)
{
    total_size -= std::min<uint64_t>(total_size, it->second.file_size);
    removed.insert(it->first);
    entries.erase(it);
    dirty = true;
}

void MermaidFileCache::SaveIndex()
{
    // 未読み込みなら変更も無いので、空の索引でディスクを上書きしない。
    if (!index_ || !index_->dirty) {
        return;
    }

    const auto path = GetIndexPath();
    PrepareWrite(path);

    const InterProcessFileLock lock(kIndexLockKind, path);
    // 他のウィンドウが先に閉じて書いた索引を、このプロセスが読んだ古い索引で消さない。読めなければ従来どおり丸ごと書く。
    auto merged = MergeWithSavedIndex();
    const IndexState& index = merged ? *merged : *index_;

    const uint32_t count = static_cast<uint32_t>(index.entries.size());

    const size_t buf_size = sizeof(IndexHeader) + count * sizeof(IndexRecord);
    auto buf = std::make_unique_for_overwrite<uint8_t[]>(buf_size);
    uint8_t* p = buf.get();

    IndexHeader header{ MAGIC, VERSION, count };
    std::memcpy(p, &header, sizeof(header));
    p += sizeof(header);

    for (const auto& [key, entry] : index.entries) {
        IndexRecord record{ key, entry.css_width, entry.css_height, entry.file_size, entry.last_used };
        std::memcpy(p, &record, sizeof(record));
        p += sizeof(record);
    }

    // 壊れた索引は LoadIndex が検証して捨てるので、durable な書き込みは要らない。
    if (AtomicWriteAllBytes(path, buf.get(), buf_size, /* durable = */ false)) {
        if (merged) {
            index_ = std::move(merged);
        }
        index_->MarkSaved();
    }
}

void MermaidFileCache::OnReadFailed(uint64_t key, DWORD read_error)
{
    if (!IsFileMissingError(read_error)) {
        return;
    }
    auto& index = Index();
    const auto it = index.entries.find(key);
    if (it == index.entries.end()) {
        return;
    }
    // StoreAsync 直後でバックグラウンド書き込みが in-flight なら「未着地＝stale」と
    // 早合点せず entry を保持する（次回 Lookup で着地する）。
    {
        const std::lock_guard lock(pending_mutex_);
        if (pending_writes_.contains(key)) {
            return;
        }
    }
    index.Remove(it);
}

std::optional<MermaidFileCache::Hit> MermaidFileCache::Find(uint64_t width_free_key, uint64_t sized_key, float max_width, bool touch)
{
    auto& index = Index();
    uint64_t key = width_free_key;
    auto it = index.entries.find(key);
    if (it == index.entries.end() || !mermaid_util::IsWidthFree(it->second.css_width, max_width)) {
        key = sized_key;
        it = index.entries.find(key);
        if (it == index.entries.end()) {
            return std::nullopt;
        }
    }
    if (touch) {
        index.Touch(it);
    }
    return Hit{
        key, { it->second.css_width, it->second.css_height }
    };
}

void MermaidFileCache::StoreAsync(uint64_t key, float css_width, float css_height, std::string data)
{
    if (data.empty()) {
        return;
    }
    auto& index = Index();

    const uint32_t file_size = static_cast<uint32_t>(data.size());

    // 既存キーの上書きは新規スロットを要さない。先に旧エントリを除去してから EvictIfNeeded を
    // 呼び、満杯時に無関係なエントリを巻き込んで削除しないようにする。
    if (const auto it = index.entries.find(key); it != index.entries.end()) {
        index.Remove(it);
    }

    EvictIfNeeded(index, 1, file_size);

    index.Add(key, { css_width, css_height, file_size, index.NextLruSeq() });

    if (!scheduler_) {
        return;
    }

    // 書き込み開始前に in-flight セットへ登録しておく。
    // 読み込みがファイル未着地で失敗しても OnReadFailed が index から消さないようにする。
    // 同じキーは同じ中身なので、書き込み中なら 2 本目は出さない (同じ図が文書に複数ある場合)。
    // 2 本目を出すと、先に着地した方が追い出し済みとして消した後に書かれ、索引に無いファイルが残る。
    {
        const std::lock_guard lock(pending_mutex_);
        const auto [it, inserted] = pending_writes_.insert_or_assign(key, false);
        if (!inserted) {
            return;
        }
    }

    scheduler_->Post([this, key, path = FilePath(key), data = std::move(data), latch_guard = latch_.Acquire()] {
        PrepareWrite(path);

        // 書き込み失敗時はキャッシュ未保存のままになるが、再生成可能なので致命的ではない
        (void)WriteAllBytes(path, data.data(), data.size());

        bool evicted = false;
        {
            const std::lock_guard lock(pending_mutex_);
            const auto node = pending_writes_.extract(key);
            evicted = node && node.mapped();
        }
        // 書き込み中に追い出されたので、索引に無い孤立ファイルとして残さない。
        if (evicted) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    });
}

void MermaidFileCache::EvictIfNeeded(IndexState& index, size_t new_entries, uint64_t new_bytes)
{
    bool doomed = false;
    while (!index.entries.empty() && (index.entries.size() + new_entries > max_entries_ || index.total_size + new_bytes > max_total_size_)) {
        const auto oldest = std::ranges::min_element(index.entries, {}, [](const auto& kv) static noexcept { return kv.second.last_used; });
        // 書き込み中なら着地後に worker が消す。ここで消しても後から書かれて孤立ファイルになる。
        bool pending = false;
        {
            const std::lock_guard lock(pending_mutex_);
            if (const auto it = pending_writes_.find(oldest->first); it != pending_writes_.end()) {
                it->second = true;
                pending = true;
            }
        }
        if (!pending) {
            const std::lock_guard lock(doomed_mutex_);
            doomed_files_.insert(FilePath(oldest->first).filename().native());
            doomed = true;
        }
        index.Remove(oldest);
    }
    if (doomed) {
        RemoveDoomedFilesAsync();
    }
}

void MermaidFileCache::Clear()
{
    {
        const std::lock_guard lock(pending_mutex_);
        std::ranges::fill(pending_writes_ | std::views::values, true);
    }
    // 消した索引を終了時に書き戻さないよう、読み込み直後と同じく dirty を落とす。
    index_.emplace();
    // 索引だけはその場で消す。worker が消すまで残すと、その前の保存が古い索引に重ねてしまい、
    // 後から消すと他のプロセスがその間に書いた索引まで消す。
    {
        const auto index_path = GetIndexPath();
        const InterProcessFileLock lock(kIndexLockKind, index_path);
        DeleteFileW(index_path.c_str());
    }
    // 満杯 (4096 件) を 1 件ずつ消すと UI が 1 秒近く止まるので worker で消す。
    // 索引に載っていないファイル (索引を書き戻す前に落ちたときなど) も残らないよう、フォルダを列挙して控える。
    // .png は版 5 までのファイル。
    std::unordered_set<std::wstring> names;
    for (const auto& path : ListEntries(cache_dir_)) {
        auto name = path.filename().native();
        if (name.ends_with(kFileExtension) || name.ends_with(L".png")) {
            names.insert(std::move(name));
        }
    }
    {
        const std::lock_guard lock(doomed_mutex_);
        doomed_files_.merge(names);
    }
    RemoveDoomedFilesAsync();
}

void MermaidFileCache::PrepareWrite(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::lock_guard lock(doomed_mutex_);
    doomed_files_.erase(path.filename().native());
}

void MermaidFileCache::RemoveDoomedFilesAsync()
{
    if (!scheduler_ || !scheduler_->Post([this, guard = latch_.Acquire()] { RemoveDoomedFiles(); })) {
        RemoveDoomedFiles();
    }
}

void MermaidFileCache::RemoveDoomedFiles()
{
    while (true) {
        // 消し終えるまでロックを持ち、PrepareWrite で外してから書くファイルと入れ違わない。
        const std::lock_guard lock(doomed_mutex_);
        if (doomed_files_.empty()) {
            return;
        }
        const auto name = doomed_files_.extract(doomed_files_.begin());
        std::error_code ec;
        std::filesystem::remove(cache_dir_ / name.value(), ec);
    }
}

std::filesystem::path MermaidFileCache::GetLegacyDataPath() const
{
    return cache_dir_.parent_path() / L"WebView2Data";
}

bool MermaidFileCache::HasLegacyData() const
{
    std::error_code ec;
    return !cache_dir_.empty() && std::filesystem::exists(GetLegacyDataPath(), ec);
}

void MermaidFileCache::RemoveLegacyDataAsync()
{
    // ファイルが多いので UI では消さない。
    if (scheduler_ && !cache_dir_.empty()) {
        scheduler_->Post([dir = GetLegacyDataPath()] {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        });
    }
}

void MermaidFileCache::Shutdown()
{
    // 書き込み中の worker は self のメンバ (pending_mutex_) を触るので完了まで待つ。
    // 待ちの書き込みも索引に載っているので取り消さない (App は先に scheduler を Shutdown して書き終えている)。
    latch_.Wait();
    scheduler_ = nullptr;
}

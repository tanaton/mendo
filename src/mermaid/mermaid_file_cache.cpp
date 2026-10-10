#include "mermaid_file_cache.h"
#include "profiler.h"
#include "task_scheduler.h"
#include "file_io.h"
#include "scope_guard.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>
#include <ranges>
#include <string_view>

namespace {

#pragma pack(push, 1)
struct IndexHeader {
    uint32_t magic;
    uint32_t version;
    float dpr;
    uint32_t count;
};

struct IndexRecord {
    uint64_t key;
    float css_width;
    float css_height;
    uint32_t png_size;
    int64_t last_used;
};
#pragma pack(pop)

static_assert(sizeof(IndexHeader) == 16);
static_assert(sizeof(IndexRecord) == 28);

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

std::filesystem::path MermaidFileCache::GetPngPath(const std::filesystem::path& dir, uint64_t key)
{
    wchar_t name[24];
    const auto r = std::format_to_n(name, std::ranges::size(name) - 1, L"{:016x}.png", key);
    return dir / std::wstring_view{ name, static_cast<size_t>(r.out - name) };
}

std::filesystem::path MermaidFileCache::GetPngPath(uint64_t key) const
{
    if (cache_dir_.empty()) {
        return {};
    }
    return GetPngPath(cache_dir_, key);
}

std::filesystem::path MermaidFileCache::GetIndexPath() const
{
    if (cache_dir_.empty()) {
        return {};
    }
    return cache_dir_ / L"index.bin";
}

void MermaidFileCache::Init(float current_dpr, TaskScheduler& scheduler)
{
    scheduler_ = &scheduler;
    // DPR ごとに InternalKey() が別キーに振り分けるため、DPR 不一致でも消去せず
    // LRU で自然淘汰させる。
    current_dpr_ = current_dpr;
    index_.reset();
}

MermaidFileCache::IndexState& MermaidFileCache::Index()
{
    if (!index_) {
        index_ = LoadIndex();
    }
    return *index_;
}

MermaidFileCache::IndexState MermaidFileCache::LoadIndex() const
{
    IndexState index;
    const auto path = GetIndexPath();
    if (path.empty()) {
        return index;
    }

    auto [buf, buf_size] = ReadAllBytes(path);
    if (!buf || buf_size < sizeof(IndexHeader)) {
        return index;
    }

    const uint8_t* p = buf.get();
    IndexHeader header;
    std::memcpy(&header, p, sizeof(header));
    p += sizeof(header);

    if (header.magic != MAGIC || header.version != VERSION) {
        return index;
    }
    // 異常なエントリ数を拒否
    if (header.count > DEFAULT_MAX_ENTRIES * 2) {
        return index;
    }

    index.entries.reserve(header.count);

    for (uint32_t i = 0; i < header.count; ++i) {
        if (static_cast<size_t>(p - buf.get()) + sizeof(IndexRecord) > buf_size) {
            break;
        }

        IndexRecord record;
        std::memcpy(&record, p, sizeof(record));
        p += sizeof(record);

        // 壊れたエントリを無視する
        if (record.css_width <= 0.0f || record.css_height <= 0.0f ||
            !std::isfinite(record.css_width) || !std::isfinite(record.css_height) ||
            record.png_size == 0) {
            continue;
        }

        index.Add(record.key, { record.css_width, record.css_height, record.png_size, record.last_used });
    }
    index.dirty = false;
    return index;
}

void MermaidFileCache::IndexState::Add(uint64_t key, IndexEntry entry)
{
    entries.insert_or_assign(key, entry);
    total_size += entry.png_size;
    lru_seq = std::max(lru_seq, entry.last_used);
    dirty = true;
}

void MermaidFileCache::IndexState::Remove(Map::iterator it) noexcept
{
    total_size -= std::min<uint64_t>(total_size, it->second.png_size);
    entries.erase(it);
    dirty = true;
}

void MermaidFileCache::SaveIndex()
{
    // 未読み込みなら変更も無いので、空の索引でディスクを上書きしない。
    const auto path = GetIndexPath();
    if (path.empty() || !index_ || !index_->dirty) {
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    const uint32_t count = static_cast<uint32_t>(index_->entries.size());

    const size_t buf_size = sizeof(IndexHeader) + count * sizeof(IndexRecord);
    auto buf = std::make_unique_for_overwrite<uint8_t[]>(buf_size);
    uint8_t* p = buf.get();

    IndexHeader header{ MAGIC, VERSION, current_dpr_, count };
    std::memcpy(p, &header, sizeof(header));
    p += sizeof(header);

    for (const auto& [key, entry] : index_->entries) {
        IndexRecord record{ key, entry.css_width, entry.css_height, entry.png_size, entry.last_used };
        std::memcpy(p, &record, sizeof(record));
        p += sizeof(record);
    }

    if (AtomicWriteAllBytes(path, buf.get(), buf_size)) {
        index_->dirty = false;
    }
}

bool MermaidFileCache::Lookup(uint64_t key, CacheEntry& entry, PngBlob& png)
{
    std::filesystem::path path;
    if (!LookupPath(key, entry, path)) {
        return false;
    }
    DWORD read_error = 0;
    auto [data, data_size] = ReadAllBytes(path, &read_error);
    if (!data) {
        OnReadFailed(key, read_error);
        return false;
    }
    png.data = std::move(data);
    png.size = data_size;
    return true;
}

bool MermaidFileCache::LookupPath(uint64_t key, CacheEntry& entry, std::filesystem::path& png_path)
{
    auto& index = Index();
    key = InternalKey(key);
    auto it = index.entries.find(key);
    if (it == index.entries.end()) {
        return false;
    }

    png_path = GetPngPath(key);
    if (png_path.empty()) {
        return false;
    }

    entry.css_width = it->second.css_width;
    entry.css_height = it->second.css_height;

    index.Touch(it);
    return true;
}

void MermaidFileCache::OnReadFailed(uint64_t key, DWORD read_error)
{
    if (read_error != ERROR_FILE_NOT_FOUND && read_error != ERROR_PATH_NOT_FOUND) {
        return;
    }
    auto& index = Index();
    key = InternalKey(key);
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

bool MermaidFileCache::LookupDimensions(uint64_t key, CacheEntry& entry)
{
    const auto& index = Index();
    key = InternalKey(key);
    const auto it = index.entries.find(key);
    if (it == index.entries.end()) {
        return false;
    }
    entry.css_width = it->second.css_width;
    entry.css_height = it->second.css_height;
    return true;
}

void MermaidFileCache::StoreAsync(uint64_t key, float css_width, float css_height, PngBytes png_data)
{
    if (!png_data || png_data->empty()) {
        return;
    }
    auto& index = Index();
    key = InternalKey(key);

    const uint32_t png_size = static_cast<uint32_t>(png_data->size());

    // 既存キーの上書きは新規スロットを要さない。先に旧エントリを除去してから EvictIfNeeded を
    // 呼び、満杯時に無関係なエントリを巻き込んで削除しないようにする。
    if (const auto it = index.entries.find(key); it != index.entries.end()) {
        index.Remove(it);
    }

    EvictIfNeeded(index, png_size);

    index.Add(key, { css_width, css_height, png_size, index.NextLruSeq() });

    if (!scheduler_) {
        return;
    }

    // 書き込み開始前に in-flight セットへ登録しておく。
    // Lookup がファイル未着地を stale 扱いで index から消すのを防ぐ。
    {
        const std::lock_guard lock(pending_mutex_);
        pending_writes_.insert(key);
    }

    const uint32_t gen = write_gen_.load();
    auto path = GetPngPath(key);
    const bool posted = scheduler_->Post([this, key, path = std::move(path), data = std::move(png_data), gen, latch_guard = latch_.Acquire()] {
        // タスク完遂・キャンセルどちらの場合も pending を必ず解除する
        auto guard = ScopeGuard([this, key] {
            const std::lock_guard lock(pending_mutex_);
            pending_writes_.erase(key);
        });

        if (write_gen_.load() != gen) {
            return;
        }

        if (path.empty()) {
            return;
        }

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        // WriteAllBytes 直前でも generation を再確認する。
        // ClearAll() / Shutdown() 後にディスクへ PNG を "復活" させて
        // しまう窓を最小化するための最後のガード。
        if (write_gen_.load() != gen) {
            return;
        }

        // 書き込み失敗時はキャッシュ未保存のままになるが、再生成可能なので致命的ではない
        (void)WriteAllBytes(path, data->data(), data->size());
    });
    if (!posted) {
        // lambda 本体が走らず body 内 ScopeGuard は構築されないため pending_writes_ は解除されない。
        // capture の latch::Guard は closure 破棄時に自動 Release されるので、ここでは Post 前に
        // insert した key だけを巻き戻す (残すと Lookup が stale 扱いを抑止し続ける)。
        const std::lock_guard lock(pending_mutex_);
        pending_writes_.erase(key);
    }
}

void MermaidFileCache::EvictIfNeeded(IndexState& index, uint32_t new_png_size)
{
    while (!index.entries.empty() && (index.entries.size() >= max_entries_ || index.total_size + new_png_size > max_total_size_)) {
        const auto oldest = std::ranges::min_element(index.entries, {}, [](const auto& kv) { return kv.second.last_used; });
        if (!cache_dir_.empty()) {
            std::error_code ec;
            std::filesystem::remove(GetPngPath(cache_dir_, oldest->first), ec);
        }
        index.Remove(oldest);
    }
}

void MermaidFileCache::ClearAll()
{
    // write_gen_ 進捗だけでは「ガード通過済みで WriteAllBytes 直前」の worker を止められず、
    // 削除後に PNG が "復活" して orphan としてディスクに残る。
    write_gen_.fetch_add(1);
    MENDO_PROFILE("MermaidFileCache::ClearAll::Wait");
    latch_.Wait();

    if (!cache_dir_.empty()) {
        std::error_code ec;
        for (const auto& [key, _] : Index().entries) {
            std::filesystem::remove(GetPngPath(cache_dir_, key), ec);
        }
        std::filesystem::remove(GetIndexPath(), ec);
    }

    index_.emplace();
}

void MermaidFileCache::Shutdown()
{
    // write_gen_ を進めて以後の worker は早期 return するが、走り始めた worker は self
    // メンバ (pending_mutex_, write_gen_) を触り続けるため完了まで待つ。
    write_gen_.fetch_add(1);
    latch_.Wait();
    scheduler_ = nullptr;
}

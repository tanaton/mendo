#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <memory_resource>

struct FileEntry {
    std::pmr::wstring full_path;
    bool is_current = false;
    bool is_directory = false;
    // ".." (親ディレクトリ) の項目。
    bool is_parent = false;

    // 戻り値は full_path の内部バッファを指す view。
    constexpr std::wstring_view GetDisplayName() const noexcept
    {
        if (is_parent) {
            return L"..";
        }
        const std::wstring_view full{ full_path };
        const auto pos = full.find_last_of(L"\\/");
        return (pos != full.npos) ? full.substr(pos + 1) : full;
    }
};

// 列挙は SetDirectory / Refresh 後の最初の GetEntries まで遅らせる。非表示のペインからは
// 呼ばれないため、起動時やファイルを開くたびの UI スレッド I/O が起きない。
class FileExplorer {
public:
    // 列挙を別スレッドに任せる先。dir を List で列挙し、結果を request と一緒に ApplyListing へ渡させる。
    // 設定すると GetEntries は列挙を待たずに今の一覧を返す。未設定なら GetEntries がその場で列挙する。
    using AsyncLister = std::move_only_function<void(std::pmr::wstring dir, uint32_t request)>;
    void SetAsyncLister(AsyncLister lister)
    {
        lister_ = std::move(lister);
    }
    // スレッド安全。".." を先頭に、ディレクトリ → Markdown ファイルの順に並べた一覧。
    static std::pmr::vector<FileEntry> List(std::wstring_view dir);
    // 最新の要求の結果なら一覧を差し替えて true。古い要求の結果や、その後に Refresh / SetDirectory された結果は捨てる。
    bool ApplyListing(uint32_t request, std::pmr::vector<FileEntry> entries);
    // 別スレッドの列挙を待っている。GetEntries は前の一覧のまま。
    constexpr bool IsListing() const noexcept
    {
        return listing_;
    }
    // 列挙を待つ間に頼まれた「現在のファイルを表示」を、一覧が届いたらやり直させる。別のフォルダへ移ると取り消す。
    constexpr void DeferReveal() noexcept
    {
        reveal_deferred_ = true;
    }
    constexpr bool TakeDeferredReveal() noexcept
    {
        return std::exchange(reveal_deferred_, false);
    }

    void SetDirectory(std::wstring_view dir_path);
    constexpr void Refresh() noexcept
    {
        stale_ = true;
    }
    const std::pmr::vector<FileEntry>& GetEntries() const;
    // 列挙し直すたびに進む。項目 index と組にすれば一覧中の項目を識別できる。
    constexpr uint32_t GetGeneration() const noexcept
    {
        return generation_;
    }
    // 以降の SetDirectory / Refresh でも強調表示が維持される。
    void SetCurrentFile(std::wstring_view path);
    constexpr const std::pmr::wstring& GetDirectory() const noexcept
    {
        return directory_;
    }

private:
    void ApplyCurrentFile() const;

    std::pmr::wstring directory_;
    std::pmr::wstring current_file_;
    // GetEntries で遅延列挙するため mutable。
    mutable std::pmr::vector<FileEntry> entries_;
    mutable uint32_t generation_ = 0;
    mutable bool stale_ = false;
    // 列挙を始めた回数。結果がどの要求のものかを見分ける。
    mutable uint32_t request_ = 0;
    mutable bool listing_ = false;
    bool reveal_deferred_ = false;
    mutable AsyncLister lister_;
};

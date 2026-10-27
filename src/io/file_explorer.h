#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct FileEntry {
    std::wstring full_path;
    bool is_current = false;
    bool is_directory = false;
    // ".." (親ディレクトリ) の項目。
    bool is_parent = false;

    // 戻り値は full_path の内部バッファを指す view。
    std::wstring_view GetDisplayName() const noexcept;
};

// 列挙は SetDirectory / Refresh 後の最初の EnsureListed まで遅らせる。表示中のペインを描くときと
// 一覧を見る操作だけが呼ぶので、ペインが非表示なら起動時やファイルを開くたびの I/O が起きない。
class FileExplorer {
public:
    // 列挙を別スレッドに任せる先。dir を List で列挙し、結果を request と一緒に ApplyListing へ渡させる。
    // 設定すると EnsureListed は列挙を待たずに戻り、一覧は届くまで前のまま。未設定なら EnsureListed がその場で列挙する。
    using AsyncLister = std::move_only_function<void(std::wstring dir, uint32_t request)>;
    void SetAsyncLister(AsyncLister lister)
    {
        lister_ = std::move(lister);
    }
    // スレッド安全。".." を先頭に、ディレクトリ → Markdown ファイルの順に並べた一覧。
    static std::vector<FileEntry> List(std::wstring_view dir);
    // 最新の要求の結果なら一覧を差し替えて true。古い要求の結果や、その後に Refresh / SetDirectory された結果は捨てる。
    bool ApplyListing(uint32_t request, std::vector<FileEntry> entries);
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
    // SetDirectory / Refresh の後なら列挙を始める。
    void EnsureListed();
    constexpr const std::vector<FileEntry>& GetEntries() const noexcept
    {
        return entries_;
    }
    // 列挙し直すたびに進む。項目 index と組にすれば一覧中の項目を識別できる。
    constexpr uint32_t GetGeneration() const noexcept
    {
        return generation_;
    }
    // 以降の SetDirectory / Refresh でも強調表示が維持される。
    void SetCurrentFile(std::wstring_view path);
    constexpr const std::wstring& GetDirectory() const noexcept
    {
        return directory_;
    }

private:
    void ApplyCurrentFile();
    void ReplaceEntries(std::vector<FileEntry> entries);

    std::wstring directory_;
    std::wstring current_file_;
    std::vector<FileEntry> entries_;
    uint32_t generation_ = 0;
    bool stale_ = false;
    // 列挙を始めた回数。結果がどの要求のものかを見分ける。
    uint32_t request_ = 0;
    bool listing_ = false;
    bool reveal_deferred_ = false;
    AsyncLister lister_;
};

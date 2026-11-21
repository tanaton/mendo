#include <gtest/gtest.h>
#include "config_service.h"
#include "string_convert.h"
#include "test_helpers.h"
#include "win32_util.h"
#include "win_handle.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <windows.h>
#include <ktmw32.h>
#include "pane_layout.h"

namespace {

// 子のキーも値も無いときだけ消す。
void DeleteKeyIfEmpty(const wchar_t* path)
{
    DWORD subkeys = 0;
    DWORD values = 0;
    {
        UniqueRegKey key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, key.put()) != ERROR_SUCCESS ||
            RegQueryInfoKeyW(key.get(), nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            return;
        }
    }
    if (subkeys == 0 && values == 0) {
        RegDeleteKeyW(HKEY_CURRENT_USER, path);
    }
}

// HKCU\Software\tanaton\mendo-test の下に、テストごとに別のキー (名前は UniqueTestName) を作って消す。
// 本人の設定 (Software\tanaton\mendo) には触らない。
class ScopedTestRegistryKey {
public:
    ScopedTestRegistryKey()
    {
        path_ = std::wstring(kParent) + L'\\' + string_convert::Utf8ToWide(UniqueTestName());
        RegDeleteTreeW(HKEY_CURRENT_USER, path_.c_str());
    }
    ~ScopedTestRegistryKey()
    {
        RegDeleteTreeW(HKEY_CURRENT_USER, path_.c_str());
        // テストのために作った親も、ほかに何も無ければ消す。
        DeleteKeyIfEmpty(kParent);
        DeleteKeyIfEmpty(L"Software\\tanaton");
    }
    ScopedTestRegistryKey(const ScopedTestRegistryKey&) = delete;
    ScopedTestRegistryKey& operator=(const ScopedTestRegistryKey&) = delete;

    const std::wstring& Path() const
    {
        return path_;
    }
    std::wstring Section(std::wstring_view name) const
    {
        return path_ + L'\\' + std::wstring(name);
    }
    bool Exists() const
    {
        UniqueRegKey key;
        return RegOpenKeyExW(HKEY_CURRENT_USER, path_.c_str(), 0, KEY_READ, key.put()) == ERROR_SUCCESS;
    }

private:
    static constexpr const wchar_t* kParent = L"Software\\tanaton\\mendo-test";
    std::wstring path_;
};

// regedit や .reg の取り込みのように、ConfigService を通さずに書く。
void WriteValue(const std::wstring& key_path, const wchar_t* name, DWORD type, const void* data, DWORD size)
{
    UniqueRegKey key;
    ASSERT_EQ(RegCreateKeyExW(HKEY_CURRENT_USER, key_path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, key.put(), nullptr), ERROR_SUCCESS);
    ASSERT_EQ(RegSetValueExW(key.get(), name, 0, type, static_cast<const BYTE*>(data), size), ERROR_SUCCESS);
}

void WriteDword(const std::wstring& key_path, const wchar_t* name, DWORD value)
{
    WriteValue(key_path, name, REG_DWORD, &value, sizeof(value));
}

void WriteString(const std::wstring& key_path, const wchar_t* name, const std::wstring& value, bool with_terminator = true)
{
    WriteValue(key_path, name, REG_SZ, value.c_str(), static_cast<DWORD>((value.size() + (with_terminator ? 1 : 0)) * sizeof(wchar_t)));
}

std::optional<DWORD> ReadDword(const std::wstring& key_path, const wchar_t* name)
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, key_path.c_str(), name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::wstring> ReadString(const std::wstring& key_path, const wchar_t* name)
{
    wchar_t buf[512];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, key_path.c_str(), name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return std::wstring(buf);
}

// 別のプロセスがコミットする前のトランザクションの代わり。key_path のキーは先に作っておく。
// コミットしなければ、最後のハンドル (tx_。key_ が先に閉じる) を閉じたときに巻き戻る。
class PendingTransaction {
public:
    PendingTransaction(const std::wstring& key_path, const wchar_t* name, DWORD value)
        : tx_(CreateTransaction(nullptr, nullptr, 0, 0, 0, 0, nullptr))
    {
        EXPECT_TRUE(tx_);
        EXPECT_EQ(RegOpenKeyTransactedW(HKEY_CURRENT_USER, key_path.c_str(), 0, KEY_SET_VALUE, key_.put(), tx_.get(), nullptr), ERROR_SUCCESS);
        EXPECT_EQ(RegSetValueExW(key_.get(), name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)), ERROR_SUCCESS);
    }

    DWORD Commit()
    {
        key_.reset();
        return CommitTransaction(tx_.get()) ? ERROR_SUCCESS : GetLastError();
    }

private:
    UniqueHandle tx_;
    UniqueRegKey key_;
};

using FlushStep = ConfigService::FlushStep;
using StepLog = std::vector<FlushStep>;

} // namespace

class ConfigServiceTest : public ::testing::Test {
protected:
    ScopedTestRegistryKey key_;
    ConfigService config_{ key_.Path() };

    // 同じ場所を使う別のインスタンス (別のウィンドウの代わり)。Flush 後に Load してラウンドトリップを検証する。
    ConfigService MakeFreshLoadedConfig() const
    {
        ConfigService c(key_.Path());
        c.Load();
        return c;
    }

    StepLog steps_;

    // config_ の Flush が呼んだ段を steps_ に記録し、on_step が返す値で OS の関数を失敗させる。
    template <class F>
    void HookFlush(F on_step)
    {
        config_.SetFlushHookForTest([this, on_step](FlushStep step) -> long {
            steps_.push_back(step);
            return on_step(step);
        });
    }
};

// 無いキーは既定値。キーが無い場所や、型の違う値 (regedit で書き換えたものなど) を読んだ後も同じ。
TEST_F(ConfigServiceTest, MissingKeysReturnDefaults)
{
    const auto expect_defaults = [](const ConfigService& config) {
        EXPECT_TRUE(config.LoadBool(L"View", L"DarkMode", true));
        EXPECT_FALSE(config.LoadBool(L"View", L"DarkMode", false));
        EXPECT_EQ(config.LoadInt(L"Window", L"Width", 7, 0, 100), 7);
        EXPECT_TRUE(config.LoadWString(L"Session", L"LastFile").empty());
    };
    {
        SCOPED_TRACE("not loaded");
        expect_defaults(config_);
    }
    {
        SCOPED_TRACE("no key");
        config_.Load();
        expect_defaults(config_);
    }
    {
        SCOPED_TRACE("wrong types");
        WriteString(key_.Section(L"View"), L"DarkMode", L"1");
        const BYTE binary[] = { 1, 2, 3 };
        WriteValue(key_.Section(L"Window"), L"Width", REG_BINARY, binary, sizeof(binary));
        WriteDword(key_.Section(L"Session"), L"LastFile", 5);
        config_.Load();
        expect_defaults(config_);
    }
}

// .reg で入れたような値を読む。整数は REG_DWORD (負の値は 2 の補数)、文字列は REG_SZ で、終端の NUL は無くてもよい。
TEST_F(ConfigServiceTest, LoadReadsValuesWrittenOutside)
{
    WriteDword(key_.Section(L"Window"), L"X", static_cast<DWORD>(-100));
    WriteDword(key_.Section(L"View"), L"DarkMode", 1);
    WriteString(key_.Section(L"General"), L"Language", L"ko", false);
    WriteString(key_.Section(L"Session"), L"LastFile", L"C:\\テスト\\a.md");
    config_.Load();
    EXPECT_EQ(config_.LoadInt(L"Window", L"X", 0, -100000, 100000), -100);
    EXPECT_TRUE(config_.LoadBool(L"View", L"DarkMode", false));
    EXPECT_EQ(config_.LoadWString(L"General", L"Language"), L"ko");
    EXPECT_EQ(config_.LoadWString(L"Session", L"LastFile"), L"C:\\テスト\\a.md");
}

// 同じキーへ順に書き、上書きも確かめる。既定値と逆の値を読めれば保存値が返っている。
TEST_F(ConfigServiceTest, SaveBoolOverwrites)
{
    config_.SaveBool(L"Test", L"Flag", true);
    EXPECT_TRUE(config_.LoadBool(L"Test", L"Flag", false));
    config_.SaveBool(L"Test", L"Flag", false);
    EXPECT_FALSE(config_.LoadBool(L"Test", L"Flag", true));
}

// 範囲は両端を含み、範囲外の保存値は既定値に倒す。
TEST_F(ConfigServiceTest, LoadIntReturnsDefaultOutsideRange)
{
    struct Case {
        int stored;
        int min_v;
        int max_v;
        int expected;
    };
    constexpr Case kCases[] = {
        { 42, 0, 100, 42 },
        { 0, 0, 100, 0 },
        { 100, 0, 100, 100 },
        { -10, -20, 20, -10 },
        { -5, 0, 100, 7 },
        { 200, 0, 100, 7 },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.stored);
        config_.SaveInt(L"Test", L"Value", c.stored);
        EXPECT_EQ(config_.LoadInt(L"Test", L"Value", 7, c.min_v, c.max_v), c.expected);
    }
}

// 同じキーへ順に書き、上書きと空文字列の保存も確かめる。
TEST_F(ConfigServiceTest, SaveWStringOverwrites)
{
    for (const std::wstring_view value : { L"C:\\Users\\test\\document.md", L"C:\\ユーザー\\テスト\\文書.md", L"" }) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ value }));
        config_.SaveWString(L"Session", L"LastFile", value);
        EXPECT_EQ(config_.LoadWString(L"Session", L"LastFile"), value);
    }
}

// 真偽値と整数は REG_DWORD、文字列は REG_SZ で書く (.reg で初期値を入れる手順はこの型に合わせる)。
TEST_F(ConfigServiceTest, FlushAndLoadRoundTrip)
{
    config_.SaveBool(L"View", L"DarkMode", true);
    config_.SaveInt(L"Window", L"X", -500);
    config_.SaveWString(L"Session", L"LastFile", L"C:\\テスト\\file.md");
    config_.SaveWString(L"General", L"Language", L"");
    config_.Flush();

    EXPECT_EQ(ReadDword(key_.Section(L"View"), L"DarkMode"), 1u);
    EXPECT_EQ(ReadDword(key_.Section(L"Window"), L"X"), static_cast<DWORD>(-500));
    EXPECT_EQ(ReadString(key_.Section(L"Session"), L"LastFile"), L"C:\\テスト\\file.md");

    auto fresh = MakeFreshLoadedConfig();
    EXPECT_TRUE(fresh.LoadBool(L"View", L"DarkMode", false));
    EXPECT_EQ(fresh.LoadInt(L"Window", L"X", 0, -100000, 100000), -500);
    EXPECT_EQ(fresh.LoadWString(L"Session", L"LastFile"), L"C:\\テスト\\file.md");
    EXPECT_TRUE(fresh.LoadWString(L"General", L"Language").empty());
}

TEST_F(ConfigServiceTest, FlushSkipsWriteWhenUnchanged)
{
    WriteDword(key_.Section(L"Window"), L"X", 42);
    config_.Load();
    config_.SaveInt(L"Window", L"X", 42);
    // 書き戻されれば 42 に戻る。
    WriteDword(key_.Section(L"Window"), L"X", 7);
    config_.Flush();

    EXPECT_EQ(ReadDword(key_.Section(L"Window"), L"X"), 7u);
}

TEST_F(ConfigServiceTest, FlushWithoutAnySaveCreatesNoKey)
{
    config_.Load();
    config_.Flush();
    EXPECT_FALSE(key_.Exists());
}

// MENDO_SETTINGS_KEY があれば、本人の設定と別のテスト用の場所を使う。
TEST(DefaultRegistryPathTest, FollowsEnvironmentVariable)
{
    const std::wstring previous = QueryPath([](wchar_t* b, UINT n) static { return GetEnvironmentVariableW(L"MENDO_SETTINGS_KEY", b, n); });
    SetEnvironmentVariableW(L"MENDO_SETTINGS_KEY", nullptr);
    EXPECT_EQ(DefaultRegistryPath(), L"Software\\tanaton\\mendo");
    SetEnvironmentVariableW(L"MENDO_SETTINGS_KEY", L"shot1");
    EXPECT_EQ(DefaultRegistryPath(), L"Software\\tanaton\\mendo-test\\shot1");
    SetEnvironmentVariableW(L"MENDO_SETTINGS_KEY", previous.empty() ? nullptr : previous.c_str());
}

// ---- 複数のウィンドウ (プロセス) ----

// 先に閉じたウィンドウが変えた値を、後から閉じたウィンドウが起動時の値で戻さない。
TEST_F(ConfigServiceTest, FlushKeepsValuesWrittenByOtherInstance)
{
    WriteDword(key_.Section(L"View"), L"DarkMode", 0);
    WriteString(key_.Section(L"General"), L"Language", L"ja");
    auto a = MakeFreshLoadedConfig();
    auto b = MakeFreshLoadedConfig();

    a.SaveBool(L"View", L"DarkMode", true);
    a.Flush();
    // 終了時は変えていない値も保存するので、同じ値の保存では記録しないことも確かめる。
    b.SaveBool(L"View", L"DarkMode", false);
    b.SaveWString(L"General", L"Language", L"en");
    b.Flush();

    auto fresh = MakeFreshLoadedConfig();
    EXPECT_TRUE(fresh.LoadBool(L"View", L"DarkMode", false));
    EXPECT_EQ(fresh.LoadWString(L"General", L"Language"), L"en");
}

// ウィンドウの位置と大きさは、最後に書いたウィンドウの組を残す。
TEST_F(ConfigServiceTest, FlushWritesWindowSectionAsWhole)
{
    for (const auto& [name, value] : { std::pair{ L"X", 0 }, { L"Y", 0 }, { L"Width", 800 }, { L"Height", 600 } }) {
        WriteDword(key_.Section(L"Window"), name, value);
    }
    auto a = MakeFreshLoadedConfig();
    auto b = MakeFreshLoadedConfig();

    a.SaveInt(L"Window", L"Width", 1000);
    a.Flush();
    b.SaveInt(L"Window", L"X", 50);
    b.SaveInt(L"Window", L"Width", 800);
    b.Flush();

    auto fresh = MakeFreshLoadedConfig();
    EXPECT_EQ(fresh.LoadInt(L"Window", L"X", -1, -100000, 100000), 50);
    EXPECT_EQ(fresh.LoadInt(L"Window", L"Width", -1, 0, 100000), 800);
}

// 開いていたファイルとスクロール位置は組で残す。別のファイルに他のウィンドウのスクロール位置を付けない。
TEST_F(ConfigServiceTest, FlushWritesSessionSectionAsWhole)
{
    WriteString(key_.Section(L"Session"), L"LastFile", L"C:\\foo.md");
    WriteDword(key_.Section(L"Session"), L"ScrollNode", 0);
    WriteDword(key_.Section(L"Session"), L"ScrollOffset", 0);
    auto a = MakeFreshLoadedConfig();
    auto b = MakeFreshLoadedConfig();

    session::SaveScrollPosition(a, 10, 0.0f);
    a.Flush();
    session::SaveLastFilePath(b, L"C:\\bar.md");
    session::SaveScrollPosition(b, 0, 0.0f);
    b.Flush();

    auto fresh = MakeFreshLoadedConfig();
    EXPECT_EQ(fresh.LoadWString(L"Session", L"LastFile"), L"C:\\bar.md");
    EXPECT_EQ(session::LoadScrollPosition(fresh).node, 0);
}

// 他のバージョンが書いたキーなど、このインスタンスが知らないキーを消さない。
TEST_F(ConfigServiceTest, FlushKeepsUnknownKeysInWholeSection)
{
    auto a = MakeFreshLoadedConfig();
    WriteDword(key_.Section(L"Window"), L"X", 0);
    WriteDword(key_.Section(L"Window"), L"Future", 1);

    a.SaveInt(L"Window", L"X", 5);
    a.Flush();

    auto fresh = MakeFreshLoadedConfig();
    EXPECT_EQ(fresh.LoadInt(L"Window", L"X", -1, -100000, 100000), 5);
    EXPECT_EQ(fresh.LoadInt(L"Window", L"Future", -1, 0, 10), 1);
}

// 書けなかった変更は失わず、次の Flush で他の値を消さずに書く。
TEST_F(ConfigServiceTest, FlushKeepsChangesAfterFailedWrite)
{
    WriteString(key_.Section(L"General"), L"Language", L"ja");
    config_.Load();
    config_.SaveBool(L"View", L"DarkMode", true);
    config_.SetFlushHookForTest([](FlushStep) { return ERROR_ACCESS_DENIED; });
    config_.Flush();
    EXPECT_FALSE(ReadDword(key_.Section(L"View"), L"DarkMode"));

    config_.SetFlushHookForTest(nullptr);
    config_.Flush();

    auto fresh = MakeFreshLoadedConfig();
    EXPECT_TRUE(fresh.LoadBool(L"View", L"DarkMode", false));
    EXPECT_EQ(fresh.LoadWString(L"General", L"Language"), L"ja");
}

// ---- トランザクションの失敗 ----

// 別のプロセスが同じキーを書いている最中 (ERROR_TRANSACTIONAL_CONFLICT) は待って書き直し、相手の値も残す。
TEST_F(ConfigServiceTest, FlushRetriesWhileAnotherTransactionWritesSameKey)
{
    WriteDword(key_.Section(L"View"), L"DarkMode", 0);
    config_.Load();
    config_.SaveBool(L"View", L"DarkMode", true);
    PendingTransaction other(key_.Section(L"View"), L"Other", 1);
    int transacted = 0;
    HookFlush([&transacted, &other](FlushStep step) {
        // 1 回目は相手と衝突し、2 回目の前に相手がコミットする。
        if (step == FlushStep::TransactedKey && ++transacted == 2) {
            EXPECT_EQ(other.Commit(), static_cast<DWORD>(ERROR_SUCCESS));
        }
        return ERROR_SUCCESS;
    });
    config_.Flush();

    EXPECT_EQ(steps_, (StepLog{ FlushStep::TransactedKey, FlushStep::TransactedKey, FlushStep::Commit }));
    EXPECT_EQ(ReadDword(key_.Section(L"View"), L"DarkMode"), 1u);
    EXPECT_EQ(ReadDword(key_.Section(L"View"), L"Other"), 1u);
}

// 衝突が続けばトランザクション無しで書く。相手のトランザクションは中止される。
TEST_F(ConfigServiceTest, FlushWritesDirectlyWhenConflictPersists)
{
    WriteDword(key_.Section(L"View"), L"DarkMode", 0);
    config_.Load();
    config_.SaveBool(L"View", L"DarkMode", true);
    PendingTransaction other(key_.Section(L"View"), L"Other", 1);
    HookFlush([](FlushStep) { return ERROR_SUCCESS; });
    config_.Flush();

    // 書き直した末に、コミットせずトランザクション無しで書いた。
    EXPECT_GT(std::ranges::count(steps_, FlushStep::TransactedKey), 1);
    EXPECT_FALSE(std::ranges::contains(steps_, FlushStep::Commit));
    ASSERT_FALSE(steps_.empty());
    EXPECT_EQ(steps_.back(), FlushStep::DirectKey);
    EXPECT_EQ(ReadDword(key_.Section(L"View"), L"DarkMode"), 1u);
    EXPECT_EQ(other.Commit(), static_cast<DWORD>(ERROR_TRANSACTION_ALREADY_ABORTED));
    EXPECT_FALSE(ReadDword(key_.Section(L"View"), L"Other"));
}

// 書いている間に regedit などがトランザクション無しで書くと、コミットが ERROR_TRANSACTION_ALREADY_ABORTED になる。書き直す。
TEST_F(ConfigServiceTest, FlushRetriesWhenAbortedByDirectWrite)
{
    WriteDword(key_.Section(L"View"), L"DarkMode", 0);
    config_.Load();
    config_.SaveBool(L"View", L"DarkMode", true);
    const std::wstring view = key_.Section(L"View");
    bool written = false;
    HookFlush([&written, &view](FlushStep step) {
        // 最初のコミットの前にだけ書く。
        if (step == FlushStep::Commit && !std::exchange(written, true)) {
            WriteDword(view, L"External", 1);
        }
        return ERROR_SUCCESS;
    });
    config_.Flush();

    EXPECT_EQ(steps_, (StepLog{ FlushStep::TransactedKey, FlushStep::Commit, FlushStep::TransactedKey, FlushStep::Commit }));
    EXPECT_EQ(ReadDword(view, L"DarkMode"), 1u);
    EXPECT_EQ(ReadDword(view, L"External"), 1u);
}

// トランザクションが使えない環境 (FSLogix のプロファイルなど) では、書き直さずにトランザクション無しで書く。
TEST_F(ConfigServiceTest, FlushWritesDirectlyWhenTransactionUnavailable)
{
    config_.SaveBool(L"View", L"DarkMode", true);
    config_.SaveInt(L"Window", L"X", 3);
    HookFlush([](FlushStep step) {
        return step == FlushStep::TransactedKey ? ERROR_RM_NOT_ACTIVE : ERROR_SUCCESS;
    });
    config_.Flush();

    EXPECT_EQ(steps_, (StepLog{ FlushStep::TransactedKey, FlushStep::DirectKey, FlushStep::DirectKey }));
    EXPECT_EQ(ReadDword(key_.Section(L"View"), L"DarkMode"), 1u);
    EXPECT_EQ(ReadDword(key_.Section(L"Window"), L"X"), 3u);
}

class SessionTest : public TempDirTestBase {
protected:
    ConfigService config_;

    PaneController LoadPanes() const
    {
        PaneController panes;
        session::LoadPanes(config_, panes);
        return panes;
    }
};

// 表示・幅・倍率はペインごとに往復する。幅はズームを含まない DIP 値で、読み側は倍率を掛けない。
TEST_F(SessionTest, PanesRoundTrip)
{
    PaneController saved;
    saved.SetSidePaneVisible(PaneTarget::File, false);
    saved.SetSidePaneWidth(PaneTarget::File, 300.0f);
    saved.SetSidePaneWidth(PaneTarget::Toc, 260.0f);
    saved.SidePaneZoom(PaneTarget::File).SetIndex(ZOOM_DEFAULT_INDEX + 2);
    saved.SidePaneZoom(PaneTarget::Toc).SetIndex(ZOOM_DEFAULT_INDEX - 1);
    session::SavePanes(config_, saved);

    const auto loaded = LoadPanes();

    EXPECT_FALSE(loaded.IsSidePaneVisible(PaneTarget::File));
    EXPECT_TRUE(loaded.IsSidePaneVisible(PaneTarget::Toc));
    EXPECT_FLOAT_EQ(loaded.GetSidePaneWidth(PaneTarget::File), 300.0f);
    EXPECT_FLOAT_EQ(loaded.GetSidePaneWidth(PaneTarget::Toc), 260.0f);
    EXPECT_EQ(loaded.SidePaneZoom(PaneTarget::File).Index(), ZOOM_DEFAULT_INDEX + 2);
    EXPECT_EQ(loaded.SidePaneZoom(PaneTarget::Toc).Index(), ZOOM_DEFAULT_INDEX - 1);
}

// 読み込み先の今の状態は残さない。
TEST_F(SessionTest, LoadPanesOverwritesCurrentState)
{
    PaneController saved;
    saved.SetSidePaneVisible(PaneTarget::Toc, false);
    session::SavePanes(config_, saved);

    PaneController panes;
    panes.SetSidePaneVisible(PaneTarget::File, false);
    panes.SetSidePaneWidth(PaneTarget::File, 400.0f);
    session::LoadPanes(config_, panes);

    EXPECT_TRUE(panes.IsSidePaneVisible(PaneTarget::File));
    EXPECT_FALSE(panes.IsSidePaneVisible(PaneTarget::Toc));
    EXPECT_FLOAT_EQ(panes.GetSidePaneWidth(PaneTarget::File), SIDE_PANE_DEFAULT_WIDTH);
}

TEST_F(SessionTest, LoadPanesKeepsSavedWidthWithoutWindowClamp)
{
    // 過剰幅は PaneController::ComputeLayout が表示幅だけを縮めるので、LoadPanes はウィンドウ幅で clamp せず保存値を
    // そのまま使う。clamp すると狭いウィンドウで起動したときに保存値が min へ潰れ、終了時の再保存で失われる。
    config_.SaveInt(L"Pane", L"FileWidth", 500);
    config_.SaveInt(L"Pane", L"TocWidth", 500);

    const auto loaded = LoadPanes();

    EXPECT_FLOAT_EQ(loaded.GetSidePaneWidth(PaneTarget::File), 500.0f);
    EXPECT_FLOAT_EQ(loaded.GetSidePaneWidth(PaneTarget::Toc), 500.0f);
}

// 何も保存していなければ、ペインは両方表示・既定の幅・等倍、スクロール位置とファイルは無し。
TEST_F(SessionTest, LoadsDefaultsWhenNothingSaved)
{
    const auto panes = LoadPanes();
    for (const auto target : { PaneTarget::File, PaneTarget::Toc }) {
        SCOPED_TRACE(static_cast<int>(target));
        EXPECT_TRUE(panes.IsSidePaneVisible(target));
        EXPECT_FLOAT_EQ(panes.GetSidePaneWidth(target), SIDE_PANE_DEFAULT_WIDTH);
        EXPECT_TRUE(panes.SidePaneZoom(target).IsDefault());
    }
    const auto pos = session::LoadScrollPosition(config_);
    EXPECT_EQ(pos.node, -1);
    EXPECT_EQ(pos.offset, 0);
    EXPECT_TRUE(session::LoadLastFilePath(config_).empty());
}

// offset は丸めて保存する。ノード先頭より上を見ている (scroll_y < node_y) と offset は負になる。
TEST_F(SessionTest, SaveAndLoadScrollPosition)
{
    struct Case {
        int node;
        float offset;
        int expected_offset;
    };
    // ノードの無い文書は -1 で書き、前のファイルの位置を消す (App::SaveSession)。
    constexpr Case kCases[] = {
        { 5, 49.6f,   50   },
        { 3, -150.0f, -150 },
        { -1, 0.0f,   0    },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.offset);
        session::SaveScrollPosition(config_, c.node, c.offset);
        const auto pos = session::LoadScrollPosition(config_);
        EXPECT_EQ(pos.node, c.node);
        EXPECT_EQ(pos.offset, c.expected_offset);
    }
}

// 100 万ノードを超える文書や、巨大な表の奥 (数千万 DIP) の位置も戻す。int に収まらない offset は端に寄せる。
TEST_F(SessionTest, SaveAndLoadScrollPosition_LargeValues)
{
    struct Case {
        int node;
        double offset;
        int expected_offset;
    };
    constexpr Case kCases[] = {
        { 2000000, 3.0e7 + 0.4, 30000000 },
        { 1, 1.0e10, std::numeric_limits<int>::max() },
        { 1, -1.0e10, std::numeric_limits<int>::min() },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.offset);
        session::SaveScrollPosition(config_, c.node, c.offset);
        const auto pos = session::LoadScrollPosition(config_);
        EXPECT_EQ(pos.node, c.node);
        EXPECT_EQ(pos.offset, c.expected_offset);
    }
}

// node は -1 未満を既定値 (-1) に倒す。
TEST_F(SessionTest, LoadScrollPosition_NegativeNodeFallsBackToDefault)
{
    config_.SaveInt(L"Session", L"ScrollNode", -5);
    EXPECT_EQ(session::LoadScrollPosition(config_).node, -1);
}

// 保存したパスを読み戻し、空のパスでは上書きしない。LoadLastFilePath は実在しないパスを弾くので実ファイルを作る。
TEST_F(SessionTest, SaveLastFilePathSkipsEmpty)
{
    const auto file = WriteTempFile(L"last.md", "# last").wstring();
    session::SaveLastFilePath(config_, file);
    session::SaveLastFilePath(config_, L"");
    EXPECT_EQ(session::LoadLastFilePath(config_), std::wstring_view{ file });
}

// UNC (\\host)・拡張パス (\\?\)・デバイスパス (\\.\) と、存在しないファイルは返さない。
TEST_F(SessionTest, LoadLastFilePathRejectsRemoteDeviceAndMissingPaths)
{
    // 存在しないファイルは temp_dir_ 配下の作らない名前にし、絶対パスへのフレークを避ける。
    const std::wstring kPaths[] = {
        L"\\\\server\\share\\file.md",
        L"\\\\?\\C:\\file.md",
        L"\\\\.\\PhysicalDrive0",
        (temp_dir_ / L"never_created.md").wstring(),
    };
    for (const auto& saved : kPaths) {
        SCOPED_TRACE(::testing::PrintToString(saved));
        config_.SaveWString(L"Session", L"LastFile", saved);
        EXPECT_TRUE(session::LoadLastFilePath(config_).empty());
    }
}

// 実在するファイルを指すデバイスパスも弾く。存在しないパスでは存在確認で空になり、判定を外しても通ってしまう。
TEST_F(SessionTest, LoadLastFilePathRejectsDeviceFormsOfExistingFile)
{
    const auto file = WriteTempFile(L"last.md", "# last").wstring();
    for (const std::wstring& prefixed : { L"\\\\?\\" + file, L"\\\\.\\" + file, L"//./" + file, L"\\??\\" + file }) {
        SCOPED_TRACE(::testing::PrintToString(prefixed));
        ASSERT_NE(::GetFileAttributesW(prefixed.c_str()), INVALID_FILE_ATTRIBUTES) << "前提: このパスでファイルを開けること";
        config_.SaveWString(L"Session", L"LastFile", prefixed);
        EXPECT_TRUE(session::LoadLastFilePath(config_).empty());
    }
}

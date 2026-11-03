#include <gtest/gtest.h>
#include "config_service.h"
#include "example_files.h"
#include "test_helpers.h"
#include "win_handle.h"
#include <filesystem>
#include <string>
#include <string_view>
#include "pane_layout.h"

class ConfigServiceTest : public TempDirTestBase {
protected:
    ConfigService config_;

    void SetUp() override
    {
        config_.SetConfigDir(temp_dir_);
    }

    // 同じ temp_dir_ を共有する別インスタンス。Flush 後に Load してラウンドトリップを検証する。
    ConfigService MakeFreshLoadedConfig()
    {
        ConfigService c;
        c.SetConfigDir(temp_dir_);
        c.Load();
        return c;
    }
};

// ---- GetConfigPath ----

TEST_F(ConfigServiceTest, GetConfigPathDefaultsToMendoFolder)
{
    const ConfigService config;
    const auto dir = config.GetConfigPath(L"x").parent_path();
    EXPECT_FALSE(dir.empty());
    EXPECT_TRUE(dir.wstring().contains(L"mendo"));
}

TEST_F(ConfigServiceTest, GetConfigPathCombinesCorrectly)
{
    auto path = config_.GetConfigPath(L"test.txt");
    EXPECT_EQ(path, temp_dir_ / L"test.txt");
}

// ---- 値の保存と読み出し ----

TEST_F(ConfigServiceTest, MissingKeysReturnDefaults)
{
    EXPECT_TRUE(config_.LoadBool("Test", "Missing", true));
    EXPECT_FALSE(config_.LoadBool("Test", "Missing", false));
    EXPECT_EQ(config_.LoadInt("Test", "Missing", 7, 0, 100), 7);
    EXPECT_TRUE(config_.LoadWString("Test", "Missing").empty());
}

// 同じキーへ順に書き、上書きも確かめる。既定値と逆の値を読めれば保存値が返っている。
TEST_F(ConfigServiceTest, SaveBoolOverwrites)
{
    config_.SaveBool("Test", "Flag", true);
    EXPECT_TRUE(config_.LoadBool("Test", "Flag", false));
    config_.SaveBool("Test", "Flag", false);
    EXPECT_FALSE(config_.LoadBool("Test", "Flag", true));
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
        config_.SaveInt("Test", "Value", c.stored);
        EXPECT_EQ(config_.LoadInt("Test", "Value", 7, c.min_v, c.max_v), c.expected);
    }
}

// 同じキーへ順に書き、上書きと空文字列の保存も確かめる。
TEST_F(ConfigServiceTest, SaveWStringOverwrites)
{
    for (const std::wstring_view value : { L"C:\\Users\\test\\document.md", L"C:\\ユーザー\\テスト\\文書.md", L"" }) {
        SCOPED_TRACE(::testing::PrintToString(std::wstring{ value }));
        config_.SaveWString("Session", "LastFile", value);
        EXPECT_EQ(config_.LoadWString("Session", "LastFile"), value);
    }
}

// ---- Load / Flush ラウンドトリップ ----

TEST_F(ConfigServiceTest, FlushAndLoadRoundTrip)
{
    config_.SaveBool("View", "DarkMode", true);
    config_.SaveInt("Window", "X", -500);
    config_.SaveWString("Session", "LastFile", L"C:\\テスト\\file.md");
    config_.Flush();

    auto fresh = MakeFreshLoadedConfig();
    EXPECT_TRUE(fresh.LoadBool("View", "DarkMode", false));
    EXPECT_EQ(fresh.LoadInt("Window", "X", 0, -100000, 100000), -500);
    EXPECT_EQ(fresh.LoadWString("Session", "LastFile"), L"C:\\テスト\\file.md");
}

TEST_F(ConfigServiceTest, LoadEmptyDirectoryProducesDefaults)
{
    config_.Load();
    EXPECT_EQ(config_.LoadInt("Window", "Width", 0, 100, 100000), 0);
    EXPECT_FALSE(config_.LoadBool("View", "DarkMode", false));
}

TEST_F(ConfigServiceTest, LoadSkipsUtf8Bom)
{
    // メモ帳等の BOM 付き UTF-8 で手編集された settings.ini でも
    // 先頭セクションが無言で失われないこと
    WriteTempFile(L"settings.ini", "\xEF\xBB\xBF[Window]\r\nX=42\r\n");
    config_.Load();
    EXPECT_EQ(config_.LoadInt("Window", "X", 0, 0, 100), 42);
}

TEST_F(ConfigServiceTest, FlushSkipsWriteWhenUnchanged)
{
    // Serialize と異なる改行コードの内容がそのまま残っていれば書き戻されていない
    const std::string original = "[Window]\r\nX=42\r\n";
    const auto path = WriteTempFile(L"settings.ini", original);
    config_.Load();
    config_.SaveInt("Window", "X", 42);
    config_.Flush();

    EXPECT_EQ(ReadFileBytes(path), original);
}

TEST_F(ConfigServiceTest, FlushWithoutAnySaveCreatesNoFile)
{
    config_.Flush();
    EXPECT_FALSE(std::filesystem::exists(temp_dir_ / L"settings.ini"));
}

TEST_F(ConfigServiceTest, FlushWritesChangedValue)
{
    WriteTempFile(L"settings.ini", "[Window]\r\nX=42\r\n");
    config_.Load();
    config_.SaveInt("Window", "X", 43);
    config_.Flush();

    EXPECT_EQ(MakeFreshLoadedConfig().LoadInt("Window", "X", 0, 0, 100), 43);
}

// ---- 複数のウィンドウ (プロセス) ----

// 先に閉じたウィンドウが変えた値を、後から閉じたウィンドウが起動時の値で戻さない。
TEST_F(ConfigServiceTest, FlushKeepsValuesWrittenByOtherInstance)
{
    WriteTempFile(L"settings.ini", "[View]\nDarkMode=0\n[General]\nLanguage=ja\n");
    auto a = MakeFreshLoadedConfig();
    auto b = MakeFreshLoadedConfig();

    a.SaveBool("View", "DarkMode", true);
    a.Flush();
    // 終了時は変えていない値も保存するので、同じ値の保存では記録しないことも確かめる。
    b.SaveBool("View", "DarkMode", false);
    b.SaveWString("General", "Language", L"en");
    b.Flush();

    auto fresh = MakeFreshLoadedConfig();
    EXPECT_TRUE(fresh.LoadBool("View", "DarkMode", false));
    EXPECT_EQ(fresh.LoadWString("General", "Language"), L"en");
}

// ウィンドウの位置と大きさは、最後に書いたウィンドウの組を残す。
TEST_F(ConfigServiceTest, FlushWritesWindowSectionAsWhole)
{
    WriteTempFile(L"settings.ini", "[Window]\nX=0\nY=0\nWidth=800\nHeight=600\n");
    auto a = MakeFreshLoadedConfig();
    auto b = MakeFreshLoadedConfig();

    a.WriteSectionAsWhole("Window");
    a.SaveInt("Window", "Width", 1000);
    a.Flush();
    b.WriteSectionAsWhole("Window");
    b.SaveInt("Window", "X", 50);
    b.SaveInt("Window", "Width", 800);
    b.Flush();

    auto fresh = MakeFreshLoadedConfig();
    EXPECT_EQ(fresh.LoadInt("Window", "X", -1, -100000, 100000), 50);
    EXPECT_EQ(fresh.LoadInt("Window", "Width", -1, 0, 100000), 800);
}

// 開いていたファイルとスクロール位置は組で残す。別のファイルに他のウィンドウのスクロール位置を付けない。
TEST_F(ConfigServiceTest, FlushWritesSessionSectionAsWhole)
{
    WriteTempFile(L"settings.ini", "[Session]\nLastFile=C:\\foo.md\nScrollNode=0\nScrollOffset=0\n");
    auto a = MakeFreshLoadedConfig();
    auto b = MakeFreshLoadedConfig();

    session::SaveScrollPosition(a, 10, 0.0f);
    a.Flush();
    session::SaveLastFilePath(b, L"C:\\bar.md");
    session::SaveScrollPosition(b, 0, 0.0f);
    b.Flush();

    auto fresh = MakeFreshLoadedConfig();
    EXPECT_EQ(fresh.LoadWString("Session", "LastFile"), L"C:\\bar.md");
    EXPECT_EQ(session::LoadScrollPosition(fresh).node, 0);
}

// 他のバージョンが書いたキーなど、このインスタンスが知らないキーを消さない。
TEST_F(ConfigServiceTest, FlushKeepsUnknownKeysInWholeSection)
{
    auto a = MakeFreshLoadedConfig();
    WriteTempFile(L"settings.ini", "[Window]\nX=0\nFuture=1\n");

    a.WriteSectionAsWhole("Window");
    a.SaveInt("Window", "X", 5);
    a.Flush();

    auto fresh = MakeFreshLoadedConfig();
    EXPECT_EQ(fresh.LoadInt("Window", "X", -1, -100000, 100000), 5);
    EXPECT_EQ(fresh.LoadInt("Window", "Future", -1, 0, 10), 1);
}

// 他のプログラムが開いていて書けなかった変更は失わず、次の Flush で他の値を消さずに書く。
TEST_F(ConfigServiceTest, FlushKeepsChangesAfterFailedWrite)
{
    const auto path = WriteTempFile(L"settings.ini", "[View]\nDarkMode=0\n[General]\nLanguage=ja\n");
    config_.Load();
    config_.SaveBool("View", "DarkMode", true);
    {
        const UniqueHandle blocker(CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        ASSERT_TRUE(blocker);
        config_.Flush();
    }
    config_.Flush();

    auto fresh = MakeFreshLoadedConfig();
    EXPECT_TRUE(fresh.LoadBool("View", "DarkMode", false));
    EXPECT_EQ(fresh.LoadWString("General", "Language"), L"ja");
}

// ---- INIファイル破損テスト ----

TEST_F(ConfigServiceTest, LoadCorruptedIniFile)
{
    WriteTempFile(L"settings.ini", "garbage content\nno sections\n");

    config_.Load();
    // パースエラーでもクラッシュしないことを確認
    EXPECT_EQ(config_.LoadInt("Window", "Width", 0, 100, 100000), 0);
}

// 数値として読み切れない値 (末尾にゴミが残る部分パースを含む) は既定値に倒す。
TEST_F(ConfigServiceTest, LoadIntReturnsDefaultForNonNumericValue)
{
    WriteTempFile(L"settings.ini", "[Test]\nWord=not_a_number\nSuffix=30px\n");
    config_.Load();
    EXPECT_EQ(config_.LoadInt("Test", "Word", 7, 0, 100), 7);
    EXPECT_EQ(config_.LoadInt("Test", "Suffix", 7, 0, 100), 7);
}

// ---- Parse テスト ----

TEST(IniParserTest, ParseEmpty)
{
    auto data = ini::Parse("");
    EXPECT_TRUE(data.empty());
}

TEST(IniParserTest, ParseSingleSection)
{
    auto data = ini::Parse("[Window]\nX=100\nY=200\n");
    ASSERT_EQ(data.size(), 1u);
    EXPECT_EQ(data["Window"]["X"], "100");
    EXPECT_EQ(data["Window"]["Y"], "200");
}

TEST(IniParserTest, ParseMultipleSections)
{
    auto data = ini::Parse(
        "[Window]\nWidth=1280\n\n[View]\nDarkMode=1\n");
    EXPECT_EQ(data["Window"]["Width"], "1280");
    EXPECT_EQ(data["View"]["DarkMode"], "1");
}

TEST(IniParserTest, ParseCommentLines)
{
    auto data = ini::Parse(
        "; comment\n# another comment\n[S]\nK=V\n");
    ASSERT_EQ(data.size(), 1u);
    EXPECT_EQ(data["S"]["K"], "V");
}

// セクション S のキー K の値。
TEST(IniParserTest, ParseValue)
{
    struct Case {
        const char* label;
        std::string_view input;
        std::string_view expected;
    };
    constexpr Case kCases[] = {
        { "Empty", "[S]\nK=\n", "" },
        // 最初の '=' だけで区切る
        { "ContainsEquals", "[S]\nK=C:\\a=b\\file.txt\n", "C:\\a=b\\file.txt" },
        { "TrimsBlanks", "[S]\n  K  =  Value  \n", "Value" },
        { "NoTrailingNewline", "[S]\nK=V", "V" },
        { "DuplicateKeyLastWins", "[S]\nK=first\nK=second\n", "second" },
        { "Utf8", "[S]\nK=C:\\\xe3\x83\xa6\xe3\x83\xbc\xe3\x82\xb6\xe3\x83\xbc\\\xe6\x96\x87\xe6\x9b\xb8.md\n",
          "C:\\\xe3\x83\xa6\xe3\x83\xbc\xe3\x82\xb6\xe3\x83\xbc\\\xe6\x96\x87\xe6\x9b\xb8.md" },
    };
    for (const auto& c : kCases) {
        SCOPED_TRACE(c.label);
        auto data = ini::Parse(c.input);
        // operator[] はキーが無くても空値を作るので、読めたことを先に確かめる。
        ASSERT_TRUE(data["S"].contains("K"));
        EXPECT_EQ(data["S"]["K"], c.expected);
    }
}

TEST(IniParserTest, ParseCRLFLineEndings)
{
    auto data = ini::Parse("[S]\r\nA=1\r\nB=2\r\n");
    EXPECT_EQ(data["S"]["A"], "1");
    EXPECT_EQ(data["S"]["B"], "2");
}

TEST(IniParserTest, ParseEmptySection)
{
    auto data = ini::Parse("[Empty]\n[Full]\nK=V\n");
    EXPECT_TRUE(!data.contains("Empty") || data["Empty"].empty());
    EXPECT_EQ(data["Full"]["K"], "V");
}

TEST(IniParserTest, ParseMalformedLineIgnored)
{
    auto data = ini::Parse("[S]\nno_equals_here\nK=V\n");
    ASSERT_EQ(data["S"].size(), 1u);
    EXPECT_EQ(data["S"]["K"], "V");
}

TEST(IniParserTest, ParseKeyBeforeSection)
{
    auto data = ini::Parse("Orphan=1\n[S]\nK=2\n");
    EXPECT_EQ(data[""]["Orphan"], "1");
    EXPECT_EQ(data["S"]["K"], "2");
}

// ---- Serialize テスト ----

TEST(IniParserTest, SerializeEmpty)
{
    ini::IniData data;
    EXPECT_EQ(ini::Serialize(data), "");
}

TEST(IniParserTest, SerializeSingleSection)
{
    ini::IniData data;
    data["Window"]["X"] = "100";
    data["Window"]["Y"] = "200";
    std::string result = ini::Serialize(data);
    EXPECT_TRUE(result.contains("[Window]"));
    EXPECT_TRUE(result.contains("X=100"));
    EXPECT_TRUE(result.contains("Y=200"));
}

TEST(IniParserTest, SerializeSkipsEmptySections)
{
    ini::IniData data;
    data["Empty"];  // 空のセクション
    data["Full"]["K"] = "V";
    std::string result = ini::Serialize(data);
    EXPECT_FALSE(result.contains("Empty"));
    EXPECT_TRUE(result.contains("[Full]"));
}

TEST(IniParserTest, SerializeMultipleSectionsSeparatedByBlankLine)
{
    ini::IniData data;
    data["A"]["K1"] = "V1";
    data["B"]["K2"] = "V2";
    std::string result = ini::Serialize(data);
    // セクション間に空行がある
    EXPECT_TRUE(result.contains("\n\n[B]"));
}

TEST(IniParserTest, RoundTrip)
{
    ini::IniData original;
    original["Window"]["X"] = "100";
    original["Window"]["Y"] = "-50";
    original["View"]["DarkMode"] = "1";
    original["Session"]["LastFile"] = "C:\\\xe3\x83\xa6\xe3\x83\xbc\xe3\x82\xb6\xe3\x83\xbc\\test.md";

    std::string text = ini::Serialize(original);
    auto parsed = ini::Parse(text);

    EXPECT_EQ(parsed["Window"]["X"], "100");
    EXPECT_EQ(parsed["Window"]["Y"], "-50");
    EXPECT_EQ(parsed["View"]["DarkMode"], "1");
    EXPECT_EQ(parsed["Session"]["LastFile"], original["Session"]["LastFile"]);
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

// ---- SavePanes / LoadPanes ----

TEST_F(SessionTest, SaveAndLoadPanes)
{
    PaneController saved;
    saved.SetSidePaneVisible(PaneTarget::File, false);
    saved.SetSidePaneWidth(PaneTarget::File, 180.0f);
    saved.SetSidePaneWidth(PaneTarget::Toc, 250.0f);
    session::SavePanes(config_, saved);

    const auto loaded = LoadPanes();

    EXPECT_FALSE(loaded.IsSidePaneVisible(PaneTarget::File));
    EXPECT_TRUE(loaded.IsSidePaneVisible(PaneTarget::Toc));
    EXPECT_FLOAT_EQ(loaded.GetSidePaneWidth(PaneTarget::File), 180.0f);
    EXPECT_FLOAT_EQ(loaded.GetSidePaneWidth(PaneTarget::Toc), 250.0f);
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
    // 過剰幅の制限はレイアウト側 (md_width = max(md_min_width, total_width - x) で MD ペイン
    // 下限を保証) に委ねるため、LoadPanes はウィンドウ幅に基づく上限 clamp を行わず、
    // 保存値をそのまま使う。これにより狭いウィンドウ起動時に保存値が min へ潰れて
    // 終了時の再保存で恒久喪失するのを防ぐ。
    config_.SaveInt("Pane", "FileWidth", 500);
    config_.SaveInt("Pane", "TocWidth", 500);

    const auto loaded = LoadPanes();

    EXPECT_FLOAT_EQ(loaded.GetSidePaneWidth(PaneTarget::File), 500.0f);
    EXPECT_FLOAT_EQ(loaded.GetSidePaneWidth(PaneTarget::Toc), 500.0f);
}

TEST_F(SessionTest, LoadPanesUsesDefaultWhenNothingSaved)
{
    const auto loaded = LoadPanes();

    EXPECT_TRUE(loaded.IsSidePaneVisible(PaneTarget::File));
    EXPECT_TRUE(loaded.IsSidePaneVisible(PaneTarget::Toc));
    EXPECT_FLOAT_EQ(loaded.GetSidePaneWidth(PaneTarget::File), SIDE_PANE_DEFAULT_WIDTH);
    EXPECT_FLOAT_EQ(loaded.GetSidePaneWidth(PaneTarget::Toc), SIDE_PANE_DEFAULT_WIDTH);
}

// 幅はズームを含まない DIP 値として往復する。読み側は倍率を掛けない。
TEST_F(SessionTest, SavedWidthRoundTripsAsDip)
{
    PaneController saved;
    saved.SetSidePaneWidth(PaneTarget::File, 300.0f);
    saved.SetSidePaneWidth(PaneTarget::Toc, 260.0f);
    saved.SidePaneZoom(PaneTarget::File).SetIndex(ZOOM_DEFAULT_INDEX + 2);
    session::SavePanes(config_, saved);

    const auto loaded = LoadPanes();

    EXPECT_FLOAT_EQ(loaded.GetSidePaneWidth(PaneTarget::File), 300.0f);
    EXPECT_FLOAT_EQ(loaded.GetSidePaneWidth(PaneTarget::Toc), 260.0f);
}

// ペインの倍率はペインごとに往復する。
TEST_F(SessionTest, PaneZoomRoundTrips)
{
    PaneController saved;
    saved.SidePaneZoom(PaneTarget::File).SetIndex(ZOOM_DEFAULT_INDEX + 2);
    saved.SidePaneZoom(PaneTarget::Toc).SetIndex(ZOOM_DEFAULT_INDEX - 1);
    session::SavePanes(config_, saved);

    const auto loaded = LoadPanes();

    EXPECT_EQ(loaded.SidePaneZoom(PaneTarget::File).Index(), ZOOM_DEFAULT_INDEX + 2);
    EXPECT_EQ(loaded.SidePaneZoom(PaneTarget::Toc).Index(), ZOOM_DEFAULT_INDEX - 1);
}

// 保存が無ければ等倍。
TEST_F(SessionTest, PaneZoomDefaultsToActualSize)
{
    const auto loaded = LoadPanes();

    EXPECT_TRUE(loaded.SidePaneZoom(PaneTarget::File).IsDefault());
    EXPECT_TRUE(loaded.SidePaneZoom(PaneTarget::Toc).IsDefault());
}

// ---- SaveScrollPosition / LoadScrollPosition ----

TEST_F(SessionTest, SaveAndLoadScrollPosition)
{
    session::SaveScrollPosition(config_, 5, 49.6f);

    const auto pos = session::LoadScrollPosition(config_);
    EXPECT_EQ(pos.node, 5);
    EXPECT_EQ(pos.offset, 50); // lround(49.6)
}

// ---- SaveLastFilePath / LoadLastFilePath ----

TEST_F(SessionTest, SaveLastFilePathStoresPath)
{
    // LoadLastFilePath は実在しないパスを弾くため、実ファイルを作って往復させる。
    const auto file = WriteTempFile(L"last.md", "# last").wstring();
    session::SaveLastFilePath(config_, file);
    EXPECT_EQ(session::LoadLastFilePath(config_), std::wstring_view{ file });
}

TEST_F(SessionTest, SaveLastFilePathSkipsEmpty)
{
    // 空パスで既存の保存値を上書きしない。
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
        config_.SaveWString("Session", "LastFile", saved);
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
        config_.SaveWString("Session", "LastFile", prefixed);
        EXPECT_TRUE(session::LoadLastFilePath(config_).empty());
    }
}

TEST_F(SessionTest, LoadLastFilePathReturnsEmptyWhenNothingSaved)
{
    auto path = session::LoadLastFilePath(config_);
    EXPECT_TRUE(path.empty());
}

// ---- SaveScrollPosition / LoadScrollPosition の境界 ----

TEST_F(SessionTest, SaveScrollPosition_NegativeOffsetRoundtrips)
{
    // ノード先頭より上を見ている (scroll_y < node_y) と offset は負になる。
    session::SaveScrollPosition(config_, 3, -150.0f);
    const auto pos = session::LoadScrollPosition(config_);
    EXPECT_EQ(pos.node, 3);
    EXPECT_EQ(pos.offset, -150);
}

TEST_F(SessionTest, LoadScrollPosition_DefaultsWhenNothingSaved)
{
    const auto pos = session::LoadScrollPosition(config_);
    EXPECT_EQ(pos.node, -1);
    EXPECT_EQ(pos.offset, 0);
}

// node は [-1, 1000000]、offset は ±1000000 を外れると既定値 (-1 / 0) に倒す。
TEST_F(SessionTest, LoadScrollPosition_OutOfRangeFallsBackToDefault)
{
    config_.SaveInt("Session", "ScrollNode", 10000000);
    config_.SaveInt("Session", "ScrollOffset", 99999999);
    const auto pos = session::LoadScrollPosition(config_);
    EXPECT_EQ(pos.node, -1);
    EXPECT_EQ(pos.offset, 0);
}

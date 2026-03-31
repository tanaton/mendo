#include <gtest/gtest.h>
#include "ini_parser.h"

// ---- Parse テスト ----

TEST(IniParserTest, ParseEmpty) {
    auto data = ini::Parse("");
    EXPECT_TRUE(data.empty());
}

TEST(IniParserTest, ParseSingleSection) {
    auto data = ini::Parse("[Window]\nX=100\nY=200\n");
    ASSERT_EQ(data.size(), 1u);
    EXPECT_EQ(data["Window"]["X"], "100");
    EXPECT_EQ(data["Window"]["Y"], "200");
}

TEST(IniParserTest, ParseMultipleSections) {
    auto data = ini::Parse(
        "[Window]\nWidth=1280\n\n[View]\nDarkMode=1\n");
    EXPECT_EQ(data["Window"]["Width"], "1280");
    EXPECT_EQ(data["View"]["DarkMode"], "1");
}

TEST(IniParserTest, ParseCommentLines) {
    auto data = ini::Parse(
        "; comment\n# another comment\n[S]\nK=V\n");
    ASSERT_EQ(data.size(), 1u);
    EXPECT_EQ(data["S"]["K"], "V");
}

TEST(IniParserTest, ParseEmptyValue) {
    auto data = ini::Parse("[S]\nKey=\n");
    EXPECT_EQ(data["S"]["Key"], "");
}

TEST(IniParserTest, ParseValueContainingEquals) {
    auto data = ini::Parse("[S]\nPath=C:\\a=b\\file.txt\n");
    EXPECT_EQ(data["S"]["Path"], "C:\\a=b\\file.txt");
}

TEST(IniParserTest, ParseWhitespaceTrimming) {
    auto data = ini::Parse("[S]\n  Key  =  Value  \n");
    EXPECT_EQ(data["S"]["Key"], "Value");
}

TEST(IniParserTest, ParseNoTrailingNewline) {
    auto data = ini::Parse("[S]\nK=V");
    EXPECT_EQ(data["S"]["K"], "V");
}

TEST(IniParserTest, ParseCRLFLineEndings) {
    auto data = ini::Parse("[S]\r\nA=1\r\nB=2\r\n");
    EXPECT_EQ(data["S"]["A"], "1");
    EXPECT_EQ(data["S"]["B"], "2");
}

TEST(IniParserTest, ParseEmptySection) {
    auto data = ini::Parse("[Empty]\n[Full]\nK=V\n");
    EXPECT_TRUE(data.find("Empty") == data.end() || data["Empty"].empty());
    EXPECT_EQ(data["Full"]["K"], "V");
}

TEST(IniParserTest, ParseDuplicateKeysLastWins) {
    auto data = ini::Parse("[S]\nK=first\nK=second\n");
    EXPECT_EQ(data["S"]["K"], "second");
}

TEST(IniParserTest, ParseMalformedLineIgnored) {
    auto data = ini::Parse("[S]\nno_equals_here\nK=V\n");
    ASSERT_EQ(data["S"].size(), 1u);
    EXPECT_EQ(data["S"]["K"], "V");
}

TEST(IniParserTest, ParseKeyBeforeSection) {
    auto data = ini::Parse("Orphan=1\n[S]\nK=2\n");
    EXPECT_EQ(data[""]["Orphan"], "1");
    EXPECT_EQ(data["S"]["K"], "2");
}

TEST(IniParserTest, ParseUtf8Content) {
    std::string input = "[Session]\nLastFile=C:\\\xe3\x83\xa6\xe3\x83\xbc\xe3\x82\xb6\xe3\x83\xbc\\\xe6\x96\x87\xe6\x9b\xb8.md\n";
    auto data = ini::Parse(input);
    EXPECT_EQ(data["Session"]["LastFile"], "C:\\\xe3\x83\xa6\xe3\x83\xbc\xe3\x82\xb6\xe3\x83\xbc\\\xe6\x96\x87\xe6\x9b\xb8.md");
}

// ---- Serialize テスト ----

TEST(IniParserTest, SerializeEmpty) {
    ini::IniData data;
    EXPECT_EQ(ini::Serialize(data), "");
}

TEST(IniParserTest, SerializeSingleSection) {
    ini::IniData data;
    data["Window"]["X"] = "100";
    data["Window"]["Y"] = "200";
    std::string result = ini::Serialize(data);
    EXPECT_NE(result.find("[Window]"), std::string::npos);
    EXPECT_NE(result.find("X=100"), std::string::npos);
    EXPECT_NE(result.find("Y=200"), std::string::npos);
}

TEST(IniParserTest, SerializeSkipsEmptySections) {
    ini::IniData data;
    data["Empty"];  // 空のセクション
    data["Full"]["K"] = "V";
    std::string result = ini::Serialize(data);
    EXPECT_EQ(result.find("Empty"), std::string::npos);
    EXPECT_NE(result.find("[Full]"), std::string::npos);
}

TEST(IniParserTest, SerializeMultipleSectionsSeparatedByBlankLine) {
    ini::IniData data;
    data["A"]["K1"] = "V1";
    data["B"]["K2"] = "V2";
    std::string result = ini::Serialize(data);
    // セクション間に空行がある
    EXPECT_NE(result.find("\n\n[B]"), std::string::npos);
}

TEST(IniParserTest, RoundTrip) {
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

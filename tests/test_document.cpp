#include <gtest/gtest.h>
#include "document.h"

TEST(DocumentTest, DefaultIsEmpty) {
    Document doc;
    EXPECT_TRUE(doc.IsEmpty());
    EXPECT_TRUE(doc.GetFilePath().empty());
    EXPECT_TRUE(doc.GetNodes().empty());
    EXPECT_TRUE(doc.GetToc().GetEntries().empty());
}

TEST(DocumentTest, FromMarkdownBasic) {
    auto doc = Document::FromMarkdown("# Hello\nworld", L"C:\\test.md");
    EXPECT_FALSE(doc.IsEmpty());
    EXPECT_EQ(doc.GetFilePath(), L"C:\\test.md");
    EXPECT_GE(doc.GetNodes().size(), 2u);
    // Should have a heading in TOC
    EXPECT_FALSE(doc.GetToc().GetEntries().empty());
    EXPECT_EQ(doc.GetToc().GetEntries()[0].text, L"Hello");
}

TEST(DocumentTest, FromMarkdownEmpty) {
    auto doc = Document::FromMarkdown("", L"C:\\empty.md");
    EXPECT_TRUE(doc.IsEmpty());
    EXPECT_EQ(doc.GetFilePath(), L"C:\\empty.md");
}

TEST(DocumentTest, GetDirectory) {
    auto doc = Document::FromMarkdown("test", L"C:\\dir\\sub\\file.md");
    EXPECT_EQ(doc.GetDirectory(), L"C:\\dir\\sub");
}

TEST(DocumentTest, GetDirectoryForwardSlash) {
    auto doc = Document::FromMarkdown("test", L"C:/dir/file.md");
    EXPECT_EQ(doc.GetDirectory(), L"C:/dir");
}

TEST(DocumentTest, GetDirectoryEmpty) {
    Document doc;
    EXPECT_TRUE(doc.GetDirectory().empty());
}

TEST(DocumentTest, GetDirectoryNoSlash) {
    auto doc = Document::FromMarkdown("test", L"file.md");
    EXPECT_TRUE(doc.GetDirectory().empty());
}

TEST(DocumentTest, SetFilePath) {
    Document doc;
    doc.SetFilePath(L"C:\\new\\path.md");
    EXPECT_EQ(doc.GetFilePath(), L"C:\\new\\path.md");
}

TEST(DocumentTest, ReplaceContent) {
    auto doc = Document::FromMarkdown("# First", L"C:\\test.md");
    EXPECT_FALSE(doc.GetToc().GetEntries().empty());
    EXPECT_EQ(doc.GetToc().GetEntries()[0].text, L"First");

    // Replace with new content
    auto doc2 = Document::FromMarkdown("# Second\n## Sub", L"");
    doc.ReplaceContent(doc2.GetNodesMut());

    // TOC should be rebuilt
    EXPECT_GE(doc.GetToc().GetEntries().size(), 2u);
    EXPECT_EQ(doc.GetToc().GetEntries()[0].text, L"Second");
    // File path should be unchanged
    EXPECT_EQ(doc.GetFilePath(), L"C:\\test.md");
}

TEST(DocumentTest, GetNodesMut) {
    auto doc = Document::FromMarkdown("hello", L"test.md");
    ASSERT_FALSE(doc.IsEmpty());

    // Modify nodes through mutable reference
    auto& nodes = doc.GetNodesMut();
    nodes[0].text = L"modified";
    EXPECT_EQ(doc.GetNodes()[0].text, L"modified");
}

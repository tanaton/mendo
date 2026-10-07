#include <gtest/gtest.h>
#include "file_watcher.h"
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <ostream>
#include <random>
#include <span>
#include <string>
#include <vector>
#include <windows.h>

// NotifyBufferHasTargetChange は ReadDirectoryChangesW の生バッファを解析する。
// 実カーネル通知では切り詰め・破損を再現できないため、FILE_NOTIFY_INFORMATION 列を手組みして検査する。

namespace {

constexpr size_t kHeaderBytes = offsetof(FILE_NOTIFY_INFORMATION, FileName);
constexpr std::wstring_view kName = L"readme.md";
constexpr std::wstring_view kShort = L"README~1.MD";

struct Entry {
    DWORD action = FILE_ACTION_MODIFIED;
    std::wstring name;
    // 指定時は実際の値の代わりに書き込む (破損の模擬)。
    std::optional<DWORD> name_length_override;
    std::optional<DWORD> next_offset_override;
};

size_t EntryBytes(const Entry& e)
{
    // カーネルと同じく各エントリを DWORD 境界に揃える。
    const size_t raw = kHeaderBytes + e.name.size() * sizeof(wchar_t);
    return (raw + sizeof(DWORD) - 1) / sizeof(DWORD) * sizeof(DWORD);
}

std::vector<std::byte> BuildBuffer(const std::vector<Entry>& entries)
{
    std::vector<std::byte> buf;
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        const size_t begin = buf.size();
        const bool last = i + 1 == entries.size();
        // 末尾エントリは名前の直後で終える (カーネルは最後のパディングを返さないことがある)。
        const size_t size = last ? kHeaderBytes + e.name.size() * sizeof(wchar_t) : EntryBytes(e);
        buf.resize(begin + size);
        const DWORD next = e.next_offset_override.value_or(last ? 0 : static_cast<DWORD>(size));
        const DWORD name_len = e.name_length_override.value_or(static_cast<DWORD>(e.name.size() * sizeof(wchar_t)));
        std::memcpy(buf.data() + begin + offsetof(FILE_NOTIFY_INFORMATION, NextEntryOffset), &next, sizeof(DWORD));
        std::memcpy(buf.data() + begin + offsetof(FILE_NOTIFY_INFORMATION, Action), &e.action, sizeof(DWORD));
        std::memcpy(buf.data() + begin + offsetof(FILE_NOTIFY_INFORMATION, FileNameLength), &name_len, sizeof(DWORD));
        std::memcpy(buf.data() + begin + kHeaderBytes, e.name.data(), e.name.size() * sizeof(wchar_t));
    }
    return buf;
}

// 正確なサイズのヒープ領域へ複製して解析する。範囲外読み取りがあれば ASan ビルドで検出される。
bool Analyze(std::span<const std::byte> bytes, std::wstring_view short_name = kShort)
{
    auto exact = std::make_unique<std::byte[]>(bytes.size());
    if (!bytes.empty()) {
        std::memcpy(exact.get(), bytes.data(), bytes.size());
    }
    return NotifyBufferHasTargetChange({ exact.get(), bytes.size() }, kName, short_name);
}

struct NotifyCase {
    const char* label;
    std::vector<Entry> entries;
    // BuildBuffer 結果の末尾を削るバイト数 (bytes_returned が実データより短い状況)。
    size_t truncate = 0;
    std::wstring_view short_name = kShort;
    bool expected;
};

void PrintTo(const NotifyCase& c, std::ostream* os)
{
    *os << c.label;
}

class NotifyBufferTest : public ::testing::TestWithParam<NotifyCase> {};

} // namespace

TEST_P(NotifyBufferTest, DetectsTargetChange)
{
    const auto& c = GetParam();
    auto buf = BuildBuffer(c.entries);
    ASSERT_LE(c.truncate, buf.size());
    buf.resize(buf.size() - c.truncate);
    EXPECT_EQ(Analyze(buf, c.short_name), c.expected);
}

INSTANTIATE_TEST_SUITE_P(
    Table, NotifyBufferTest,
    ::testing::Values(
        NotifyCase{ "Modified", { { FILE_ACTION_MODIFIED, L"readme.md" } }, 0, kShort, true },
        NotifyCase{ "Added", { { FILE_ACTION_ADDED, L"readme.md" } }, 0, kShort, true },
        NotifyCase{ "RenamedNewName", { { FILE_ACTION_RENAMED_NEW_NAME, L"readme.md" } }, 0, kShort, true },
        NotifyCase{ "RemovedIgnored", { { FILE_ACTION_REMOVED, L"readme.md" } }, 0, kShort, false },
        NotifyCase{ "RenamedOldNameIgnored", { { FILE_ACTION_RENAMED_OLD_NAME, L"readme.md" } }, 0, kShort, false },
        NotifyCase{ "CaseInsensitive", { { FILE_ACTION_MODIFIED, L"README.MD" } }, 0, kShort, true },
        NotifyCase{ "OtherFile", { { FILE_ACTION_MODIFIED, L"other.md" } }, 0, kShort, false },
        NotifyCase{ "LongerNameSamePrefix", { { FILE_ACTION_MODIFIED, L"readme.md.bak" } }, 0, kShort, false },
        NotifyCase{ "ShorterName", { { FILE_ACTION_MODIFIED, L"readme.m" } }, 0, kShort, false },
        NotifyCase{ "ShortNameMatches", { { FILE_ACTION_MODIFIED, L"readme~1.md" } }, 0, kShort, true },
        NotifyCase{ "ShortNameUnknown", { { FILE_ACTION_MODIFIED, L"README~1.MD" } }, 0, L"", false },
        // 一時ファイルへ書いてから rename で置き換えるエディタの保存列。
        NotifyCase{ "AtomicSaveSequence",
                    { { FILE_ACTION_ADDED, L"readme.md.tmp" },
                      { FILE_ACTION_REMOVED, L"readme.md" },
                      { FILE_ACTION_RENAMED_OLD_NAME, L"readme.md.tmp" },
                      { FILE_ACTION_RENAMED_NEW_NAME, L"readme.md" } },
                    0, kShort, true },
        NotifyCase{ "TargetOnlyRemovedAmongOthers",
                    { { FILE_ACTION_MODIFIED, L"a.md" }, { FILE_ACTION_REMOVED, L"readme.md" }, { FILE_ACTION_MODIFIED, L"b.md" } },
                    0, kShort, false },
        NotifyCase{ "TargetInLastOfMany",
                    { { FILE_ACTION_MODIFIED, L"a.md" }, { FILE_ACTION_ADDED, L"bb.md" }, { FILE_ACTION_MODIFIED, L"readme.md" } },
                    0, kShort, true },
        NotifyCase{ "EmptyBufferMeansOverflow", {}, 0, kShort, true },
        NotifyCase{ "HeaderTruncated", { { FILE_ACTION_MODIFIED, L"readme.md" } }, 18 + 4, kShort, false },
        NotifyCase{ "NameTruncated", { { FILE_ACTION_MODIFIED, L"readme.md" } }, 2, kShort, false },
        NotifyCase{ "LastEntryNameTruncated",
                    { { FILE_ACTION_MODIFIED, L"a.md" }, { FILE_ACTION_MODIFIED, L"readme.md" } }, 2, kShort, false },
        NotifyCase{ "HugeFileNameLength", { { FILE_ACTION_MODIFIED, L"readme.md", 0xFFFFFFF0u } }, 0, kShort, false },
        NotifyCase{ "FileNameLengthOneBytePastEnd", { { FILE_ACTION_MODIFIED, L"readme.md", 19u } }, 0, kShort, false },
        NotifyCase{ "OddFileNameLength", { { FILE_ACTION_MODIFIED, L"readme.md", 17u } }, 0, kShort, false },
        NotifyCase{ "NextOffsetPastEnd",
                    { { FILE_ACTION_MODIFIED, L"a.md", std::nullopt, 0x10000u }, { FILE_ACTION_MODIFIED, L"readme.md" } }, 0, kShort, false },
        // 次エントリの固定部が収まらない位置を指す NextEntryOffset。
        NotifyCase{ "NextOffsetLeavesNoHeader",
                    { { FILE_ACTION_MODIFIED, L"a.md", std::nullopt, 40u }, { FILE_ACTION_MODIFIED, L"readme.md" } }, 0, kShort, false },
        // 一致は次エントリの検証より先に確定する。
        NotifyCase{ "MatchBeforeCorruptNext",
                    { { FILE_ACTION_MODIFIED, L"readme.md", std::nullopt, 0xFFFFFFF0u } }, 0, kShort, true }),
    [](const ::testing::TestParamInfo<NotifyCase>& info) { return std::string(info.param.label); });

// 実データ領域は名前の最後まであるが、bytes_returned (span) はその手前で切れている状況。
// 名前長の検証を欠くと span の外にある正しい名前を読んで一致してしまう。
TEST(NotifyBuffer, NameBeyondReportedLengthIsNotRead)
{
    const auto full = BuildBuffer({ { FILE_ACTION_MODIFIED, L"a.md" }, { FILE_ACTION_MODIFIED, L"readme.md" } });
    ASSERT_TRUE(NotifyBufferHasTargetChange(full, kName, {}));
    for (size_t cut = 1; cut < full.size(); ++cut) {
        SCOPED_TRACE(::testing::Message() << "cut=" << cut);
        EXPECT_FALSE(NotifyBufferHasTargetChange(std::span{ full }.first(full.size() - cut), kName, {}));
    }
}

// ランダムな有効列とその切り詰め・破損版で、(1) 有効列/切り詰めは「span に完全に収まる先頭側エントリ」
// だけから結果が決まる (2) 破損版でも範囲外を読まない (ASan ビルドで検出) ことを確かめる。
TEST(NotifyBuffer, RandomBuffersMatchModelAndStayInBounds)
{
    const std::wstring names[] = { L"readme.md", L"README.MD", L"readme~1.md", L"other.md", L"readme.md.tmp", L"x" };
    const DWORD actions[] = { FILE_ACTION_ADDED, FILE_ACTION_REMOVED, FILE_ACTION_MODIFIED, FILE_ACTION_RENAMED_OLD_NAME,
                              FILE_ACTION_RENAMED_NEW_NAME };
    const auto is_hit = [&](const Entry& e) {
        return e.action != FILE_ACTION_REMOVED && e.action != FILE_ACTION_RENAMED_OLD_NAME && e.name != L"other.md" &&
            e.name != L"readme.md.tmp" && e.name != L"x";
    };

    for (const uint32_t seed : { 1u, 2u, 3u, 99u, 12345u }) {
        std::mt19937 rng(seed);
        for (int iter = 0; iter < 200; ++iter) {
            std::vector<Entry> entries(std::uniform_int_distribution<size_t>(1, 5)(rng));
            std::string desc;
            for (auto& e : entries) {
                e.action = actions[std::uniform_int_distribution<size_t>(0, std::size(actions) - 1)(rng)];
                const size_t name_index = std::uniform_int_distribution<size_t>(0, std::size(names) - 1)(rng);
                e.name = names[name_index];
                desc += std::to_string(e.action) + ":" + std::to_string(name_index) + " ";
            }
            const auto buf = BuildBuffer(entries);
            const size_t len = std::uniform_int_distribution<size_t>(0, buf.size())(rng);
            SCOPED_TRACE(::testing::Message() << "seed=" << seed << " iter=" << iter << " len=" << len << "/" << buf.size()
                                              << " entries(action:name_index)=" << desc);

            // 切り詰めモデル: 長さ 0 は溢れ扱い、それ以外は完全に収まるエントリの中に一致があるか。
            bool expected = len == 0;
            size_t begin = 0;
            for (const auto& e : entries) {
                if (len == 0 || begin + kHeaderBytes + e.name.size() * sizeof(wchar_t) > len) {
                    break;
                }
                if (is_hit(e)) {
                    expected = true;
                    break;
                }
                begin += EntryBytes(e);
            }
            EXPECT_EQ(Analyze(std::span{ buf }.first(len)), expected);

            // 破損: ランダムなバイトを書き換える。結果は問わず、範囲外を読まないことだけを要求する。
            auto corrupt = buf;
            const int flips = std::uniform_int_distribution<int>(1, 4)(rng);
            for (int f = 0; f < flips && !corrupt.empty(); ++f) {
                const size_t pos = std::uniform_int_distribution<size_t>(0, corrupt.size() - 1)(rng);
                corrupt[pos] = static_cast<std::byte>(std::uniform_int_distribution<int>(0, 255)(rng));
            }
            (void)Analyze(std::span{ corrupt }.first(len));
        }
    }
}

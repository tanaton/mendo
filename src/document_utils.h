#pragma once
#include "types.h"
#include <string>
#include <vector>
#include <optional>

// Extract selected text from nodes based on selection range.
// Returns the concatenated text with \r\n between nodes.
std::wstring ExtractSelectedText(const std::vector<RenderNode>& nodes,
                                  const TextSelection& selection);

// Find a link URL at a given text position within a node's runs.
// Returns the link URL if the position falls within a link run, otherwise nullopt.
std::optional<std::wstring> FindLinkAtPosition(const RenderNode& node, uint32_t text_pos);

// Find the index of the heading node that matches the given anchor ID.
// The anchor is compared case-insensitively (lowercased).
// Returns -1 if not found.
int FindAnchorNodeIndex(const std::vector<RenderNode>& nodes, const std::wstring& anchor);

// Word boundary result for double-click word selection.
struct WordBoundary {
    uint32_t start = 0;
    uint32_t end = 0;
    bool found = false;
};

// Find word boundaries around a position in text.
// A "word character" is alphanumeric or underscore.
// Returns {start, end, true} if the position is on a word character, otherwise {0, 0, false}.
WordBoundary FindWordBoundaries(const std::wstring& text, uint32_t pos);

// Extract the filename portion from a full file path.
// e.g. "C:\\dir\\file.md" -> "file.md"
std::wstring ExtractFilename(const std::wstring& path);

// Build a title string from a file path.
// e.g. "C:\\dir\\file.md" -> "file.md - MaDView"
// If path is empty, returns "MaDView".
std::wstring BuildTitleString(const std::wstring& path);

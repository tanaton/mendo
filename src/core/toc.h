#pragma once

// 目次は文書順の見出しの列 (パーサが見出しの終わりで積む)。
struct TocEntry {
    int node_index = -1;
    int heading_level = 1;
};

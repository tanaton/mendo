#include "toc.h"
#include "pane_layout.h"

int TableOfContents::HitTest(float local_y, float item_height) const noexcept
{
    return HitTestUniformList(local_y, item_height, entries_.size());
}

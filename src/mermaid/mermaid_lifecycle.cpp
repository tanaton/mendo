#include "mermaid_lifecycle.h"
#include "document_types.h"
#include "syntax.h"

namespace mermaid_lifecycle {

bool ShouldTriggerInitForNode(const Node& node) noexcept
{
    return node.type == NodeType::CodeBlock && IsDiagramLanguage(node.code_language);
}

} // namespace mermaid_lifecycle

#pragma once

#include "rapport/layout.h"

namespace rap::detail
{

// Plain containers need a widget wrapper for binding or applying common state.
inline bool needs_container_wrapper(const node& n)
{
    if (!n.common.handle && n.common.visible && n.common.enabled && n.common.tooltip.empty())
        return false;
    return std::holds_alternative<spec::box>(n.spec) || std::holds_alternative<spec::form>(n.spec) ||
           std::holds_alternative<spec::group>(n.spec) || std::holds_alternative<spec::scroll>(n.spec) ||
           std::holds_alternative<spec::toolbar>(n.spec);
}

}

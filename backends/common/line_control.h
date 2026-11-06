#pragma once

#include "rapport/layout.h"

namespace rap::detail
{

inline bool is_line_control(const node_spec& s)
{
    return std::holds_alternative<spec::label>(s) || std::holds_alternative<spec::button>(s) ||
           std::holds_alternative<spec::check>(s) || std::holds_alternative<spec::entry>(s) ||
           std::holds_alternative<spec::drop_down>(s) || std::holds_alternative<spec::combo>(s) ||
           std::holds_alternative<spec::menu_button>(s) ||
           std::holds_alternative<spec::progress_bar>(s) ||
           std::holds_alternative<spec::date_picker>(s);
}

}

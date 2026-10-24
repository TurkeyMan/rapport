// What the Linux file managers' adapters share: asking the provider without
// letting anything escape into the file manager, and keeping the provider
// that made a menu alive until one of its items is chosen.
#pragma once

#include <rapport/shell.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rap::shell::adapter
{

// One menu's provider and what it was asked about, shared by the items it
// made (one provider serves one request: a menu, then the chosen item).
struct request
{
    std::unique_ptr<shell::provider> provider;
    selection sel;
};

// The provider's menu for `sel`, tidied; null with no items if there's
// nothing to show or the provider threw. `separators` false drops them, for
// file managers whose menus have none.
std::shared_ptr<request> ask(selection sel, std::vector<menu_item>& items,
                             bool separators) noexcept;

// Hands the chosen item back to the provider that made it.
void choose(const request& r, std::string_view id) noexcept;

// A label with GTK's mnemonic ('_') in place of '&'.
std::string gtk_label(std::string_view label);
// A label with no mnemonic.
std::string plain_label(std::string_view label);

// The icon as file managers take it: a file's absolute path, a theme icon's
// name, or empty if it has none or the file isn't there.
std::string icon_spec(const std::string& icon);

// Whether Shift is held, from the toolkit the file manager has loaded.
bool shift_held() noexcept;

}  // namespace rap::shell::adapter

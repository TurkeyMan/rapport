// rap::i18n: .po files read directly. Lookups are a hash per domain from
// context + "\x04" + msgid (gettext's key) to the translated forms; the
// plural rule is the file's Plural-Forms expression, evaluated by a small
// parser for gettext's C subset.
#include "rapport/i18n.h"

#include "rapport/sys.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <unordered_map>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace rap::i18n
{
namespace
{

// -- plural rules -----------------------------------------------------------------

// gettext's plural expressions: n, integers, ! * / % + - < <= > >= == != &&
// || ?: and parentheses. Parsed once into a tree.
struct expr
{
    enum kind
    {
        num,
        var,
        unary_not,
        binary,
        ternary
    } k = num;
    uint64_t value = 0;
    std::string op;
    std::unique_ptr<expr> a, b, c;

    uint64_t eval(uint64_t n) const
    {
        switch (k)
        {
        case num: return value;
        case var: return n;
        case unary_not: return !a->eval(n);
        case ternary: return a->eval(n) ? b->eval(n) : c->eval(n);
        case binary: break;
        }
        const uint64_t x = a->eval(n);
        if (op == "&&") return x && b->eval(n);
        if (op == "||") return x || b->eval(n);
        const uint64_t y = b->eval(n);
        if (op == "==") return x == y;
        if (op == "!=") return x != y;
        if (op == "<") return x < y;
        if (op == "<=") return x <= y;
        if (op == ">") return x > y;
        if (op == ">=") return x >= y;
        if (op == "+") return x + y;
        if (op == "-") return x - y;
        if (op == "*") return x * y;
        if (op == "/") return y ? x / y : 0;
        if (op == "%") return y ? x % y : 0;
        return 0;
    }
};

class plural_parser
{
public:
    explicit plural_parser(std::string_view s)
        : s_(s)
    {
    }
    std::unique_ptr<expr> parse()
    {
        auto e = ternary();
        skip();
        return e && at_ == s_.size() ? std::move(e) : nullptr;
    }

private:
    void skip()
    {
        while (at_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[at_]))) ++at_;
    }
    bool eat(std::string_view op)
    {
        skip();
        if (!s_.substr(at_).starts_with(op)) return false;
        // "<" isn't "<=", "!" isn't "!=".
        if (op.size() == 1 && at_ + 1 < s_.size() && s_[at_ + 1] == '=' &&
            std::string_view("<>!=").find(op[0]) != std::string_view::npos)
            return false;
        at_ += op.size();
        return true;
    }
    static std::unique_ptr<expr> make(std::string op, std::unique_ptr<expr> a, std::unique_ptr<expr> b)
    {
        if (!a || !b) return nullptr;
        auto e = std::make_unique<expr>();
        e->k = expr::binary;
        e->op = std::move(op);
        e->a = std::move(a);
        e->b = std::move(b);
        return e;
    }
    std::unique_ptr<expr> ternary()
    {
        auto cond = binary(0);
        if (!cond || !eat("?")) return cond;
        auto yes = ternary();
        if (!eat(":")) return nullptr;
        auto no = ternary();
        if (!yes || !no) return nullptr;
        auto e = std::make_unique<expr>();
        e->k = expr::ternary;
        e->a = std::move(cond);
        e->b = std::move(yes);
        e->c = std::move(no);
        return e;
    }
    // Precedence climbing, loosest first.
    std::unique_ptr<expr> binary(size_t level)
    {
        static const std::vector<std::vector<std::string_view>> levels = {
            {"||"}, {"&&"}, {"==", "!="}, {"<=", ">=", "<", ">"}, {"+", "-"}, {"*", "/", "%"}};
        if (level == levels.size()) return unary();
        auto left = binary(level + 1);
        while (left)
        {
            std::string_view found;
            for (std::string_view op : levels[level])
                if (eat(op))
                {
                    found = op;
                    break;
                }
            if (found.empty()) break;
            left = make(std::string(found), std::move(left), binary(level + 1));
        }
        return left;
    }
    std::unique_ptr<expr> unary()
    {
        if (eat("!"))
        {
            auto inner = unary();
            if (!inner) return nullptr;
            auto e = std::make_unique<expr>();
            e->k = expr::unary_not;
            e->a = std::move(inner);
            return e;
        }
        if (eat("("))
        {
            auto e = ternary();
            return eat(")") ? std::move(e) : nullptr;
        }
        skip();
        if (at_ < s_.size() && s_[at_] == 'n')
        {
            ++at_;
            auto e = std::make_unique<expr>();
            e->k = expr::var;
            return e;
        }
        if (at_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[at_])))
        {
            auto e = std::make_unique<expr>();
            while (at_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[at_])))
                e->value = e->value * 10 + uint64_t(s_[at_++] - '0');
            return e;
        }
        return nullptr;
    }

    std::string_view s_;
    size_t at_ = 0;
};

// -- catalogs -------------------------------------------------------------------

struct catalog
{
    std::string tag;
    std::unordered_map<std::string, std::vector<std::string>> strings;
    std::unique_ptr<expr> plural;  // null: English's (n != 1)
    size_t forms = 2;

    size_t form(uint64_t n) const
    {
        const uint64_t f = plural ? plural->eval(n) : n != 1;
        return f < forms ? size_t(f) : 0;
    }
};

std::string key(std::string_view context, std::string_view text)
{
    std::string k;
    if (!context.empty())
    {
        k = context;
        k += '\x04';
    }
    k += text;
    return k;
}

// A quoted .po string's contents, escapes undone.
bool unquote(std::string_view s, std::string& out)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    if (s.size() < 2 || s.front() != '"' || s.back() != '"') return false;
    s = s.substr(1, s.size() - 2);
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] != '\\' || i + 1 == s.size())
        {
            out += s[i];
            continue;
        }
        switch (s[++i])
        {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'a': out += '\a'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'v': out += '\v'; break;
        default: out += s[i]; break;  // \" \\ and anything unknown as itself
        }
    }
    return true;
}

// Reads a .po file; nullopt when it can't be read.
std::optional<catalog> read_po(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    catalog cat;
    struct entry
    {
        std::string context, id, plural_id;
        std::map<size_t, std::string> forms;
        bool fuzzy = false, has_id = false;
    } e;
    std::string* field = nullptr;  // what a continuation line appends to
    std::string header;
    auto flush = [&]
    {
        // The header (msgid "") counts even when marked fuzzy, as new files' are.
        if (e.has_id && e.id.empty() && e.context.empty())
            header = e.forms[0];
        else if (e.has_id && !e.fuzzy)
        {
            {
                std::vector<std::string> forms;
                for (auto& [i, s] : e.forms)
                {
                    if (forms.size() != i) break;  // a gap: not trusted
                    forms.push_back(std::move(s));
                }
                // Untranslated entries stay out: the source text shows.
                const bool translated =
                    !forms.empty() && std::ranges::all_of(forms, [](auto& s) { return !s.empty(); });
                if (translated) cat.strings[key(e.context, e.id)] = std::move(forms);
            }
        }
        e = {};
        field = nullptr;
    };
    std::string line;
    bool first = true;
    while (std::getline(in, line))
    {
        if (first && line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);  // a BOM
        first = false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string_view l = line;
        while (!l.empty() && std::isspace(static_cast<unsigned char>(l.front()))) l.remove_prefix(1);
        if (l.empty()) continue;
        if (l.starts_with("#,"))
        {
            // Flags come before the entry they belong to.
            if (e.has_id) flush();
            if (l.find("fuzzy") != std::string_view::npos) e.fuzzy = true;
            continue;
        }
        if (l.front() == '#') continue;
        if (l.front() == '"')
        {
            if (field) unquote(l, *field);
            continue;
        }
        auto value = [&](std::string_view keyword) -> std::optional<std::string_view>
        {
            if (!l.starts_with(keyword)) return std::nullopt;
            std::string_view rest = l.substr(keyword.size());
            if (rest.empty() || !std::isspace(static_cast<unsigned char>(rest.front()))) return std::nullopt;
            while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front())))
                rest.remove_prefix(1);
            return rest;
        };
        if (auto v = value("msgctxt"))
        {
            if (e.has_id) flush();
            field = &e.context;
            unquote(*v, *field);
        }
        else if (auto v = value("msgid"))
        {
            if (e.has_id) flush();
            e.has_id = true;
            field = &e.id;
            unquote(*v, *field);
        }
        else if (auto v = value("msgid_plural"))
        {
            field = &e.plural_id;
            unquote(*v, *field);
        }
        else if (auto v = value("msgstr"))
        {
            field = &e.forms[0];
            unquote(*v, *field);
        }
        else if (l.starts_with("msgstr["))
        {
            const size_t close = l.find(']');
            if (close == std::string_view::npos) continue;
            const size_t i = size_t(std::strtoul(std::string(l.substr(7, close - 7)).c_str(), nullptr, 10));
            l = l.substr(close + 1);
            while (!l.empty() && std::isspace(static_cast<unsigned char>(l.front()))) l.remove_prefix(1);
            field = &e.forms[i];
            unquote(l, *field);
        }
    }
    flush();

    // "Plural-Forms: nplurals=3; plural=(n==1 ? 0 : ...);"
    std::istringstream hs(header);
    for (std::string h; std::getline(hs, h);)
    {
        if (!h.starts_with("Plural-Forms:")) continue;
        const size_t np = h.find("nplurals="), pl = h.find("plural=", np == std::string::npos ? 0 : np + 9);
        if (np == std::string::npos || pl == std::string::npos) break;
        cat.forms = size_t(std::max(1L, std::strtol(h.c_str() + np + 9, nullptr, 10)));
        std::string rule = h.substr(pl + 7);
        if (const size_t semi = rule.rfind(';'); semi != std::string::npos) rule.resize(semi);
        cat.plural = plural_parser(rule).parse();
    }
    return cat;
}

struct state
{
    std::vector<std::string> languages;
    std::map<std::string, catalog, std::less<>> domains;
};

state& global()
{
    static state s;
    return s;
}

const catalog* find(std::string_view domain)
{
    auto& d = global().domains;
    auto it = d.find(domain);
    return it == d.end() ? nullptr : &it->second;
}

std::string base_language(std::string_view tag)
{
    std::string base(tag.substr(0, tag.find_first_of("-_")));
    for (char& c : base) c = char(std::tolower(static_cast<unsigned char>(c)));
    return base;
}

// The files a tag can be in: "pt-BR", then "pt".
std::vector<std::string> candidates(std::string_view tag)
{
    std::vector<std::string> out{std::string(tag)};
    if (const std::string base = base_language(tag); base != tag) out.push_back(base);
    return out;
}

std::filesystem::path po_path(const std::filesystem::path& folder, std::string_view domain,
                              std::string_view tag)
{
    return folder / sys::path_from_utf8(std::string(domain) + "." + std::string(tag) + ".po");
}

}  // namespace

// -- choosing ---------------------------------------------------------------------

void set_languages(std::vector<std::string> preferred, bool override)
{
    global().languages = std::move(preferred);
    if (!override || global().languages.empty()) return;
    // Text the platform draws, and child processes, follow the override.
    std::string colon;
    for (const std::string& tag : global().languages)
    {
        std::string posix = tag;
        std::replace(posix.begin(), posix.end(), '-', '_');
        colon += (colon.empty() ? "" : ":") + posix;
    }
#if defined(_WIN32)
    SetEnvironmentVariableW(L"LANGUAGE", sys::path_from_utf8(colon).c_str());
    _wputenv_s(L"LANGUAGE", sys::path_from_utf8(colon).c_str());
    // Message boxes and common dialogs, where the language is installed.
    std::wstring list;
    for (const std::string& tag : global().languages)
    {
        list += sys::path_from_utf8(tag).wstring();
        list += L'\0';
    }
    list += L'\0';
    ULONG count = 0;
    SetProcessPreferredUILanguages(MUI_LANGUAGE_NAME, list.c_str(), &count);
#else
    // gettext reads LANGUAGE only when the locale isn't "C": GTK's own text
    // and child processes (git) follow it.
    setenv("LANGUAGE", colon.c_str(), 1);
#endif
}

const std::vector<std::string>& languages()
{
    return global().languages;
}

std::string load(std::string_view domain, const std::filesystem::path& folder)
{
    auto& d = global().domains;
    d.erase(std::string(domain));
    for (const std::string& tag : global().languages)
    {
        if (base_language(tag) == "en") return {};  // English first: as written
        for (const std::string& name : candidates(tag))
            if (auto cat = read_po(po_path(folder, domain, name)))
            {
                cat->tag = name;
                d.emplace(std::string(domain), std::move(*cat));
                return name;
            }
    }
    return {};
}

std::string loaded(std::string_view domain)
{
    const catalog* c = find(domain);
    return c ? c->tag : std::string();
}

// -- lookups ----------------------------------------------------------------------

const char* tr(std::string_view domain, const char* text)
{
    return tr_ctx(domain, {}, text);
}

const char* tr_ctx(std::string_view domain, std::string_view context, const char* text)
{
    const catalog* c = find(domain);
    if (!c) return text;
    auto it = c->strings.find(key(context, text));
    return it == c->strings.end() ? text : it->second[0].c_str();
}

const char* tr_n(std::string_view domain, const char* one, const char* many, uint64_t n)
{
    const catalog* c = find(domain);
    if (c)
        if (auto it = c->strings.find(key({}, one)); it != c->strings.end())
        {
            const size_t f = c->form(n);
            if (f < it->second.size()) return it->second[f].c_str();
        }
    return n == 1 ? one : many;
}

std::vector<language> available(std::string_view domain, const std::filesystem::path& folder)
{
    std::vector<language> out;
    const std::string prefix = std::string(domain) + ".";
    std::error_code ec;
    for (const auto& f : std::filesystem::directory_iterator(folder, ec))
    {
        const std::string name = sys::path_to_utf8(f.path().filename());
        if (!name.starts_with(prefix) || !name.ends_with(".po")) continue;
        const std::string tag = name.substr(prefix.size(), name.size() - prefix.size() - 3);
        if (!tag.empty()) out.push_back({tag, native_name(tag)});
    }
    std::ranges::sort(out, {}, &language::name);
    return out;
}

std::string native_name(std::string_view tag)
{
    // Each language's name for itself; regional variants add their country.
    static const std::unordered_map<std::string_view, std::string_view> names = {
        {"ar", "العربية"},       {"bg", "Български"},    {"ca", "Català"},       {"cs", "Čeština"},
        {"da", "Dansk"},          {"de", "Deutsch"},       {"el", "Ελληνικά"},     {"en", "English"},
        {"es", "Español"},        {"et", "Eesti"},         {"eu", "Euskara"},      {"fa", "فارسی"},
        {"fi", "Suomi"},          {"fr", "Français"},      {"gl", "Galego"},       {"he", "עברית"},
        {"hi", "हिन्दी"},          {"hr", "Hrvatski"},      {"hu", "Magyar"},       {"id", "Bahasa Indonesia"},
        {"it", "Italiano"},       {"ja", "日本語"},         {"ko", "한국어"},        {"lt", "Lietuvių"},
        {"lv", "Latviešu"},       {"nb", "Norsk bokmål"},  {"nl", "Nederlands"},   {"nn", "Norsk nynorsk"},
        {"pl", "Polski"},         {"pt", "Português"},     {"pt-BR", "Português (Brasil)"},
        {"ro", "Română"},         {"ru", "Русский"},       {"sk", "Slovenčina"},   {"sl", "Slovenščina"},
        {"sr", "Српски"},         {"sv", "Svenska"},       {"th", "ไทย"},           {"tr", "Türkçe"},
        {"uk", "Українська"},     {"vi", "Tiếng Việt"},    {"zh-CN", "简体中文"},   {"zh-TW", "繁體中文"},
        {"zh", "中文"},
    };
    if (auto it = names.find(tag); it != names.end()) return std::string(it->second);
    if (auto it = names.find(base_language(tag)); it != names.end())
        return std::string(it->second) + " (" + std::string(tag) + ")";
    return std::string(tag);
}

// -- formatting -------------------------------------------------------------------

std::string detail::format(std::string_view text, const std::vector<std::string>& args)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if ((c == '{' || c == '}') && i + 1 < text.size() && text[i + 1] == c)
        {
            out += c;
            ++i;
            continue;
        }
        if (c == '{')
        {
            size_t j = i + 1, n = 0;
            while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j])))
                n = n * 10 + size_t(text[j++] - '0');
            if (j > i + 1 && j < text.size() && text[j] == '}' && n < args.size())
            {
                out += args[n];
                i = j;
                continue;
            }
        }
        out += c;
    }
    return out;
}

// -- rapport's own strings ----------------------------------------------------------

std::string stock_label(dialog_role r)
{
    switch (r)
    {
    case dialog_role::ok: return tr_ctx("rapport", "button", "_OK");
    case dialog_role::cancel: return tr_ctx("rapport", "button", "_Cancel");
    case dialog_role::apply: return tr_ctx("rapport", "button", "_Apply");
    case dialog_role::close: return tr_ctx("rapport", "button", "_Close");
    case dialog_role::yes: return tr_ctx("rapport", "button", "_Yes");
    case dialog_role::no: return tr_ctx("rapport", "button", "_No");
    case dialog_role::help: return tr_ctx("rapport", "button", "_Help");
    case dialog_role::reset: return tr_ctx("rapport", "button", "_Reset");
    case dialog_role::none: break;
    }
    return {};
}

std::string shortcut_label(std::string_view shortcut)
{
    // Key names as written in shortcuts, each translated ("Ctrl" is "Strg"
    // in German).
    // KEY() marks them for xgettext (ci/update-pot.sh).
#define KEY(name) name
    static constexpr const char* k_names[] = {
        KEY("Ctrl"),     KEY("Shift"),   KEY("Alt"),       KEY("Meta"),      KEY("Cmd"),
        KEY("Super"),    KEY("Enter"),   KEY("Return"),    KEY("Space"),     KEY("Tab"),
        KEY("Esc"),      KEY("Del"),     KEY("Delete"),    KEY("Ins"),       KEY("Insert"),
        KEY("Backspace"), KEY("Home"),   KEY("End"),       KEY("PgUp"),      KEY("PgDown"),
        KEY("Page Up"),  KEY("Page Down"), KEY("Up"),      KEY("Down"),      KEY("Left"),
        KEY("Right")};
#undef KEY
    std::string out;
    size_t at = 0;
    while (at <= shortcut.size())
    {
        // "+" separates keys, but a trailing "+" is the key itself (Ctrl++).
        size_t plus = shortcut.find('+', at);
        if (plus == at && plus + 1 == shortcut.size()) plus = std::string_view::npos;
        const std::string_view part =
            shortcut.substr(at, plus == std::string_view::npos ? std::string_view::npos : plus - at);
        bool named = false;
        for (const char* name : k_names)
            if (part == name)
            {
                out += tr("rapport", name);
                named = true;
                break;
            }
        if (!named) out += part;
        if (plus == std::string_view::npos) break;
        out += '+';
        at = plus + 1;
    }
    return out;
}

}  // namespace rap::i18n

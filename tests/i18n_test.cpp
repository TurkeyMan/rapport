// rap::i18n against the catalogs in tests/i18n:
//   rapport-i18n-test <path to tests/i18n>
#include <rapport/i18n.h>
#include <rapport/sys.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{

int fails = 0;

void eq(const std::string& got, const std::string& want, const char* what)
{
    if (got == want)
    {
        std::printf("ok   %s\n", what);
        return;
    }
    ++fails;
    std::printf("FAIL %s: got [%s] want [%s]\n", what, got.c_str(), want.c_str());
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: %s <tests/i18n folder>\n", argv[0]);
        return 2;
    }
    namespace i = rap::i18n;
    const std::filesystem::path dir = rap::sys::path_from_utf8(argv[1]);
    i::set_languages({"de-AT", "en"});
    eq(i::load("app", dir), "de", "de-AT falls back to de");
    eq(i::load("rapport", dir), "de", "rapport domain");
    eq(i::tr("app", "Open"), "Öffnen", "plain");
    eq(i::tr_ctx("app", "state", "Open"), "Offen", "context");
    eq(i::tr("app", "Push"), "Push", "fuzzy skipped");
    eq(i::tr("app", "Untranslated"), "Untranslated", "empty msgstr skipped");
    eq(i::tr("app", "Missing"), "Missing", "unknown");
    eq(i::tr("app", "multi line\n"), "mehrzeilig\n", "continuations and escapes");
    eq(i::format(i::tr("app", "{0} of {1}"), 3, "zehn"), "zehn davon 3", "reordered placeholders");
    eq(i::format("{{x}} {0} {5}", 1.5), "{x} 1.5 {5}", "braces and missing arg");
    eq(i::format(i::tr_n("app", "{0} file", "{0} files", 1), 1), "1 Datei", "plural one");
    eq(i::format(i::tr_n("app", "{0} file", "{0} files", 7), 7), "7 Dateien", "plural many");
    eq(i::stock_label(rap::dialog_role::cancel), "_Abbrechen", "stock label");
    eq(i::stock_label(rap::dialog_role::ok), "_OK", "stock label untranslated");
    eq(i::shortcut_label("Ctrl+Shift+S"), "Strg+Umschalt+S", "shortcut");
    eq(i::shortcut_label("Ctrl++"), "Strg++", "shortcut plus key");
    i::set_languages({"pl"});
    eq(i::load("app", dir), "pl", "polish");
    const char* want[] = {"0 plików", "1 plik", "2 pliki", "5 plików", "12 plików", "22 pliki", "25 plików", "112 plików"};
    int ns[] = {0, 1, 2, 5, 12, 22, 25, 112};
    for (int k = 0; k < 8; ++k)
        eq(i::format(i::tr_n("app", "{0} file", "{0} files", ns[k]), ns[k]), want[k], "polish plural rule");
    i::set_languages({"en-GB", "de"});
    eq(i::load("app", dir), "", "English preferred first");
    eq(i::tr("app", "Open"), "Open", "as written");
    auto av = i::available("app", dir);
    std::string list;
    for (auto& l : av) list += l.tag + "=" + l.name + ";";
    eq(list, "de=Deutsch;pl=Polski;", "available");
    // An override reaches child processes: gettext programs (git) read
    // LANGUAGE. On Windows from the process's environment, which children
    // inherit (a DLL with its own C runtime has its own getenv copy).
    i::set_languages({"pt-BR", "de"}, true);
#ifdef _WIN32
    wchar_t buf[64] = {};
    GetEnvironmentVariableW(L"LANGUAGE", buf, 64);
    const std::string env = rap::sys::path_to_utf8(std::filesystem::path(buf));
#else
    const char* e = std::getenv("LANGUAGE");
    const std::string env = e ? e : "";
#endif
    eq(env, "pt_BR:de", "override sets LANGUAGE");

    std::string langs;
    for (auto& t : rap::sys::ui_languages()) langs += t + ";";
    std::printf("ui_languages: %s\n", langs.c_str());  // whatever this machine says
    return fails ? 1 : 0;
}

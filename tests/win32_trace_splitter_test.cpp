#include <rapport/rapport.h>
#include <thread>
#include <windows.h>

#ifdef RAPPORT_TEST_BUILTIN_WIN32
RAPPORT_BUILTIN_BACKEND(win32);
extern "C" void* rapport_win32_trace_splitter_bar(rap::splitter*, unsigned);
#endif

int main(int argc, char** argv)
{
    auto ui = rap::ui::load(argc, argv, {.backend = "win32"});
    if (!ui) return 1;
    using lookup = void* (*)(rap::splitter*, unsigned);
#ifdef RAPPORT_TEST_BUILTIN_WIN32
    lookup bar = rapport_win32_trace_splitter_bar;
#else
    HMODULE module = GetModuleHandleW(L"rapport_win32.dll");
    lookup bar =
        module
            ? reinterpret_cast<lookup>(GetProcAddress(module, "rapport_win32_trace_splitter_bar"))
            : nullptr;
#endif
    if (!bar || bar(nullptr, 0)) return 1;
    using namespace rap::layout;
    rap::splitter* split = nullptr;
    auto win = (*ui)->create(window{
        "Trace splitter lookup",
        {splitter{rap::orientation::horizontal, label{"left"}, label{"right"}}.bind(split)}});
    win->show();
    HWND h = static_cast<HWND>(bar(split, 0));
    if (!h || !IsWindow(h) || bar(split, 1)) return 1;
    wchar_t cls[64]{};
    GetClassNameW(h, cls, 64);
    if (wcscmp(cls, L"rapportHost")) return 1;
    bool rejected = false;
    std::thread worker([&] { rejected = bar(split, 0) == nullptr; });
    worker.join();
    return rejected ? 0 : 1;
}

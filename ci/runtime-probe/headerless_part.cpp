// Built with the static runtime and no CRT headers, so no RuntimeLibrary
// record for LNK2038: only its /DEFAULTLIB:LIBCMT conflicts (LNK4098, an
// error for a shared rapport library). The "headerless" case.
extern "C" void* malloc(unsigned long long);
extern "C" void* headerless_part() { return malloc(16); }

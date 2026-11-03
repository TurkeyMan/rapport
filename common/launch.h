// Starting a program and not waiting for it: shared by the core (rap::sys's
// open_path, open_url) and the shell extensions (rap::shell::launch), each
// compiling launch.cpp in. Private: not installed, and depending on nothing
// but the standard library and the OS, so the extensions stay free of the
// core.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Not among the core library's exports: each user has its own copy.
#if defined(__GNUC__) && !defined(_WIN32)
#pragma GCC visibility push(hidden)
#endif

namespace rap::launch
{

// Appends one argument to a Windows command line, a space apart from what's
// there, quoted so that CommandLineToArgvW and the C runtimes read it back
// as it was. Plain C++, for the tests to check anywhere.
void append_quoted(std::wstring& line, std::wstring_view arg);

#ifndef _WIN32

// argv[0] (looked up on PATH) with the rest as its arguments, started in
// `dir` (or here) without waiting for it, its input and output /dev/null. A
// shell is spawned to background it and exits at once, so the program is
// reparented to init and nothing is left to reap. False when it couldn't be
// started: not found, or the shell's exit not known to be a success.
bool spawn_detached(const std::vector<std::string>& argv, const std::filesystem::path& dir = {});

// Waits for a child; true only when it exited with 0. False when it didn't,
// or when waitpid fails (not a child, or reaped already: SIGCHLD ignored).
bool exited_ok(int pid);

#endif

}  // namespace rap::launch

#if defined(__GNUC__) && !defined(_WIN32)
#pragma GCC visibility pop
#endif

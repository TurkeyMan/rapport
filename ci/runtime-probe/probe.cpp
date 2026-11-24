// What a shared rapport library does: std:: types, so the C++ runtime.
#include <string>
extern "C" __declspec(dllexport) int probe_size() { return int(std::string(100, 'x').size()); }

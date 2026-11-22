// Built with the static runtime, linked into the probe (the "mixed" case).
#include <string>
int static_part() { return int(std::string(100, 'y').size()); }

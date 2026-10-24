// Apart from shell.cpp: it names the module's identity, which an app that
// links rapport::shell only to register its extension doesn't define.
#include <rapport/shell.h>

namespace rap::shell
{

std::filesystem::path resource_dir()
{
    const char* dir = this_extension.resource_dir;
    return dir && *dir ? std::filesystem::path(dir) : module_dir();
}

}  // namespace rap::shell

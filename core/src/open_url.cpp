#include "dect2/open_url.h"
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <cerrno>
extern char** environ;
#endif
namespace dect2 {
void openUrl(const std::string& url) {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
#ifdef __APPLE__
    const char* command = "/usr/bin/open";
#else
    const char* command = "xdg-open";
#endif
    char* args[] = {const_cast<char*>(command), const_cast<char*>(url.c_str()), nullptr};
    pid_t child;
    if (posix_spawnp(&child, command, nullptr, nullptr, args, environ) == 0) {
        while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
    }
#endif
}
} // namespace dect2

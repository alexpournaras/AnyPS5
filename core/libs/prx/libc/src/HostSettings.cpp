#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <string>
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#else
#include <climits>
#include <unistd.h>
#endif

namespace {

std::filesystem::path ExecutableDirectory() {
#ifdef _WIN32
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return {};
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    return std::filesystem::path(path).parent_path();
#elif defined(__APPLE__)
    char path[PATH_MAX];
    std::uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) return {};
    return std::filesystem::path(path).parent_path();
#else
    char path[PATH_MAX];
    const auto length = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length <= 0) return {};
    path[length] = '\0';
    return std::filesystem::path(path).parent_path();
#endif
}

std::string_view Trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

// anyps5.env next to the executable holds a title's settings as NAME=VALUE lines (blank lines and
// lines starting with # are skipped), so the relinked executable runs with them when started
// directly. A variable already set in the environment keeps its value. libc.prx is the first
// runtime library loaded, so the settings are in place before any other library reads them.
void ApplySettingsFile() {
    const auto directory = ExecutableDirectory();
    if (directory.empty()) return;
    const auto path = directory / "anyps5.env";
    std::ifstream file(path);
    if (!file) return;
    int applied = 0, kept = 0;
    std::string line;
    while (std::getline(file, line)) {
        const auto text = Trim(line);
        if (text.empty() || text.front() == '#') continue;
        const auto separator = text.find('=');
        if (separator == std::string_view::npos || separator == 0) {
            std::fprintf(stderr, "[settings] %s: ignored line without NAME=VALUE: %.*s\n", path.string().c_str(), static_cast<int>(text.size()), text.data());
            continue;
        }
        const std::string name(Trim(text.substr(0, separator)));
        const std::string value(Trim(text.substr(separator + 1)));
        if (std::getenv(name.c_str()) != nullptr) {
            ++kept;
            continue;
        }
#ifdef _WIN32
        _putenv_s(name.c_str(), value.c_str());
#else
        setenv(name.c_str(), value.c_str(), 0);
#endif
        ++applied;
    }
    std::fprintf(stderr, "[settings] %s: %d applied, %d already set in the environment\n", path.string().c_str(), applied, kept);
}

[[maybe_unused]] const bool settingsApplied = (ApplySettingsFile(), true);

}

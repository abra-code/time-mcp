#include "Platform.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

namespace timemcp {

int64_t currentTime() {
    static const char *const fixed = getenv("TIME_MCP_TEST_NOW");
    if (fixed != nullptr && fixed[0] != '\0') {
        char *end = nullptr;
        errno = 0;
        const long long value = strtoll(fixed, &end, 10);
        // Years 1 to 9999, the range the date formatting is written for.
        if (errno == 0 && end != fixed && *end == '\0' && value >= -62135596800LL && value <= 253402300799LL) {
            return static_cast<int64_t>(value);
        }
    }
    return static_cast<int64_t>(time(nullptr));
}

static bool isDirectory(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

const std::string &zoneDatabaseDirectory() {
    static const std::string directory = [] {
        const char *fromEnvironment = getenv("TZDIR");
        if (fromEnvironment != nullptr && fromEnvironment[0] == '/') {
            return std::string(fromEnvironment);
        }
        static const char *const kLocations[] = {
            "/usr/share/zoneinfo",
            "/usr/lib/zoneinfo",
            "/usr/share/lib/zoneinfo",
            "/etc/zoneinfo",
        };
        for (const char *location : kLocations) {
            if (isDirectory(location)) {
                return std::string(location);
            }
        }
        return std::string(kLocations[0]);
    }();
    return directory;
}

// "/var/db/timezone/zoneinfo/Europe/Warsaw" -> "Europe/Warsaw".
static std::string nameAfterZoneinfo(const std::string &path) {
    static const char kMarker[] = "/zoneinfo/";
    const size_t position = path.rfind(kMarker);
    if (position == std::string::npos) {
        return std::string();
    }
    return path.substr(position + sizeof(kMarker) - 1);
}

std::vector<std::string> localZoneCandidates() {
    std::vector<std::string> candidates;

    // TZ overrides the system setting, as it does for every other program.
    // Only the ":Area/City" and "Area/City" forms name a database zone.
    const char *fromEnvironment = getenv("TZ");
    if (fromEnvironment != nullptr && fromEnvironment[0] != '\0') {
        std::string name(fromEnvironment[0] == ':' ? fromEnvironment + 1 : fromEnvironment);
        if (!name.empty() && name[0] == '/') {
            name = nameAfterZoneinfo(name);
        }
        if (!name.empty()) {
            candidates.push_back(name);
        }
    }

    char target[PATH_MAX];
    const ssize_t length = readlink("/etc/localtime", target, sizeof(target) - 1);
    if (length > 0) {
        target[length] = '\0';
        const std::string name = nameAfterZoneinfo(target);
        if (!name.empty()) {
            candidates.push_back(name);
        }
    }

    // Debian and its relatives also keep the name in a one-line file.
    FILE *file = fopen("/etc/timezone", "r");
    if (file != nullptr) {
        char line[256];
        if (fgets(line, sizeof(line), file) != nullptr) {
            line[strcspn(line, " \t\r\n")] = '\0';
            if (line[0] != '\0') {
                candidates.push_back(line);
            }
        }
        fclose(file);
    }

    candidates.push_back("UTC");
    return candidates;
}

} // namespace timemcp

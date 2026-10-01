// time-mcp: a Model Context Protocol server for the current time and time
// zone conversions. It reads requests on standard input and answers on
// standard output; it opens no sockets and writes no files.

#include "McpStdio.h"
#include "Platform.h"
#include "TimeTools.h"
#include "TimeZone.h"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

#ifndef TIME_MCP_VERSION
#define TIME_MCP_VERSION "0.0.0"
#endif

using namespace timemcp;

static void printUsage(FILE *stream) {
    fprintf(stream,
            "Usage: time-mcp [--local-timezone <name>]\n"
            "       time-mcp --version | --help\n"
            "\n"
            "A Model Context Protocol server on standard input and output with two\n"
            "tools: get_current_time and convert_time.\n"
            "\n"
            "  --local-timezone <name>  IANA name of the time zone to suggest when the\n"
            "                           user names none (default: this machine's zone)\n"
            "  --version                print the version and exit\n"
            "  --help                   print this text and exit\n"
            "\n"
            "Time zones are read from the system's zone database, or from the\n"
            "directory named by TZDIR.\n");
}

int main(int argc, char **argv) {
    std::string localZone;
    bool localZoneGiven = false;
    for (int index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        static const char kLocalOption[] = "--local-timezone";
        const size_t optionLength = sizeof(kLocalOption) - 1;
        if (strcmp(argument, "--version") == 0) {
            printf("time-mcp %s\n", TIME_MCP_VERSION);
            return 0;
        }
        if (strcmp(argument, "--help") == 0 || strcmp(argument, "-h") == 0) {
            printUsage(stdout);
            return 0;
        }
        if (strcmp(argument, kLocalOption) == 0) {
            if (index + 1 >= argc) {
                fprintf(stderr, "time-mcp: %s needs a time zone name\n", kLocalOption);
                return 2;
            }
            localZone = argv[++index];
            localZoneGiven = true;
        } else if (strncmp(argument, kLocalOption, optionLength) == 0 && argument[optionLength] == '=') {
            localZone = argument + optionLength + 1;
            localZoneGiven = true;
        } else {
            fprintf(stderr, "time-mcp: unknown option: %s\n", argument);
            printUsage(stderr);
            return 2;
        }
    }

    TimeZone zone;
    std::string reason;
    if (localZoneGiven) {
        // A wrong name here is a configuration mistake; say so and stop,
        // rather than advertise a zone that every call would then reject.
        if (!TimeZone::load(localZone, zone, reason)) {
            fprintf(stderr, "time-mcp: --local-timezone '%s': %s (zone database: %s)\n", localZone.c_str(),
                    reason.c_str(), zoneDatabaseDirectory().c_str());
            return 2;
        }
    } else {
        for (const std::string &candidate : localZoneCandidates()) {
            if (TimeZone::load(candidate, zone, reason)) {
                localZone = candidate;
                break;
            }
        }
        if (localZone.empty()) {
            fprintf(stderr, "time-mcp: no time zone database found in %s: %s\n", zoneDatabaseDirectory().c_str(),
                    reason.c_str());
            return 2;
        }
    }

    // A client that goes away mid-reply must not kill the process with a
    // signal; the failed write ends the loop instead.
    signal(SIGPIPE, SIG_IGN);

    TimeTools tools(localZone);
    McpServer server("time-mcp", TIME_MCP_VERSION, tools);
    return server.run(STDIN_FILENO, STDOUT_FILENO);
}

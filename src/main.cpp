// time-mcp: a Model Context Protocol server for the current time and time
// zone conversions. It reads requests on standard input and answers on
// standard output; it opens no sockets and writes no files.

#include "McpStdio.h"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#ifndef TIME_MCP_VERSION
#define TIME_MCP_VERSION "0.0.0"
#endif

using namespace timemcp;

namespace {

// The protocol with no tools behind it: an empty list, and every call is for
// an unknown tool.
class NoTools : public ToolProvider {
public:
    void listTools(yyjson_mut_doc *, yyjson_mut_val *, bool) override {}
    bool callTool(const char *, yyjson_val *, yyjson_mut_doc *, ToolResult &) override { return false; }
};

} // namespace

static void printUsage(FILE *stream) {
    fprintf(stream,
            "Usage: time-mcp\n"
            "       time-mcp --version | --help\n"
            "\n"
            "A Model Context Protocol server on standard input and output.\n"
            "\n"
            "  --version                print the version and exit\n"
            "  --help                   print this text and exit\n");
}

int main(int argc, char **argv) {
    for (int index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        if (strcmp(argument, "--version") == 0) {
            printf("time-mcp %s\n", TIME_MCP_VERSION);
            return 0;
        }
        if (strcmp(argument, "--help") == 0 || strcmp(argument, "-h") == 0) {
            printUsage(stdout);
            return 0;
        }
        fprintf(stderr, "time-mcp: unknown option: %s\n", argument);
        printUsage(stderr);
        return 2;
    }

    // A client that goes away mid-reply must not kill the process with a
    // signal; the failed write ends the loop instead.
    signal(SIGPIPE, SIG_IGN);

    NoTools tools;
    McpServer server("time-mcp", TIME_MCP_VERSION, tools);
    return server.run(STDIN_FILENO, STDOUT_FILENO);
}

// A Model Context Protocol server over standard input and output:
// newline-delimited JSON-RPC 2.0, one message per line. It knows the
// protocol and nothing about any particular tool; tools come from a
// ToolProvider.
//
// It serves both protocol eras in one process:
// - modern (2026-07-28): no handshake; every request names its protocol
//   version in params._meta and is answered on its own;
// - legacy (2025-11-25 and earlier): the client opens with "initialize".

#pragma once

#include "yyjson.h"

#include <string>

namespace timemcp {

struct ToolResult {
    // The result as a JSON value, or null when the call failed.
    yyjson_mut_val *structured = nullptr;
    // What went wrong, in words the calling model can act on.
    std::string errorText;
};

class ToolProvider {
public:
    virtual ~ToolProvider() = default;

    // Appends every Tool object to `tools`, always in the same order.
    // `withOutputSchema` is false for protocol revisions older than output
    // schemas.
    virtual void listTools(yyjson_mut_doc *doc, yyjson_mut_val *tools, bool withOutputSchema) = 0;

    // Runs the tool. Returns false when there is no tool called `name`.
    // `arguments` is a JSON object or null.
    virtual bool callTool(const char *name, yyjson_val *arguments, yyjson_mut_doc *doc, ToolResult &result) = 0;
};

class McpServer {
public:
    McpServer(const char *name, const char *version, ToolProvider &tools);

    // Reads requests until end of input, answering each complete line.
    // Returns the process exit status.
    int run(int inputDescriptor, int outputDescriptor);

    // Handles one line and returns the reply line (without the newline), or
    // an empty string when the line calls for no reply.
    std::string handleLine(const char *line, size_t length);

private:
    struct Request;

    yyjson_mut_val *handleMessage(yyjson_mut_doc *doc, yyjson_val *message);
    yyjson_mut_val *dispatch(yyjson_mut_doc *doc, const Request &request, int &errorCode, std::string &errorText);
    yyjson_mut_val *supportedVersions(yyjson_mut_doc *doc) const;
    yyjson_mut_val *serverInfo(yyjson_mut_doc *doc) const;
    yyjson_mut_val *capabilities(yyjson_mut_doc *doc) const;

    const char *name_;
    const char *version_;
    ToolProvider &tools_;
    // The revision agreed in "initialize", for legacy requests that follow.
    const char *legacyVersion_;
};

} // namespace timemcp

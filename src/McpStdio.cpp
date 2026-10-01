#include "McpStdio.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace timemcp {

namespace {

const char kModernVersion[] = "2026-07-28";
// Newest first; the head answers an "initialize" that asks for anything else.
const char *const kLegacyVersions[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
// Structured tool output and output schemas arrived with this revision.
const char kStructuredOutputVersion[] = "2025-06-18";

const char kMetaProtocolVersion[] = "io.modelcontextprotocol/protocolVersion";
const char kMetaClientCapabilities[] = "io.modelcontextprotocol/clientCapabilities";
const char kMetaServerInfo[] = "io.modelcontextprotocol/serverInfo";

const int kParseError = -32700;
const int kInvalidRequest = -32600;
const int kMethodNotFound = -32601;
const int kInvalidParams = -32602;
const int kInternalError = -32603;
const int kUnsupportedProtocolVersion = -32022;

// A longer line than this is not a request this server could be sent.
const size_t kMaxLineLength = 1 << 20;

// More members than this in one batch is not a client at work; one error
// answers the lot, instead of a reply many times the size of the request.
const size_t kMaxBatchSize = 100;

// Tool lists and discovery results change only when the server is restarted.
const int64_t kCacheLifetimeMs = 3600000;

const char *knownLegacyVersion(const char *version) {
    for (const char *known : kLegacyVersions) {
        if (strcmp(known, version) == 0) {
            return known;
        }
    }
    return nullptr;
}

// A JSON string may hold a NUL ("\u0000"), which would end a C string
// comparison early: "ping\u0000x" is not "ping".
bool hasEmbeddedNul(yyjson_val *text) {
    return strlen(yyjson_get_str(text)) != yyjson_get_len(text);
}

yyjson_mut_val *errorReply(yyjson_mut_doc *doc, yyjson_val *id, int code, const std::string &text,
                           yyjson_mut_val *data) {
    yyjson_mut_val *reply = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, reply, "jsonrpc", "2.0");
    yyjson_mut_obj_add_val(doc, reply, "id", id != nullptr ? yyjson_val_mut_copy(doc, id) : yyjson_mut_null(doc));
    yyjson_mut_val *error = yyjson_mut_obj_add_obj(doc, reply, "error");
    yyjson_mut_obj_add_int(doc, error, "code", code);
    yyjson_mut_obj_add_strncpy(doc, error, "message", text.data(), text.size());
    if (data != nullptr) {
        yyjson_mut_obj_add_val(doc, error, "data", data);
    }
    return reply;
}

std::string serialize(yyjson_mut_val *root) {
    size_t length = 0;
    // Minified output never contains a newline, as the transport requires.
    char *text = yyjson_mut_val_write(root, YYJSON_WRITE_NOFLAG, &length);
    if (text == nullptr) {
        return "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"Internal error\"}}";
    }
    std::string line(text, length);
    free(text);
    return line;
}

bool writeAll(int descriptor, const char *data, size_t size) {
    size_t total = 0;
    while (total < size) {
        const ssize_t count = write(descriptor, data + total, size - total);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        total += static_cast<size_t>(count);
    }
    return true;
}

} // namespace

struct McpServer::Request {
    yyjson_val *id = nullptr;
    const char *method = nullptr;
    yyjson_val *params = nullptr;
    bool modern = false;
    // The revision this request is served under.
    const char *version = nullptr;
};

McpServer::McpServer(const char *name, const char *version, ToolProvider &tools)
    : name_(name), version_(version), tools_(tools), legacyVersion_(kLegacyVersions[0]) {}

yyjson_mut_val *McpServer::supportedVersions(yyjson_mut_doc *doc) const {
    yyjson_mut_val *versions = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_str(doc, versions, kModernVersion);
    for (const char *version : kLegacyVersions) {
        yyjson_mut_arr_add_str(doc, versions, version);
    }
    return versions;
}

yyjson_mut_val *McpServer::serverInfo(yyjson_mut_doc *doc) const {
    yyjson_mut_val *info = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, info, "name", name_);
    yyjson_mut_obj_add_str(doc, info, "version", version_);
    return info;
}

yyjson_mut_val *McpServer::capabilities(yyjson_mut_doc *doc) const {
    yyjson_mut_val *all = yyjson_mut_obj(doc);
    yyjson_mut_val *tools = yyjson_mut_obj_add_obj(doc, all, "tools");
    yyjson_mut_obj_add_bool(doc, tools, "listChanged", false);
    return all;
}

yyjson_mut_val *McpServer::handleMessage(yyjson_mut_doc *doc, yyjson_val *message) {
    if (!yyjson_is_obj(message)) {
        return errorReply(doc, nullptr, kInvalidRequest, "Invalid request: a message must be a JSON object", nullptr);
    }
    yyjson_val *id = yyjson_obj_get(message, "id");
    yyjson_val *method = yyjson_obj_get(message, "method");
    const bool idIsValid = yyjson_is_str(id) || yyjson_is_int(id);
    if (method == nullptr) {
        // A response. Clients have nothing to respond to; ignore it.
        if (yyjson_obj_get(message, "result") != nullptr || yyjson_obj_get(message, "error") != nullptr) {
            return nullptr;
        }
        return errorReply(doc, idIsValid ? id : nullptr, kInvalidRequest, "Invalid request: no method", nullptr);
    }
    if (!yyjson_is_str(method)) {
        return errorReply(doc, idIsValid ? id : nullptr, kInvalidRequest, "Invalid request: method must be a string",
                          nullptr);
    }
    if (id == nullptr) {
        // A notification: never answered, whatever it says.
        return nullptr;
    }
    if (!idIsValid) {
        return errorReply(doc, nullptr, kInvalidRequest, "Invalid request: id must be a string or an integer", nullptr);
    }
    if (hasEmbeddedNul(method)) {
        return errorReply(doc, id, kMethodNotFound, "Method not found", nullptr);
    }

    Request request;
    request.id = id;
    request.method = yyjson_get_str(method);
    request.params = yyjson_obj_get(message, "params");
    if (request.params != nullptr && !yyjson_is_obj(request.params)) {
        return errorReply(doc, id, kInvalidParams, "Invalid params: params must be an object", nullptr);
    }

    // Which era is this request? A modern one says so itself, in _meta.
    yyjson_val *meta = yyjson_obj_get(request.params, "_meta");
    yyjson_val *declared = yyjson_obj_get(meta, kMetaProtocolVersion);
    if (strcmp(request.method, "initialize") == 0) {
        request.version = legacyVersion_;
    } else if (declared != nullptr) {
        if (!yyjson_is_str(declared)) {
            return errorReply(doc, id, kInvalidParams,
                              std::string("Invalid params: _meta.") + kMetaProtocolVersion + " must be a string",
                              nullptr);
        }
        // A version with a NUL inside is no version this server knows.
        const char *version = hasEmbeddedNul(declared) ? "" : yyjson_get_str(declared);
        if (strcmp(version, kModernVersion) == 0) {
            request.modern = true;
            request.version = kModernVersion;
            if (!yyjson_is_obj(yyjson_obj_get(meta, kMetaClientCapabilities))) {
                return errorReply(doc, id, kInvalidParams,
                                  std::string("Invalid params: _meta.") + kMetaClientCapabilities + " is required",
                                  nullptr);
            }
        } else if (knownLegacyVersion(version) != nullptr) {
            request.version = knownLegacyVersion(version);
        } else {
            yyjson_mut_val *data = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_val(doc, data, "supported", supportedVersions(doc));
            yyjson_mut_obj_add_val(doc, data, "requested", yyjson_val_mut_copy(doc, declared));
            return errorReply(doc, id, kUnsupportedProtocolVersion, "Unsupported protocol version", data);
        }
    } else if (strcmp(request.method, "server/discover") == 0) {
        return errorReply(doc, id, kInvalidParams,
                          std::string("Invalid params: _meta.") + kMetaProtocolVersion + " is required", nullptr);
    } else {
        request.version = legacyVersion_;
    }

    int errorCode = 0;
    std::string errorText;
    yyjson_mut_val *result = dispatch(doc, request, errorCode, errorText);
    if (result == nullptr) {
        return errorReply(doc, id, errorCode, errorText, nullptr);
    }
    if (request.modern) {
        yyjson_mut_val *resultMeta = yyjson_mut_obj_add_obj(doc, result, "_meta");
        yyjson_mut_obj_add_val(doc, resultMeta, kMetaServerInfo, serverInfo(doc));
    }
    yyjson_mut_val *reply = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, reply, "jsonrpc", "2.0");
    yyjson_mut_obj_add_val(doc, reply, "id", yyjson_val_mut_copy(doc, id));
    yyjson_mut_obj_add_val(doc, reply, "result", result);
    return reply;
}

// Returns the result object, or null with the error filled in.
yyjson_mut_val *McpServer::dispatch(yyjson_mut_doc *doc, const Request &request, int &errorCode,
                                    std::string &errorText) {
    yyjson_mut_val *result = yyjson_mut_obj(doc);
    if (request.modern) {
        yyjson_mut_obj_add_str(doc, result, "resultType", "complete");
    }

    if (strcmp(request.method, "initialize") == 0) {
        const char *asked = yyjson_get_str(yyjson_obj_get(request.params, "protocolVersion"));
        const char *known = asked != nullptr ? knownLegacyVersion(asked) : nullptr;
        legacyVersion_ = known != nullptr ? known : kLegacyVersions[0];
        yyjson_mut_obj_add_str(doc, result, "protocolVersion", legacyVersion_);
        yyjson_mut_obj_add_val(doc, result, "capabilities", capabilities(doc));
        yyjson_mut_obj_add_val(doc, result, "serverInfo", serverInfo(doc));
        return result;
    }

    if (request.modern && strcmp(request.method, "server/discover") == 0) {
        yyjson_mut_obj_add_val(doc, result, "supportedVersions", supportedVersions(doc));
        yyjson_mut_obj_add_val(doc, result, "capabilities", capabilities(doc));
        yyjson_mut_obj_add_int(doc, result, "ttlMs", kCacheLifetimeMs);
        yyjson_mut_obj_add_str(doc, result, "cacheScope", "public");
        return result;
    }

    // The modern revision removed ping.
    if (!request.modern && strcmp(request.method, "ping") == 0) {
        return result;
    }

    const bool structuredOutput = strcmp(request.version, kStructuredOutputVersion) >= 0;

    if (strcmp(request.method, "tools/list") == 0) {
        yyjson_mut_val *tools = yyjson_mut_obj_add_arr(doc, result, "tools");
        tools_.listTools(doc, tools, structuredOutput);
        if (request.modern) {
            yyjson_mut_obj_add_int(doc, result, "ttlMs", kCacheLifetimeMs);
            yyjson_mut_obj_add_str(doc, result, "cacheScope", "public");
        }
        return result;
    }

    if (strcmp(request.method, "tools/call") == 0) {
        yyjson_val *name = yyjson_obj_get(request.params, "name");
        yyjson_val *arguments = yyjson_obj_get(request.params, "arguments");
        if (!yyjson_is_str(name)) {
            errorCode = kInvalidParams;
            errorText = "Invalid params: name must be a string";
            return nullptr;
        }
        if (arguments != nullptr && !yyjson_is_obj(arguments)) {
            errorCode = kInvalidParams;
            errorText = "Invalid params: arguments must be an object";
            return nullptr;
        }
        ToolResult outcome;
        if (hasEmbeddedNul(name) || !tools_.callTool(yyjson_get_str(name), arguments, doc, outcome)) {
            errorCode = kInvalidParams;
            errorText = std::string("Unknown tool: ") + yyjson_get_str(name);
            return nullptr;
        }
        // A failed call is a result the model can read and correct, not a
        // protocol error.
        const bool failed = outcome.structured == nullptr;
        std::string text = outcome.errorText;
        if (!failed) {
            size_t length = 0;
            char *pretty = yyjson_mut_val_write(outcome.structured, YYJSON_WRITE_PRETTY_TWO_SPACES, &length);
            if (pretty == nullptr) {
                errorCode = kInternalError;
                errorText = "Internal error";
                return nullptr;
            }
            text.assign(pretty, length);
            free(pretty);
        }
        yyjson_mut_val *content = yyjson_mut_obj_add_arr(doc, result, "content");
        yyjson_mut_val *block = yyjson_mut_arr_add_obj(doc, content);
        yyjson_mut_obj_add_str(doc, block, "type", "text");
        yyjson_mut_obj_add_strncpy(doc, block, "text", text.data(), text.size());
        if (!failed && structuredOutput) {
            yyjson_mut_obj_add_val(doc, result, "structuredContent", outcome.structured);
        }
        yyjson_mut_obj_add_bool(doc, result, "isError", failed);
        return result;
    }

    errorCode = kMethodNotFound;
    errorText = std::string("Method not found: ") + request.method;
    return nullptr;
}

std::string McpServer::handleLine(const char *line, size_t length) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(nullptr);
    if (doc == nullptr) {
        return std::string();
    }
    // yyjson_read wants a mutable buffer only with the in-situ flag; without
    // it the input is left alone.
    yyjson_doc *input = yyjson_read(line, length, YYJSON_READ_NOFLAG);
    yyjson_mut_val *reply = nullptr;
    if (input == nullptr) {
        reply = errorReply(doc, nullptr, kParseError, "Parse error: the line is not valid JSON", nullptr);
    } else {
        yyjson_val *root = yyjson_doc_get_root(input);
        if (yyjson_is_arr(root)) {
            // A batch (revision 2025-03-26). Replies come back as one array;
            // a batch of notifications alone gets no reply.
            if (yyjson_arr_size(root) == 0) {
                reply = errorReply(doc, nullptr, kInvalidRequest, "Invalid request: empty batch", nullptr);
            } else if (yyjson_arr_size(root) > kMaxBatchSize) {
                reply = errorReply(doc, nullptr, kInvalidRequest, "Invalid request: the batch is too large", nullptr);
            } else {
                yyjson_mut_val *replies = yyjson_mut_arr(doc);
                size_t index = 0;
                size_t count = 0;
                yyjson_val *message = nullptr;
                yyjson_arr_foreach(root, index, count, message) {
                    yyjson_mut_val *one = handleMessage(doc, message);
                    if (one != nullptr) {
                        yyjson_mut_arr_append(replies, one);
                    }
                }
                if (yyjson_mut_arr_size(replies) > 0) {
                    reply = replies;
                }
            }
        } else {
            reply = handleMessage(doc, root);
        }
    }
    std::string text;
    if (reply != nullptr) {
        text = serialize(reply);
    }
    yyjson_doc_free(input);
    yyjson_mut_doc_free(doc);
    return text;
}

int McpServer::run(int inputDescriptor, int outputDescriptor) {
    std::string pending;
    // True while skipping the rest of a line that grew past the limit.
    bool discarding = false;
    char chunk[65536];

    auto answer = [&](const char *line, size_t length) -> bool {
        if (length > 0 && line[length - 1] == '\r') {
            --length;
        }
        if (length == 0) {
            return true;
        }
        std::string reply = handleLine(line, length);
        if (reply.empty()) {
            return true;
        }
        reply.push_back('\n');
        return writeAll(outputDescriptor, reply.data(), reply.size());
    };
    auto refuseOversized = [&]() -> bool {
        static const char kReply[] =
            "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Parse error: the line is too long\"}}\n";
        return writeAll(outputDescriptor, kReply, sizeof(kReply) - 1);
    };

    for (;;) {
        const ssize_t count = read(inputDescriptor, chunk, sizeof(chunk));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        size_t start = 0;
        const size_t size = static_cast<size_t>(count);
        while (start < size) {
            const char *newline = static_cast<const char *>(memchr(chunk + start, '\n', size - start));
            const size_t end = newline != nullptr ? static_cast<size_t>(newline - chunk) : size;
            if (!discarding) {
                pending.append(chunk + start, end - start);
                if (pending.size() > kMaxLineLength) {
                    pending.clear();
                    discarding = true;
                }
            }
            if (newline == nullptr) {
                break;
            }
            bool written = true;
            if (discarding) {
                discarding = false;
                written = refuseOversized();
            } else {
                written = answer(pending.data(), pending.size());
                pending.clear();
            }
            if (!written) {
                // The client closed its end; nothing more to do.
                return 0;
            }
            start = end + 1;
        }
    }
    // End of input. A last line without its newline is still a request.
    if (!discarding && !pending.empty()) {
        answer(pending.data(), pending.size());
    }
    return 0;
}

} // namespace timemcp

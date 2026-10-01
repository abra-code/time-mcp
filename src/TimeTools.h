// The two time tools: get_current_time and convert_time.

#pragma once

#include "McpStdio.h"

#include <string>

namespace timemcp {

class TimeTools : public ToolProvider {
public:
    // `localZone` is the machine's time zone name. It appears only in the
    // tool descriptions, as the zone to assume when the user names none.
    explicit TimeTools(const std::string &localZone);
    ~TimeTools() override;

    void listTools(yyjson_mut_doc *doc, yyjson_mut_val *tools, bool withOutputSchema) override;
    bool callTool(const char *name, yyjson_val *arguments, yyjson_mut_doc *doc, ToolResult &result) override;

private:
    void getCurrentTime(yyjson_val *arguments, yyjson_mut_doc *doc, ToolResult &result);
    void convertTime(yyjson_val *arguments, yyjson_mut_doc *doc, ToolResult &result);

    yyjson_doc *definitions_;
};

} // namespace timemcp

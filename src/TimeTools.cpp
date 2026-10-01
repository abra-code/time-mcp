#include "TimeTools.h"

#include "Civil.h"
#include "Platform.h"
#include "TimeZone.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace timemcp {

namespace {

// The tool definitions as clients see them. @LOCAL@ is replaced with the
// machine's time zone name, which has passed TimeZone::isValidName and so
// needs no JSON escaping.
const char kDefinitions[] = R"JSON([
  {
    "name": "get_current_time",
    "title": "Get Current Time",
    "description": "Get current time in a specific timezone",
    "inputSchema": {
      "type": "object",
      "properties": {
        "timezone": {
          "type": "string",
          "description": "IANA timezone name (e.g., 'America/New_York', 'Europe/London'). Use '@LOCAL@' as local timezone if no timezone provided by the user."
        }
      },
      "required": ["timezone"]
    },
    "outputSchema": {
      "type": "object",
      "properties": {
        "timezone": {"type": "string", "description": "The timezone name as given"},
        "datetime": {"type": "string", "description": "Local date and time with the UTC offset, ISO 8601 (e.g., '2026-03-08T14:30:00-07:00')"},
        "day_of_week": {"type": "string", "description": "English weekday name"},
        "is_dst": {"type": "boolean", "description": "Whether daylight saving time is in effect"}
      },
      "required": ["timezone", "datetime", "day_of_week", "is_dst"]
    },
    "annotations": {
      "readOnlyHint": true,
      "destructiveHint": false,
      "idempotentHint": true,
      "openWorldHint": false
    }
  },
  {
    "name": "convert_time",
    "title": "Convert Time",
    "description": "Convert time between timezones",
    "inputSchema": {
      "type": "object",
      "properties": {
        "source_timezone": {
          "type": "string",
          "description": "Source IANA timezone name (e.g., 'America/New_York', 'Europe/London'). Use '@LOCAL@' as local timezone if no source timezone provided by the user."
        },
        "time": {
          "type": "string",
          "description": "Time to convert in 24-hour format (HH:MM)"
        },
        "target_timezone": {
          "type": "string",
          "description": "Target IANA timezone name (e.g., 'Asia/Tokyo', 'America/Los_Angeles'). Use '@LOCAL@' as local timezone if no target timezone provided by the user."
        }
      },
      "required": ["source_timezone", "time", "target_timezone"]
    },
    "outputSchema": {
      "type": "object",
      "properties": {
        "source": {
          "type": "object",
          "description": "The given time, today, in the source timezone",
          "properties": {
            "timezone": {"type": "string", "description": "The timezone name as given"},
            "datetime": {"type": "string", "description": "Local date and time with the UTC offset, ISO 8601"},
            "day_of_week": {"type": "string", "description": "English weekday name"},
            "is_dst": {"type": "boolean", "description": "Whether daylight saving time is in effect"}
          },
          "required": ["timezone", "datetime", "day_of_week", "is_dst"]
        },
        "target": {
          "type": "object",
          "description": "The same instant in the target timezone",
          "properties": {
            "timezone": {"type": "string", "description": "The timezone name as given"},
            "datetime": {"type": "string", "description": "Local date and time with the UTC offset, ISO 8601"},
            "day_of_week": {"type": "string", "description": "English weekday name"},
            "is_dst": {"type": "boolean", "description": "Whether daylight saving time is in effect"}
          },
          "required": ["timezone", "datetime", "day_of_week", "is_dst"]
        },
        "time_difference": {"type": "string", "description": "Target offset minus source offset in hours (e.g., '+9.0h', '-5.5h')"}
      },
      "required": ["source", "target", "time_difference"]
    },
    "annotations": {
      "readOnlyHint": true,
      "destructiveHint": false,
      "idempotentHint": true,
      "openWorldHint": false
    }
  }
])JSON";

const char *const kWeekdays[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

// A model may send anything as a zone name; keep what is quoted back short.
std::string quoted(const std::string &text) {
    const size_t kLimit = 80;
    if (text.size() <= kLimit) {
        return "'" + text + "'";
    }
    // Cut between characters: half of a UTF-8 sequence cannot be written as
    // JSON.
    size_t length = kLimit;
    while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) {
        --length;
    }
    return "'" + text.substr(0, length) + "...'";
}

// Reads a required string argument. On failure sets `error` and returns
// false.
bool stringArgument(yyjson_val *arguments, const char *key, std::string &value, std::string &error) {
    yyjson_val *found = yyjson_obj_get(arguments, key);
    if (found == nullptr) {
        error = std::string("Missing required argument: ") + key;
        return false;
    }
    if (!yyjson_is_str(found)) {
        error = std::string("Argument ") + key + " must be a string";
        return false;
    }
    value.assign(yyjson_get_str(found), yyjson_get_len(found));
    if (value.empty()) {
        error = std::string("Missing required argument: ") + key;
        return false;
    }
    return true;
}

bool loadZone(const std::string &name, TimeZone &zone, std::string &error) {
    std::string reason;
    if (TimeZone::load(name, zone, reason)) {
        return true;
    }
    error = "Unknown timezone " + quoted(name) +
            ". Use an IANA timezone name such as 'America/New_York', 'Europe/London' or 'UTC'.";
    return false;
}

// "HH:MM", 24-hour, one or two digits each, nothing else.
bool parseClockTime(const std::string &text, int &hour, int &minute) {
    const size_t colon = text.find(':');
    if (colon == std::string::npos || colon < 1 || colon > 2) {
        return false;
    }
    const size_t minuteDigits = text.size() - colon - 1;
    if (minuteDigits < 1 || minuteDigits > 2) {
        return false;
    }
    hour = 0;
    minute = 0;
    for (size_t index = 0; index < text.size(); ++index) {
        if (index == colon) {
            continue;
        }
        const char c = text[index];
        if (c < '0' || c > '9') {
            return false;
        }
        if (index < colon) {
            hour = hour * 10 + (c - '0');
        } else {
            minute = minute * 10 + (c - '0');
        }
    }
    return hour <= 23 && minute <= 59;
}

// {"timezone", "datetime", "day_of_week", "is_dst"} for a wall clock reading
// (`wall` is local seconds counted as if UTC) under the given type.
yyjson_mut_val *describe(yyjson_mut_doc *doc, const std::string &name, int64_t wall, const LocalType &type) {
    const int64_t days = floorDiv(wall, 86400);
    const int64_t secondOfDay = wall - days * 86400;
    int64_t year = 0;
    unsigned month = 0;
    unsigned day = 0;
    civilFromDays(days, year, month, day);

    const int32_t offset = type.utcOffset < 0 ? -type.utcOffset : type.utcOffset;
    char text[64];
    int length = snprintf(text, sizeof(text), "%04lld-%02u-%02uT%02d:%02d:%02d%c%02d:%02d", static_cast<long long>(year),
                          month, day, static_cast<int>(secondOfDay / 3600), static_cast<int>(secondOfDay / 60 % 60),
                          static_cast<int>(secondOfDay % 60), type.utcOffset < 0 ? '-' : '+', offset / 3600,
                          offset / 60 % 60);
    if (offset % 60 != 0 && length > 0 && static_cast<size_t>(length) < sizeof(text)) {
        // Offsets with seconds exist only in old local mean time records.
        snprintf(text + length, sizeof(text) - static_cast<size_t>(length), ":%02d", offset % 60);
    }

    yyjson_mut_val *object = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strncpy(doc, object, "timezone", name.data(), name.size());
    yyjson_mut_obj_add_strcpy(doc, object, "datetime", text);
    yyjson_mut_obj_add_str(doc, object, "day_of_week", kWeekdays[weekdayFromDays(days)]);
    yyjson_mut_obj_add_bool(doc, object, "is_dst", type.isDst);
    return object;
}

// "+9.0h", "-8.0h", "+5.75h", "+9.5h".
std::string hoursDifference(int32_t seconds) {
    char text[32];
    if (seconds % 3600 == 0) {
        snprintf(text, sizeof(text), "%+.1fh", seconds / 3600.0);
        return text;
    }
    snprintf(text, sizeof(text), "%+.2f", seconds / 3600.0);
    std::string trimmed(text);
    while (!trimmed.empty() && trimmed.back() == '0') {
        trimmed.pop_back();
    }
    if (!trimmed.empty() && trimmed.back() == '.') {
        trimmed.pop_back();
    }
    return trimmed + "h";
}

} // namespace

TimeTools::TimeTools(const std::string &localZone) : definitions_(nullptr) {
    std::string text(kDefinitions);
    static const char kPlaceholder[] = "@LOCAL@";
    for (size_t at = text.find(kPlaceholder); at != std::string::npos; at = text.find(kPlaceholder, at)) {
        text.replace(at, sizeof(kPlaceholder) - 1, localZone);
        at += localZone.size();
    }
    definitions_ = yyjson_read(text.data(), text.size(), YYJSON_READ_NOFLAG);
    if (definitions_ == nullptr) {
        // Cannot happen with a valid zone name; a server with no tools would
        // only confuse its client.
        fprintf(stderr, "time-mcp: internal error: the tool definitions are not valid JSON\n");
        exit(70);
    }
}

TimeTools::~TimeTools() {
    yyjson_doc_free(definitions_);
}

void TimeTools::listTools(yyjson_mut_doc *doc, yyjson_mut_val *tools, bool withOutputSchema) {
    size_t index = 0;
    size_t count = 0;
    yyjson_val *definition = nullptr;
    yyjson_arr_foreach(yyjson_doc_get_root(definitions_), index, count, definition) {
        yyjson_mut_val *tool = yyjson_val_mut_copy(doc, definition);
        if (!withOutputSchema) {
            yyjson_mut_obj_remove_key(tool, "outputSchema");
        }
        yyjson_mut_arr_append(tools, tool);
    }
}

bool TimeTools::callTool(const char *name, yyjson_val *arguments, yyjson_mut_doc *doc, ToolResult &result) {
    if (strcmp(name, "get_current_time") == 0) {
        getCurrentTime(arguments, doc, result);
        return true;
    }
    if (strcmp(name, "convert_time") == 0) {
        convertTime(arguments, doc, result);
        return true;
    }
    return false;
}

void TimeTools::getCurrentTime(yyjson_val *arguments, yyjson_mut_doc *doc, ToolResult &result) {
    std::string name;
    TimeZone zone;
    if (!stringArgument(arguments, "timezone", name, result.errorText) || !loadZone(name, zone, result.errorText)) {
        return;
    }
    const int64_t now = currentTime();
    const LocalType type = zone.typeAt(now);
    result.structured = describe(doc, name, now + type.utcOffset, type);
}

void TimeTools::convertTime(yyjson_val *arguments, yyjson_mut_doc *doc, ToolResult &result) {
    std::string sourceName;
    std::string targetName;
    std::string timeText;
    TimeZone source;
    TimeZone target;
    if (!stringArgument(arguments, "source_timezone", sourceName, result.errorText) ||
        !stringArgument(arguments, "time", timeText, result.errorText) ||
        !stringArgument(arguments, "target_timezone", targetName, result.errorText) ||
        !loadZone(sourceName, source, result.errorText) || !loadZone(targetName, target, result.errorText)) {
        return;
    }
    int hour = 0;
    int minute = 0;
    if (!parseClockTime(timeText, hour, minute)) {
        result.errorText = "Invalid time " + quoted(timeText) + ". Expected HH:MM in 24-hour format, such as '09:30' or '17:05'.";
        return;
    }

    // The given time on today's date in the source zone.
    const int64_t now = currentTime();
    const int64_t today = floorDiv(now + source.typeAt(now).utcOffset, 86400);
    const int64_t sourceWall = today * 86400 + hour * 3600 + minute * 60;
    const LocalType sourceType = source.typeForWall(sourceWall);
    const int64_t instant = sourceWall - sourceType.utcOffset;
    const LocalType targetType = target.typeAt(instant);

    yyjson_mut_val *object = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, object, "source", describe(doc, sourceName, sourceWall, sourceType));
    yyjson_mut_obj_add_val(doc, object, "target",
                           describe(doc, targetName, instant + targetType.utcOffset, targetType));
    const std::string difference = hoursDifference(targetType.utcOffset - sourceType.utcOffset);
    yyjson_mut_obj_add_strncpy(doc, object, "time_difference", difference.data(), difference.size());
    result.structured = object;
}

} // namespace timemcp

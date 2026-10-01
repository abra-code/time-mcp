#include "TimeZone.h"

#include "Civil.h"
#include "Platform.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

namespace timemcp {

namespace {

const size_t kMaxNameLength = 255;
const size_t kMaxFileSize = 1 << 20;
const size_t kHeaderSize = 44;
// RFC 9636 keeps offsets within about 26 hours of UTC.
const int32_t kMaxOffset = 26 * 3600;

uint32_t readBig32(const uint8_t *bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
}

int64_t readBig64(const uint8_t *bytes) {
    const uint64_t high = readBig32(bytes);
    const uint64_t low = readBig32(bytes + 4);
    return static_cast<int64_t>((high << 32) | low);
}

struct Header {
    uint8_t version = 0;
    uint32_t utCount = 0;
    uint32_t standardCount = 0;
    uint32_t leapCount = 0;
    uint32_t timeCount = 0;
    uint32_t typeCount = 0;
    uint32_t charCount = 0;
};

bool readHeader(const uint8_t *data, size_t size, Header &header) {
    if (size < kHeaderSize || memcmp(data, "TZif", 4) != 0) {
        return false;
    }
    header.version = data[4];
    header.utCount = readBig32(data + 20);
    header.standardCount = readBig32(data + 24);
    header.leapCount = readBig32(data + 28);
    header.timeCount = readBig32(data + 32);
    header.typeCount = readBig32(data + 36);
    header.charCount = readBig32(data + 40);
    // Each count is far below this in any real file; the bound keeps the
    // size arithmetic below from overflowing.
    const uint32_t kLimit = static_cast<uint32_t>(kMaxFileSize);
    return header.utCount <= kLimit && header.standardCount <= kLimit && header.leapCount <= kLimit &&
           header.timeCount <= kLimit && header.typeCount <= kLimit && header.charCount <= kLimit;
}

size_t bodySize(const Header &header, size_t timeSize) {
    return static_cast<size_t>(header.timeCount) * timeSize + header.timeCount +
           static_cast<size_t>(header.typeCount) * 6 + header.charCount +
           static_cast<size_t>(header.leapCount) * (timeSize + 4) + header.standardCount + header.utCount;
}

// --- POSIX TZ rule ("EST5EDT,M3.2.0,M11.1.0") ---

bool skipRuleName(const char *&cursor) {
    if (*cursor == '<') {
        const char *close = strchr(cursor, '>');
        if (close == nullptr || close == cursor + 1) {
            return false;
        }
        cursor = close + 1;
        return true;
    }
    const char *start = cursor;
    while ((*cursor >= 'A' && *cursor <= 'Z') || (*cursor >= 'a' && *cursor <= 'z')) {
        ++cursor;
    }
    return cursor - start >= 3;
}

bool readNumber(const char *&cursor, int maxDigits, int &value) {
    int digits = 0;
    value = 0;
    while (*cursor >= '0' && *cursor <= '9' && digits < maxDigits) {
        value = value * 10 + (*cursor - '0');
        ++cursor;
        ++digits;
    }
    return digits > 0;
}

// [+-]h[h[h]][:mm[:ss]] as seconds.
bool readClock(const char *&cursor, int maxHours, int32_t &seconds) {
    int sign = 1;
    if (*cursor == '+' || *cursor == '-') {
        sign = (*cursor == '-') ? -1 : 1;
        ++cursor;
    }
    int hours = 0;
    int minutes = 0;
    int secs = 0;
    if (!readNumber(cursor, 3, hours) || hours > maxHours) {
        return false;
    }
    if (*cursor == ':') {
        ++cursor;
        if (!readNumber(cursor, 2, minutes) || minutes > 59) {
            return false;
        }
        if (*cursor == ':') {
            ++cursor;
            if (!readNumber(cursor, 2, secs) || secs > 59) {
                return false;
            }
        }
    }
    seconds = sign * (hours * 3600 + minutes * 60 + secs);
    return true;
}

bool readRuleDate(const char *&cursor, RuleDate &date) {
    if (*cursor == 'M') {
        ++cursor;
        date.kind = RuleDate::MonthWeekDay;
        if (!readNumber(cursor, 2, date.month) || date.month < 1 || date.month > 12 || *cursor != '.') {
            return false;
        }
        ++cursor;
        if (!readNumber(cursor, 1, date.week) || date.week < 1 || date.week > 5 || *cursor != '.') {
            return false;
        }
        ++cursor;
        if (!readNumber(cursor, 1, date.day) || date.day > 6) {
            return false;
        }
    } else if (*cursor == 'J') {
        ++cursor;
        date.kind = RuleDate::JulianNoLeap;
        if (!readNumber(cursor, 3, date.day) || date.day < 1 || date.day > 365) {
            return false;
        }
    } else {
        date.kind = RuleDate::ZeroBased;
        if (!readNumber(cursor, 3, date.day) || date.day > 365) {
            return false;
        }
    }
    date.time = 7200;
    if (*cursor == '/') {
        ++cursor;
        // RFC 9636 version 3 files allow -167 to 167 hours here.
        if (!readClock(cursor, 167, date.time)) {
            return false;
        }
    }
    return true;
}

// The rule date in `year` as local seconds counted as if UTC.
int64_t ruleWallSeconds(const RuleDate &date, int64_t year) {
    int64_t days = 0;
    switch (date.kind) {
        case RuleDate::JulianNoLeap:
            // Day 1-365; February 29 is never counted.
            days = daysFromCivil(year, 1, 1) + (date.day - 1);
            if (isLeapYear(year) && date.day >= 60) {
                days += 1;
            }
            break;
        case RuleDate::ZeroBased:
            days = daysFromCivil(year, 1, 1) + date.day;
            break;
        case RuleDate::MonthWeekDay: {
            const unsigned month = static_cast<unsigned>(date.month);
            const int64_t first = daysFromCivil(year, month, 1);
            const int firstWeekday = static_cast<int>(weekdayFromDays(first));
            days = first + (date.day - firstWeekday + 7) % 7 + (date.week - 1) * 7;
            // Week 5 means the last such weekday of the month.
            while (days >= first + daysInMonth(year, month)) {
                days -= 7;
            }
            break;
        }
    }
    return days * 86400 + date.time;
}

int64_t yearOfSeconds(int64_t seconds) {
    int64_t year = 0;
    unsigned month = 0;
    unsigned day = 0;
    civilFromDays(floorDiv(seconds, 86400), year, month, day);
    return year;
}

// --- finding the file ---

std::string joinPath(const std::string &directory, const std::string &name) {
    return directory + "/" + name;
}

// Finds `name` under `directory` comparing each component without regard to
// case, so "utc" and "europe/warsaw" resolve the same way on a case-sensitive
// volume (Linux) as they do on a case-insensitive one (macOS).
std::string findIgnoringCase(const std::string &directory, const std::string &name) {
    std::string current = directory;
    size_t start = 0;
    while (start <= name.size()) {
        size_t end = name.find('/', start);
        if (end == std::string::npos) {
            end = name.size();
        }
        const std::string component = name.substr(start, end - start);
        DIR *handle = opendir(current.c_str());
        if (handle == nullptr) {
            return std::string();
        }
        std::string match;
        for (;;) {
            const struct dirent *entry = readdir(handle);
            if (entry == nullptr) {
                break;
            }
            if (strcasecmp(entry->d_name, component.c_str()) == 0) {
                match = entry->d_name;
                break;
            }
        }
        closedir(handle);
        if (match.empty()) {
            return std::string();
        }
        current = joinPath(current, match);
        start = end + 1;
    }
    return current;
}

bool readWholeFile(const std::string &path, std::vector<uint8_t> &bytes, std::string &reason) {
    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        reason = (errno == ENOENT || errno == ENOTDIR) ? "no such zone in the time zone database" : strerror(errno);
        return false;
    }
    struct stat info;
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode)) {
        close(descriptor);
        reason = "not a time zone file";
        return false;
    }
    if (info.st_size < static_cast<off_t>(kHeaderSize) || info.st_size > static_cast<off_t>(kMaxFileSize)) {
        close(descriptor);
        reason = "not a time zone file";
        return false;
    }
    bytes.resize(static_cast<size_t>(info.st_size));
    size_t total = 0;
    while (total < bytes.size()) {
        const ssize_t count = read(descriptor, bytes.data() + total, bytes.size() - total);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        total += static_cast<size_t>(count);
    }
    close(descriptor);
    if (total != bytes.size()) {
        reason = "the time zone file could not be read";
        return false;
    }
    return true;
}

} // namespace

// --- PosixRule ---

bool PosixRule::parse(const char *text, PosixRule &out) {
    PosixRule rule;
    const char *cursor = text;
    int32_t seconds = 0;
    // POSIX counts offsets west of Greenwich; everything here counts east.
    if (!skipRuleName(cursor) || !readClock(cursor, 24, seconds)) {
        return false;
    }
    rule.standard.utcOffset = -seconds;
    rule.standard.isDst = false;
    if (*cursor == '\0') {
        out = rule;
        return true;
    }

    if (!skipRuleName(cursor)) {
        return false;
    }
    rule.hasDaylight = true;
    rule.daylight.isDst = true;
    rule.daylight.utcOffset = rule.standard.utcOffset + 3600;
    if (*cursor != '\0' && *cursor != ',') {
        if (!readClock(cursor, 24, seconds)) {
            return false;
        }
        rule.daylight.utcOffset = -seconds;
    }
    if (*cursor == ',') {
        ++cursor;
        if (!readRuleDate(cursor, rule.start) || *cursor != ',') {
            return false;
        }
        ++cursor;
        if (!readRuleDate(cursor, rule.end)) {
            return false;
        }
    } else {
        // No dates given: the long-standing default of the reference code.
        const char *defaults = "M3.2.0";
        readRuleDate(defaults, rule.start);
        defaults = "M11.1.0";
        readRuleDate(defaults, rule.end);
    }
    if (*cursor != '\0') {
        return false;
    }
    out = rule;
    return true;
}

void PosixRule::changesInYear(int64_t year, int64_t &toDaylight, int64_t &toStandard) const {
    // The start is given in standard time, the end in daylight time.
    toDaylight = ruleWallSeconds(start, year) - standard.utcOffset;
    toStandard = ruleWallSeconds(end, year) - daylight.utcOffset;
}

LocalType PosixRule::typeAt(int64_t utc) const {
    if (!hasDaylight) {
        return standard;
    }
    int64_t toDaylight = 0;
    int64_t toStandard = 0;
    changesInYear(yearOfSeconds(utc + standard.utcOffset), toDaylight, toStandard);
    bool isDaylight = false;
    if (toDaylight < toStandard) {
        isDaylight = utc >= toDaylight && utc < toStandard;
    } else {
        // Southern hemisphere: daylight time spans the new year.
        isDaylight = !(utc >= toStandard && utc < toDaylight);
    }
    return isDaylight ? daylight : standard;
}

// --- TimeZone ---

bool TimeZone::isValidName(const std::string &name) {
    if (name.empty() || name.size() > kMaxNameLength) {
        return false;
    }
    size_t componentStart = 0;
    for (size_t index = 0; index <= name.size(); ++index) {
        if (index == name.size() || name[index] == '/') {
            const size_t length = index - componentStart;
            // No empty components (leading, trailing or doubled slash), and
            // none that begins with a dot ("." and ".." included).
            if (length == 0 || name[componentStart] == '.') {
                return false;
            }
            componentStart = index + 1;
            continue;
        }
        const char c = name[index];
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                             c == '_' || c == '-' || c == '+' || c == '.';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

bool TimeZone::load(const std::string &name, TimeZone &out, std::string &reason) {
    if (!isValidName(name)) {
        reason = "not a time zone name";
        return false;
    }
    const std::string &directory = zoneDatabaseDirectory();
    std::vector<uint8_t> bytes;
    if (!readWholeFile(joinPath(directory, name), bytes, reason)) {
        const std::string found = findIgnoringCase(directory, name);
        if (found.empty() || !readWholeFile(found, bytes, reason)) {
            return false;
        }
    }
    return parse(bytes.data(), bytes.size(), out, reason);
}

bool TimeZone::parse(const uint8_t *data, size_t size, TimeZone &out, std::string &reason) {
    reason = "not a valid time zone file";
    Header header;
    if (!readHeader(data, size, header)) {
        return false;
    }
    size_t timeSize = 4;
    const uint8_t *body = data + kHeaderSize;
    size_t remaining = size - kHeaderSize;
    if (header.version >= '2') {
        // Version 2 and later repeat the data with 64-bit times after the
        // 32-bit block; only the second block is used.
        const size_t firstBody = bodySize(header, 4);
        if (remaining < firstBody + kHeaderSize) {
            return false;
        }
        const uint8_t *second = body + firstBody;
        if (!readHeader(second, remaining - firstBody, header)) {
            return false;
        }
        timeSize = 8;
        body = second + kHeaderSize;
        remaining -= firstBody + kHeaderSize;
    }
    const size_t needed = bodySize(header, timeSize);
    if (remaining < needed || header.typeCount == 0 || header.typeCount > 256) {
        return false;
    }

    TimeZone zone;
    zone.transitions_.reserve(header.timeCount);
    for (uint32_t index = 0; index < header.timeCount; ++index) {
        const uint8_t *at = body + static_cast<size_t>(index) * timeSize;
        const int64_t instant =
            (timeSize == 8) ? readBig64(at) : static_cast<int64_t>(static_cast<int32_t>(readBig32(at)));
        if (index > 0 && instant <= zone.transitions_.back()) {
            return false;
        }
        zone.transitions_.push_back(instant);
    }
    const uint8_t *indexes = body + static_cast<size_t>(header.timeCount) * timeSize;
    for (uint32_t index = 0; index < header.timeCount; ++index) {
        if (indexes[index] >= header.typeCount) {
            return false;
        }
    }
    zone.typeIndexes_.assign(indexes, indexes + header.timeCount);
    const uint8_t *types = indexes + header.timeCount;
    zone.types_.reserve(header.typeCount);
    for (uint32_t index = 0; index < header.typeCount; ++index) {
        const uint8_t *at = types + static_cast<size_t>(index) * 6;
        LocalType type;
        type.utcOffset = static_cast<int32_t>(readBig32(at));
        type.isDst = at[4] != 0;
        if (type.utcOffset < -kMaxOffset || type.utcOffset > kMaxOffset) {
            return false;
        }
        zone.types_.push_back(type);
    }

    if (timeSize == 8) {
        // The footer: a newline, the POSIX TZ rule for instants after the
        // last transition (possibly empty), and a newline.
        const uint8_t *footer = body + needed;
        const size_t footerSize = remaining - needed;
        if (footerSize >= 2 && footer[0] == '\n') {
            const uint8_t *close = static_cast<const uint8_t *>(memchr(footer + 1, '\n', footerSize - 1));
            if (close != nullptr && close > footer + 1) {
                const std::string text(reinterpret_cast<const char *>(footer + 1), static_cast<size_t>(close - footer - 1));
                if (memchr(text.data(), '\0', text.size()) != nullptr || !PosixRule::parse(text.c_str(), zone.rule_)) {
                    return false;
                }
                zone.hasRule_ = true;
            }
        }
    }

    out = zone;
    reason.clear();
    return true;
}

LocalType TimeZone::typeAt(int64_t utc) const {
    if (transitions_.empty()) {
        return hasRule_ ? rule_.typeAt(utc) : types_[0];
    }
    if (utc < transitions_.front()) {
        // Before the first transition: the first standard time type, else
        // the first type.
        for (const LocalType &type : types_) {
            if (!type.isDst) {
                return type;
            }
        }
        return types_[0];
    }
    if (utc >= transitions_.back()) {
        return hasRule_ ? rule_.typeAt(utc) : types_[typeIndexes_.back()];
    }
    const size_t next = static_cast<size_t>(std::upper_bound(transitions_.begin(), transitions_.end(), utc) -
                                            transitions_.begin());
    return types_[typeIndexes_[next - 1]];
}

LocalType TimeZone::typeForWall(int64_t wall) const {
    // No zone is more than about a day from UTC, so the instant this reading
    // stands for lies well inside two days either side of it.
    const int64_t low = wall - 2 * 86400;
    const int64_t high = wall + 2 * 86400;

    std::vector<int64_t> changes;
    auto first = std::upper_bound(transitions_.begin(), transitions_.end(), low);
    for (auto it = first; it != transitions_.end() && *it <= high; ++it) {
        changes.push_back(*it);
    }
    if (hasRule_ && rule_.hasDaylight) {
        const int64_t year = yearOfSeconds(wall);
        for (int64_t candidate = year - 1; candidate <= year + 1; ++candidate) {
            int64_t pair[2];
            rule_.changesInYear(candidate, pair[0], pair[1]);
            for (int64_t change : pair) {
                const bool afterTable = transitions_.empty() || change > transitions_.back();
                if (afterTable && change > low && change <= high) {
                    changes.push_back(change);
                }
            }
        }
        std::sort(changes.begin(), changes.end());
    }

    // Walk the changes in order. Local clocks reach the new type once they
    // pass the later of the two readings the change joins: the end of a
    // skipped hour, or the end of a repeated one. Until then the earlier
    // type applies, which is the usual "first occurrence" reading.
    LocalType current = typeAt(low);
    for (int64_t change : changes) {
        const LocalType before = typeAt(change - 1);
        const LocalType after = typeAt(change);
        if (wall < change + std::max(before.utcOffset, after.utcOffset)) {
            break;
        }
        current = after;
    }
    return current;
}

} // namespace timemcp

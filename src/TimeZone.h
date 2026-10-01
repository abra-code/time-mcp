// Time zone rules read from the system's zone database: the compiled TZif
// files (RFC 9636) that macOS and Linux both keep under /usr/share/zoneinfo.
// No process-global state (no TZ, no tzset), so a zone is a plain value.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace timemcp {

// One "local time type" of a zone: the offset from UTC in seconds (east is
// positive) and the daylight saving flag as the zone database records it.
struct LocalType {
    int32_t utcOffset = 0;
    bool isDst = false;
};

// A date of the POSIX TZ rule that follows the last recorded transition.
struct RuleDate {
    enum Kind { JulianNoLeap, ZeroBased, MonthWeekDay };
    Kind kind = MonthWeekDay;
    int month = 0;    // 1-12 (MonthWeekDay)
    int week = 0;     // 1-5, 5 = last (MonthWeekDay)
    int day = 0;      // weekday 0-6, or the day number
    int32_t time = 7200; // seconds after local midnight, may be negative or past 24 h
};

struct PosixRule {
    LocalType standard;
    LocalType daylight;
    bool hasDaylight = false;
    RuleDate start;
    RuleDate end;

    static bool parse(const char *text, PosixRule &out);
    LocalType typeAt(int64_t utc) const;
    // The two changes of `year` as UTC instants: into and out of daylight time.
    void changesInYear(int64_t year, int64_t &toDaylight, int64_t &toStandard) const;
};

class TimeZone {
public:
    // True when `name` has the shape of a zone database key: a relative path
    // of plain components, with no way out of the database directory.
    static bool isValidName(const std::string &name);

    // Loads `name` from the system zone database. On failure returns false
    // and sets `reason` to a short explanation.
    static bool load(const std::string &name, TimeZone &out, std::string &reason);

    // Parses the bytes of a TZif file.
    static bool parse(const uint8_t *data, size_t size, TimeZone &out, std::string &reason);

    // The local time type in effect at a UTC instant.
    LocalType typeAt(int64_t utc) const;

    // The local time type for a wall clock reading (`wall` is the local date
    // and time counted in seconds as if it were UTC). A reading that the
    // clocks skip, or show twice, gets the type in effect before the change.
    LocalType typeForWall(int64_t wall) const;

private:
    std::vector<int64_t> transitions_;
    std::vector<uint8_t> typeIndexes_;
    std::vector<LocalType> types_;
    PosixRule rule_;
    bool hasRule_ = false;
};

} // namespace timemcp

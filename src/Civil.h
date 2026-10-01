// Calendar arithmetic on the proleptic Gregorian calendar, with no time zone
// and no locale. Days are counted from 1970-01-01.

#pragma once

#include <cstdint>

namespace timemcp {

inline int64_t daysFromCivil(int64_t year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned dayOfYear = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
}

inline void civilFromDays(int64_t days, int64_t &year, unsigned &month, unsigned &day) {
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned dayOfEra = static_cast<unsigned>(days - era * 146097);
    const unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const unsigned monthIndex = (5 * dayOfYear + 2) / 153;
    day = dayOfYear - (153 * monthIndex + 2) / 5 + 1;
    month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
    year = static_cast<int64_t>(yearOfEra) + era * 400 + (month <= 2);
}

// Floor division, for seconds and days before 1970.
inline int64_t floorDiv(int64_t value, int64_t divisor) {
    int64_t quotient = value / divisor;
    if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) {
        quotient -= 1;
    }
    return quotient;
}

// 0 = Sunday ... 6 = Saturday.
inline unsigned weekdayFromDays(int64_t days) {
    return static_cast<unsigned>(((days + 4) % 7 + 7) % 7);
}

inline bool isLeapYear(int64_t year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

inline unsigned daysInMonth(int64_t year, unsigned month) {
    static const unsigned kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return (month == 2 && isLeapYear(year)) ? 29 : kDays[month - 1];
}

} // namespace timemcp

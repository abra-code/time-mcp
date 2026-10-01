// Test tool. Reads commands on standard input, one per line:
//
//   utc   <zone> <from> <to> <step>   offsets at UTC instants
//   wall  <zone> <from> <to> <step>   offsets for wall clock readings
//   libc  <zone> <from> <to> <step>   compare the UTC lookup with the C library
//
// "utc" and "wall" print "<sample> <offset> <is_dst>" for the first sample
// and for each sample where the answer differs from the one before, then
// "end". "libc" prints one line per differing sample (at most 5), then
// "end <count>". A zone that does not load prints "error <reason>".

#include "TimeZone.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

using namespace timemcp;

int main() {
    char line[1024];
    while (fgets(line, sizeof(line), stdin) != nullptr) {
        char command[16];
        char name[512];
        long long from = 0;
        long long to = 0;
        long long step = 0;
        if (sscanf(line, "%15s %511s %lld %lld %lld", command, name, &from, &to, &step) != 5 || step <= 0) {
            printf("error bad command\n");
            fflush(stdout);
            continue;
        }
        TimeZone zone;
        std::string reason;
        if (!TimeZone::load(name, zone, reason)) {
            printf("error %s\n", reason.c_str());
            fflush(stdout);
            continue;
        }
        if (strcmp(command, "libc") == 0) {
            setenv("TZ", name, 1);
            tzset();
            long long differences = 0;
            for (long long sample = from; sample <= to; sample += step) {
                const LocalType ours = zone.typeAt(sample);
                const time_t instant = static_cast<time_t>(sample);
                struct tm fields;
                localtime_r(&instant, &fields);
                const bool theirDst = fields.tm_isdst > 0;
                if (ours.utcOffset != fields.tm_gmtoff || ours.isDst != theirDst) {
                    if (differences < 5) {
                        printf("%lld ours %d %d libc %ld %d\n", sample, ours.utcOffset, ours.isDst ? 1 : 0,
                               fields.tm_gmtoff, theirDst ? 1 : 0);
                    }
                    ++differences;
                }
            }
            printf("end %lld\n", differences);
        } else {
            const bool wall = strcmp(command, "wall") == 0;
            bool first = true;
            LocalType previous;
            for (long long sample = from; sample <= to; sample += step) {
                const LocalType type = wall ? zone.typeForWall(sample) : zone.typeAt(sample);
                if (first || type.utcOffset != previous.utcOffset || type.isDst != previous.isDst) {
                    printf("%lld %d %d\n", sample, type.utcOffset, type.isDst ? 1 : 0);
                }
                previous = type;
                first = false;
            }
            printf("end\n");
        }
        fflush(stdout);
    }
    return 0;
}

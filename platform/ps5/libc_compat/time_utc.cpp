// gmtime_r / localtime_r for PS5 titles (owner: Agent 2, platform/). Neither
// the title libc module (sce_module/libc.prx) nor libSceLibcInternal exports
// them; spdlog's time formatting (gpu/xenos) references them. Linked by
// platform/ps5/tools/build_title.sh and gpu/vulkan/ps5/build-ps5.sh.
#include <time.h>

// Proleptic Gregorian conversion of a
// time_t (seconds since 1970-01-01 UTC). The title has no time zone database,
// so local time is UTC (tm_gmtoff 0, tm_zone "UTC"). Weak: a library linked
// into the same title that has its own (PS5_Vulkan's support archive defines
// localtime_r) takes precedence.
extern "C" {
__attribute__((weak)) struct tm* gmtime_r(const time_t* t, struct tm* out) {
    if (!t || !out) return nullptr;
    long long s = *t, days = s / 86400, rem = s % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    out->tm_hour = int(rem / 3600);
    out->tm_min = int(rem % 3600 / 60);
    out->tm_sec = int(rem % 60);
    out->tm_wday = int(((days % 7) + 11) % 7);  // 1970-01-01 was a Thursday (4)
    // civil_from_days (H. Hinnant, public domain algorithm).
    long long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int d = int(doy - (153 * mp + 2) / 5 + 1), m = int(mp < 10 ? mp + 3 : mp - 9);
    if (m <= 2) ++y;
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    static const int kCum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    out->tm_year = int(y - 1900);
    out->tm_mon = m - 1;
    out->tm_mday = d;
    out->tm_yday = kCum[m - 1] + d - 1 + (leap && m > 2 ? 1 : 0);
    out->tm_isdst = 0;
    out->tm_gmtoff = 0;
    out->tm_zone = const_cast<char*>("UTC");
    return out;
}
__attribute__((weak)) struct tm* localtime_r(const time_t* t, struct tm* out) { return gmtime_r(t, out); }
}

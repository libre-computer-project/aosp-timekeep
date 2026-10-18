// SPDX-License-Identifier: Apache-2.0
//
// timekeep's decisions, free of side effects so the tests can drive them: which
// record restores the clock, whether a time is plausible, how records are parsed and
// read. main.cpp does the clock, property and logging.

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace timekeep {

// A record is plausible from the vendor image's build time to kSpan after it.
inline constexpr int64_t kSpan = 631152000;  // 20 years
// The floor when the build time cannot be read (2026-01-01). Never 0: a 0 floor
// would make every real time implausible and leave the clock unset.
inline constexpr int64_t kFloor = 1767225600;

// The build time from ro.vendor.build.date.utc's text, never below kFloor.
int64_t BuildTime(const std::string& prop);

// Whether v is a time inside [build, build + kSpan].
bool Plausible(std::optional<int64_t> v, int64_t build);

// Seconds since the epoch from a record's text: digits, optionally followed by
// whitespace. Anything else is no record.
std::optional<int64_t> ParseEpoch(const std::string& text);

enum class Source { kRtc, kVrtc, kSaved };
const char* SourceName(Source s);

struct RestoreInput {
    int64_t now;
    int64_t build;
    std::optional<int64_t> rtc;    // battery-backed RTC (/dev/rtc0), if it gave a time
    std::optional<int64_t> vrtc;   // the vRTC's since_epoch, if there is a vRTC
    std::optional<int64_t> saved;  // the saved record, if it parsed
};

struct RestoreDecision {
    enum class Kind { kAlreadySet, kNoRecord, kSet } kind;
    int64_t value = 0;
    Source source = Source::kSaved;
};

// The clock is restored only while it is unset (implausible). The best plausible
// record wins, in this order: a battery RTC (it ran while the power was off), the
// vRTC's kept time (newer than the saved time: written every 60 s, survives a warm
// reset), the saved time. Either direction.
RestoreDecision DecideRestore(const RestoreInput& in);

// An unset clock is never saved: it would replace a good record with one the next
// restore has to throw away.
bool ShouldSave(int64_t now, int64_t build);

// The vRTC's since_epoch: the first rtc under class_dir (e.g. /sys/class/rtc) whose
// name reads "meson-vrtc" or "meson-vrtc <device>".
std::optional<int64_t> ReadVrtc(const std::string& class_dir);

// The saved record in file, if it parses.
std::optional<int64_t> ReadSaved(const std::string& file);

// Writes now to file through file.tmp + rename, holding an exclusive flock on the
// directory so two writers (the save loop and the shutdown save) never share the
// temp file. False on any error.
bool WriteSaved(const std::string& file, int64_t now);

// Milliseconds to wait before the next save so saves stay at least min_gap_ms
// apart (a process setting the clock in a loop costs one write per gap, not one per
// set); 0 means save now. last_ms is the previous save's monotonic time, if any.
int64_t SaveDelayMs(int64_t now_ms, std::optional<int64_t> last_ms, int64_t min_gap_ms);

// The time of an RTC device (RTC_RD_TIME), if it gives one: none when the device
// is absent or refuses the read (never set, battery lost).
std::optional<int64_t> ReadRtcDevice(const std::string& dev);

}  // namespace timekeep

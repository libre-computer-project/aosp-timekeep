// SPDX-License-Identifier: Apache-2.0
//
// timekeep -- start the clock as close to real time as the board can know without
// a network.
//
//   timekeep restore    once /data is mounted, if the clock is unset (before the
//                       vendor image's build time): set it from the best record -- a
//                       battery-backed RTC (/dev/rtc0) when one is fitted and gives a
//                       plausible time, else the vRTC's kept time, else the saved time
//   timekeep save       write the current time
//   timekeep save-loop  save every 60 s, and at once whenever the clock is set
//                       (saves at least 1 s apart)
//
// Network time (Android's time detector) corrects whatever this leaves, ahead or
// behind, and writes /dev/rtc0 itself. This never touches the network.

#define LOG_TAG "timekeep"

#include <android-base/properties.h>
#include <errno.h>
#include <log/log.h>
#include <poll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cstring>
#include <ctime>
#include <optional>
#include <string>

#include "timekeep_core.h"

namespace {

constexpr char kSaved[] = "/data/vendor/timekeep/epoch";
constexpr char kRtcDevice[] = "/dev/rtc0";
constexpr char kRtcClass[] = "/sys/class/rtc";
constexpr time_t kSaveInterval = 60;
constexpr int64_t kMinSaveGapMs = 1000;

int64_t Now() {
    return static_cast<int64_t>(time(nullptr));
}

int64_t MonoMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

int64_t Build() {
    return timekeep::BuildTime(android::base::GetProperty("ro.vendor.build.date.utc", ""));
}

int Restore() {
    timekeep::RestoreInput in;
    in.now = Now();
    in.build = Build();
    in.rtc = timekeep::ReadRtcDevice(kRtcDevice);
    in.vrtc = timekeep::ReadVrtc(kRtcClass);
    in.saved = timekeep::ReadSaved(kSaved);
    const timekeep::RestoreDecision d = timekeep::DecideRestore(in);
    switch (d.kind) {
        case timekeep::RestoreDecision::Kind::kAlreadySet:
            ALOGI("clock already set (%lld); restore skipped", static_cast<long long>(in.now));
            return 0;
        case timekeep::RestoreDecision::Kind::kNoRecord:
            ALOGI("no plausible record; clock left at %lld", static_cast<long long>(in.now));
            return 0;
        case timekeep::RestoreDecision::Kind::kSet:
            break;
    }
    const timespec ts = {static_cast<time_t>(d.value), 0};
    if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
        ALOGE("clock_settime %lld: %s", static_cast<long long>(d.value), strerror(errno));
        return 1;
    }
    ALOGI("clock set from %lld to %s %lld", static_cast<long long>(in.now),
          timekeep::SourceName(d.source), static_cast<long long>(d.value));
    return 0;
}

int Save(int64_t build) {
    const int64_t now = Now();
    if (!timekeep::ShouldSave(now, build)) return 0;
    if (!timekeep::WriteSaved(kSaved, now)) {
        ALOGE("save %s: %s", kSaved, strerror(errno));
        return 1;
    }
    return 0;
}

// A realtime timer far in the future, cancelled (the read fails with ECANCELED)
// whenever the clock is set: the cue to save the new time at once.
bool ArmClockSetWatch(int fd) {
    itimerspec it = {};
    it.it_value.tv_sec = time(nullptr) + 10 * 365 * 86400;
    return timerfd_settime(fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &it, nullptr) == 0;
}

int SaveLoop() {
    const int64_t build = Build();
    const int tick = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    const int set = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC);
    if (tick < 0 || set < 0) {
        ALOGE("timerfd_create: %s", strerror(errno));
        return 1;
    }
    itimerspec every = {};
    every.it_interval.tv_sec = kSaveInterval;
    every.it_value.tv_sec = kSaveInterval;
    if (timerfd_settime(tick, 0, &every, nullptr) != 0 || !ArmClockSetWatch(set)) {
        ALOGE("timerfd_settime: %s", strerror(errno));
        return 1;
    }
    pollfd fds[2] = {{tick, POLLIN, 0}, {set, POLLIN, 0}};
    std::optional<int64_t> last_save;
    bool pending = true;  // the first save, at start
    for (;;) {
        // A save due inside kMinSaveGapMs of the last one waits out the gap (poll's
        // timeout), so a burst of clock sets costs one write and the last set is kept.
        int timeout = -1;
        if (pending) {
            const int64_t wait = timekeep::SaveDelayMs(MonoMs(), last_save, kMinSaveGapMs);
            if (wait == 0) {
                Save(build);
                last_save = MonoMs();
                pending = false;
            } else {
                timeout = static_cast<int>(wait);
            }
        }
        if (poll(fds, 2, timeout) < 0) {
            if (errno == EINTR) continue;
            ALOGE("poll: %s", strerror(errno));
            return 1;
        }
        uint64_t n;
        if (fds[0].revents & POLLIN) {
            (void)read(tick, &n, sizeof(n));
            pending = true;
        }
        if (fds[1].revents & POLLIN) {
            if (read(set, &n, sizeof(n)) < 0 && errno == ECANCELED) ArmClockSetWatch(set);
            pending = true;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string verb = argc > 1 ? argv[1] : "";
    if (verb == "restore") return Restore();
    if (verb == "save") return Save(Build());
    if (verb == "save-loop") return SaveLoop();
    fprintf(stderr, "usage: timekeep restore|save|save-loop\n");
    return 2;
}

// SPDX-License-Identifier: Apache-2.0

#include "timekeep_core.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/rtc.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cctype>
#include <ctime>
#include <fstream>
#include <sstream>

namespace timekeep {

namespace {

std::optional<std::string> ReadFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

std::optional<int64_t> ParseEpoch(const std::string& text) {
    size_t i = 0;
    int64_t v = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
        if (v > (INT64_MAX - 9) / 10) return std::nullopt;  // overflow: not a time
        v = v * 10 + (text[i] - '0');
        ++i;
    }
    if (i == 0) return std::nullopt;
    for (; i < text.size(); ++i) {
        if (!std::isspace(static_cast<unsigned char>(text[i]))) return std::nullopt;
    }
    return v;
}

int64_t BuildTime(const std::string& prop) {
    std::optional<int64_t> b = ParseEpoch(prop);
    if (!b || *b < kFloor) return kFloor;
    return *b;
}

bool Plausible(std::optional<int64_t> v, int64_t build) {
    return v && *v >= build && *v <= build + kSpan;
}

const char* SourceName(Source s) {
    switch (s) {
        case Source::kRtc:
            return "rtc";
        case Source::kVrtc:
            return "vrtc";
        case Source::kSaved:
            return "saved";
    }
    return "?";
}

RestoreDecision DecideRestore(const RestoreInput& in) {
    if (Plausible(in.now, in.build)) return {RestoreDecision::Kind::kAlreadySet};
    if (Plausible(in.rtc, in.build)) return {RestoreDecision::Kind::kSet, *in.rtc, Source::kRtc};
    if (Plausible(in.vrtc, in.build)) {
        return {RestoreDecision::Kind::kSet, *in.vrtc, Source::kVrtc};
    }
    if (Plausible(in.saved, in.build)) {
        return {RestoreDecision::Kind::kSet, *in.saved, Source::kSaved};
    }
    return {RestoreDecision::Kind::kNoRecord};
}

bool ShouldSave(int64_t now, int64_t build) {
    return Plausible(now, build);
}

std::optional<int64_t> ReadVrtc(const std::string& class_dir) {
    DIR* d = opendir(class_dir.c_str());
    if (!d) return std::nullopt;
    std::optional<int64_t> kept;
    while (dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name.rfind("rtc", 0) != 0) continue;
        const std::string dir = class_dir + "/" + name;
        std::optional<std::string> n = ReadFile(dir + "/name");
        if (!n) continue;
        // name reads "<driver> <device>", e.g. "meson-vrtc c81000a8.rtc"
        const std::string word = n->substr(0, n->find_first_of(" \n"));
        if (word != "meson-vrtc") continue;
        std::optional<std::string> s = ReadFile(dir + "/since_epoch");
        if (s) kept = ParseEpoch(*s);
        break;
    }
    closedir(d);
    return kept;
}

std::optional<int64_t> ReadSaved(const std::string& file) {
    std::optional<std::string> s = ReadFile(file);
    if (!s) return std::nullopt;
    return ParseEpoch(*s);
}

bool WriteSaved(const std::string& file, int64_t now) {
    const std::string tmp = file + ".tmp";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
    if (fd < 0) return false;
    const std::string text = std::to_string(now) + "\n";
    const bool ok = write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    close(fd);
    return ok && rename(tmp.c_str(), file.c_str()) == 0;
}

std::optional<int64_t> ReadRtcDevice(const std::string& dev) {
    const int fd = open(dev.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    rtc_time rt = {};
    const int r = ioctl(fd, RTC_RD_TIME, &rt);
    close(fd);
    if (r != 0) return std::nullopt;
    tm t = {};
    t.tm_sec = rt.tm_sec;
    t.tm_min = rt.tm_min;
    t.tm_hour = rt.tm_hour;
    t.tm_mday = rt.tm_mday;
    t.tm_mon = rt.tm_mon;
    t.tm_year = rt.tm_year;
    const time_t v = timegm(&t);
    if (v == static_cast<time_t>(-1)) return std::nullopt;
    return static_cast<int64_t>(v);
}

}  // namespace timekeep

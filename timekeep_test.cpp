// SPDX-License-Identifier: Apache-2.0
//
// timekeep_core cases. Runs on the host and on the device (atest timekeep_test, or
// push the device binary and run it).

#include "timekeep_core.h"

#include <android-base/file.h>
#include <gtest/gtest.h>
#include <sys/stat.h>

#include <string>

namespace timekeep {
namespace {

constexpr int64_t kBuild = 1790000000;
constexpr int64_t kN = 1792273000;

RestoreInput In(int64_t now, std::optional<int64_t> saved, std::optional<int64_t> vrtc,
                std::optional<int64_t> rtc = std::nullopt, int64_t build = kBuild) {
    return {now, build, rtc, vrtc, saved};
}

void ExpectSet(const RestoreDecision& d, int64_t value, Source source) {
    EXPECT_EQ(d.kind, RestoreDecision::Kind::kSet);
    EXPECT_EQ(d.value, value);
    EXPECT_EQ(d.source, source);
}

TEST(Restore, KeptBeatsALaterSaved) {
    ExpectSet(DecideRestore(In(14, kN + 5000, kN)), kN, Source::kVrtc);
}
TEST(Restore, KeptBeatsAnEarlierSaved) {
    ExpectSet(DecideRestore(In(14, kN - 5000, kN)), kN, Source::kVrtc);
}
TEST(Restore, SavedWhenTheVrtcHasNoKeptTime) {
    // No kept record: the vRTC reads the (unset) system time.
    ExpectSet(DecideRestore(In(14, kN - 5000, 14)), kN - 5000, Source::kSaved);
}
TEST(Restore, SavedWhenThereIsNoVrtc) {
    ExpectSet(DecideRestore(In(14, kN, std::nullopt)), kN, Source::kSaved);
}
TEST(Restore, SavedAheadIsUsedBackwardIsFine) {
    ExpectSet(DecideRestore(In(14, kN + 86400, std::nullopt)), kN + 86400, Source::kSaved);
}
TEST(Restore, ImplausibleFutureSavedIgnored) {
    EXPECT_EQ(DecideRestore(In(14, 4102444800, std::nullopt)).kind,
              RestoreDecision::Kind::kNoRecord);
}
TEST(Restore, PreBuildSavedIgnored) {
    EXPECT_EQ(DecideRestore(In(14, 1700000000, std::nullopt)).kind,
              RestoreDecision::Kind::kNoRecord);
}
TEST(Restore, NoSavedRecord) {
    EXPECT_EQ(DecideRestore(In(14, std::nullopt, std::nullopt)).kind,
              RestoreDecision::Kind::kNoRecord);
}
TEST(Restore, ImplausibleKeptFallsToSaved) {
    ExpectSet(DecideRestore(In(14, kN, 4102444800)), kN, Source::kSaved);
}
TEST(Restore, ClockAlreadySetSkips) {
    EXPECT_EQ(DecideRestore(In(kN, kN + 999, kN + 999)).kind,
              RestoreDecision::Kind::kAlreadySet);
}
TEST(Restore, Post2038KeptIsPlausible) {
    ExpectSet(DecideRestore(In(14, std::nullopt, 2200000000)), 2200000000, Source::kVrtc);
}
TEST(Restore, UnreadableBuildTimeUsesTheFloorNotZero) {
    ExpectSet(DecideRestore(In(14, kN, std::nullopt, std::nullopt, BuildTime(""))), kN,
              Source::kSaved);
}
TEST(Restore, RtcBeatsKeptAndSaved) {
    ExpectSet(DecideRestore(In(14, kN + 5000, kN, 1792278600)), 1792278600, Source::kRtc);
}
TEST(Restore, RtcThatGaveNoTimeFallsToKept) {
    ExpectSet(DecideRestore(In(14, kN + 5000, kN, std::nullopt)), kN, Source::kVrtc);
}
TEST(Restore, RtcImplausibleBatteryLostFallsToSaved) {
    ExpectSet(DecideRestore(In(14, kN - 5000, std::nullopt, 946684800)), kN - 5000,
              Source::kSaved);
}
TEST(Restore, ClockAlreadySetByHctosysSkips) {
    EXPECT_EQ(DecideRestore(In(kN, kN + 5000, kN, 1792278600)).kind,
              RestoreDecision::Kind::kAlreadySet);
}

TEST(Save, SkipsAnUnsetClock) {
    EXPECT_FALSE(ShouldSave(17, kBuild));
}
TEST(Save, TakesAnEarlierClock) {
    EXPECT_TRUE(ShouldSave(kN - 3600, kBuild));
}
TEST(Save, TakesALaterClock) {
    EXPECT_TRUE(ShouldSave(kN + 60, kBuild));
}

TEST(Parse, Epoch) {
    EXPECT_EQ(ParseEpoch("1792273000"), 1792273000);
    EXPECT_EQ(ParseEpoch("1792273000\n"), 1792273000);
    EXPECT_EQ(ParseEpoch("2200000000"), 2200000000);  // past 2^31
    EXPECT_EQ(ParseEpoch(""), std::nullopt);
    EXPECT_EQ(ParseEpoch("junk"), std::nullopt);
    EXPECT_EQ(ParseEpoch("17x"), std::nullopt);
    EXPECT_EQ(ParseEpoch("-5"), std::nullopt);
    EXPECT_EQ(ParseEpoch("99999999999999999999999"), std::nullopt);
}
TEST(Parse, BuildTime) {
    EXPECT_EQ(BuildTime("1792186758"), 1792186758);
    EXPECT_EQ(BuildTime(""), kFloor);
    EXPECT_EQ(BuildTime("junk"), kFloor);
    EXPECT_EQ(BuildTime("1700000000"), kFloor);  // never below the floor
}

class Files : public ::testing::Test {
  protected:
    TemporaryDir dir;
    std::string Path(const std::string& p) { return std::string(dir.path) + "/" + p; }
    void Put(const std::string& p, const std::string& text) {
        ASSERT_TRUE(android::base::WriteStringToFile(text, Path(p)));
    }
};

TEST_F(Files, VrtcFoundByItsDriverWord) {
    mkdir(Path("rtc0").c_str(), 0755);
    mkdir(Path("rtc1").c_str(), 0755);
    Put("rtc0/name", "rtc-ds1307 1-0068\n");
    Put("rtc0/since_epoch", "1111111111\n");
    Put("rtc1/name", "meson-vrtc c81000a8.rtc\n");
    Put("rtc1/since_epoch", "1792273000\n");
    EXPECT_EQ(ReadVrtc(dir.path), 1792273000);
}
TEST_F(Files, NoVrtc) {
    mkdir(Path("rtc0").c_str(), 0755);
    Put("rtc0/name", "rtc-ds1307 1-0068\n");
    Put("rtc0/since_epoch", "1111111111\n");
    EXPECT_EQ(ReadVrtc(dir.path), std::nullopt);
    EXPECT_EQ(ReadVrtc(Path("absent")), std::nullopt);
}
TEST_F(Files, SavedRoundTrip) {
    ASSERT_TRUE(WriteSaved(Path("epoch"), 1792273000));
    EXPECT_EQ(ReadSaved(Path("epoch")), 1792273000);
    std::string text;
    ASSERT_TRUE(android::base::ReadFileToString(Path("epoch"), &text));
    EXPECT_EQ(text, "1792273000\n");
    struct stat st;
    EXPECT_NE(stat(Path("epoch.tmp").c_str(), &st), 0);  // renamed into place
}
TEST_F(Files, SavedJunkOrAbsent) {
    Put("epoch", "junk\n");
    EXPECT_EQ(ReadSaved(Path("epoch")), std::nullopt);
    EXPECT_EQ(ReadSaved(Path("absent")), std::nullopt);
}
TEST_F(Files, SaveFailsInAMissingDir) {
    EXPECT_FALSE(WriteSaved(Path("absent/epoch"), 1792273000));
}
TEST_F(Files, RtcDeviceAbsentOrNotAnRtc) {
    EXPECT_EQ(ReadRtcDevice(Path("rtc0")), std::nullopt);
    Put("rtc0", "not a device\n");
    EXPECT_EQ(ReadRtcDevice(Path("rtc0")), std::nullopt);  // the ioctl fails
}

}  // namespace
}  // namespace timekeep

# aosp-timekeep

`timekeep` starts an Android board's clock as close to real time as it can know
without a network, for boards with no battery-backed RTC fitted (and uses one when it
is). Android's network time (NTP) corrects whatever it leaves, in either direction.
It never touches the network.

## What it does

| Verb | When | Effect |
|---|---|---|
| `restore` | once, at `post-fs-data` | if the clock is unset (before the vendor image's build time), set it from the best plausible record: a battery-backed RTC on `/dev/rtc0`, else the Amlogic vRTC's kept time (`rtc-meson-vrtc` with `amlogic,keep-sram`; survives a warm reset), else the saved time |
| `save-loop` | `class main` | save the clock to `/data/vendor/timekeep/epoch` every 60 s, and at once whenever the clock is set (network or Settings time) |
| `save` | `on shutdown` | one last save |

A record is plausible from the build time to 20 years after it; anything else (a
lost RTC battery, a corrupt file) is skipped. Only the restore holds `SYS_TIME`.
Android's time detector writes `/dev/rtc0` itself on every network or Settings time.

One native binary (`main.cpp`; the decisions in `timekeep_core.cpp`): the save loop
is a single process that sleeps on a monotonic timer, runs nothing else and does
not wake a suspended device. Its SELinux domain has no network access.

## Using it in a board

```make
# device.mk
PRODUCT_PACKAGES += timekeep
# BoardConfig.mk
BOARD_VENDOR_SEPOLICY_DIRS += vendor/libre-computer/timekeep/sepolicy
```

and label the vRTC's sysfs device directory `sysfs_rtc` in the board's
`genfs_contexts` (see `sepolicy/timekeep.te`).

## Test

`timekeep_test` (gtest) covers the restore choice in both directions, the
plausibility window (including times past 2038), parsing, and the file and sysfs
readers against a temporary directory. Run it on the host and on the device:

```sh
m timekeep_test
$ANDROID_HOST_OUT/nativetest64/timekeep_test/timekeep_test
adb push $OUT/data/nativetest64/timekeep_test/timekeep_test /data/local/tmp/
adb shell /data/local/tmp/timekeep_test
```

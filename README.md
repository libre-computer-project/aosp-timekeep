# aosp-timekeep

`lc-timekeep` starts an Android board's clock as close to real time as it can know
without a network, for boards with no battery-backed RTC fitted (and uses one when it
is). Android's network time (NTP) corrects whatever it leaves, in either direction.
It never touches the network.

## What it does

| Verb | When | Effect |
|---|---|---|
| `restore` | once, at `post-fs-data` | if the clock is unset (before the vendor image's build time), set it from the best plausible record: a battery-backed RTC on `/dev/rtc0`, else the Amlogic vRTC's kept time (`rtc-meson-vrtc` with `amlogic,keep-sram`; survives a warm reset), else the saved time |
| `save-loop` | `class main` | save the clock to `/data/vendor/timekeep/epoch` every 60 s |
| `save` | `on shutdown` | one last save |

A record is plausible from the build time to 20 years after it; anything else (a
lost RTC battery, a corrupt file) is skipped. Only the restore holds `SYS_TIME`.
Android's time detector writes `/dev/rtc0` itself on every network or Settings time.

## Using it in a board

```make
# device.mk
PRODUCT_PACKAGES += lc-timekeep
# BoardConfig.mk
BOARD_VENDOR_SEPOLICY_DIRS += vendor/libre-computer/timekeep/sepolicy
```

and label the vRTC's sysfs device directory `sysfs_rtc` in the board's
`genfs_contexts` (see `sepolicy/lc_timekeep.te`).

## Test

```sh
./test-lc-timekeep.sh                      # cases on this host's /bin/sh
./test-lc-timekeep.sh --adb "adb -s SERIAL" # the same cases on the device's shell
```

Every I/O is mocked. Run the `--adb` form before landing a change: Android's shell
does 32-bit arithmetic, which a host shell does not. The test also fails if the
script gains a network call.

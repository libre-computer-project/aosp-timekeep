#!/bin/bash
# test-timekeep.sh -- restore/save cases for timekeep with every I/O mocked
# (battery RTC, rtc sysfs, build time, clock, date -s, log).
#
#   test-timekeep.sh [--adb "<adb command>"] [SCRIPT]
#
# Without --adb the cases run on this host's /bin/sh. With --adb (e.g. --adb adb, or
# --adb "sudo adb -s SERIAL") they run on the device under su with /system/bin/sh --
# the real shell matters: Android's shell does 32-bit arithmetic, which a host shell
# does not. SCRIPT defaults to the timekeep beside this file (pass an older
# revision to see the cases fail).
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ADB=""
if [ "${1:-}" = --adb ]; then
	[ "$#" -ge 2 ] || { echo "--adb needs the adb command" >&2; exit 2; }
	ADB=$2
	shift 2
fi
SRC=${1:-$HERE/timekeep}
[ -r "$SRC" ] || { echo "no script: $SRC" >&2; exit 2; }

# timekeep never touches the network (operator decision 2026-10-18: no HTTP time).
if grep -nE '(^|[^a-z_])(nc|wget|curl)[[:space:]]|https?://|HTTP' "$SRC"; then
	echo "FAIL network access in $SRC" >&2
	exit 1
fi

if [ -n "$ADB" ]; then D=/data/local/tmp/timekeep-test; SH=/system/bin/sh
else D=$(mktemp -d); SH=/bin/sh; fi
OUT=$(mktemp -d)

# The host has GNU date, which has no -D: there the RTC's time is parsed with -d. On
# the device the script's own toybox `date -D` runs.
HOSTDATE=""
[ -z "$ADB" ] && HOSTDATE='s#date -u -D "[^"]*" -d#date -u -d#'
sed -e "1s#.*#\#!$SH#" -e "s#^F=.*#F=$D/epoch#" \
	-e "s#^RTC_DEV=.*#RTC_DEV=$D/rtc0#" \
	-e "s#hwclock -r -u -f \"\$RTC_DEV\"#grep -v FAIL \"\$RTC_DEV\"#" \
	-e "s#/sys/class/rtc/rtc\*#$D/rtc/rtc*#" \
	-e "s#\$(getprop ro.vendor.build.date.utc)#\$(cat $D/build)#" \
	-e "s#\$(date +%s)#\$(cat $D/now)#" \
	-e "s#date -u -s \"@\$\([a-z]*\)\" > /dev/null#echo SET=\$\1#" \
	-e "${HOSTDATE:-s#^\$##}" \
	-e "s#log -t timekeep#echo LOG#" "$SRC" > "$OUT/tk"
# The mocks must all have applied, or the cases test the real clock.
for m in "cat $D/build" "cat $D/now" "echo SET=" "echo LOG" "$D/rtc/rtc"; do
	grep -qF "$m" "$OUT/tk" || { echo "mock not applied: $m" >&2; exit 2; }
done
HAS_RTC=0
grep -q rtc_read "$SRC" && HAS_RTC=1
if [ "$HAS_RTC" = 1 ]; then
	grep -qF "RTC_DEV=$D/rtc0" "$OUT/tk" || { echo "mock not applied: RTC_DEV" >&2; exit 2; }
	grep -q 'hwclock' "$OUT/tk" && { echo "real hwclock left in" >&2; exit 2; }
fi

cat > "$OUT/run.sh" <<EOF_RUN
D=$D
TK="$SH \$D/tk"
HAS_RTC=$HAS_RTC
EOF_RUN
cat >> "$OUT/run.sh" <<'EOF_RUN'
mkdir -p $D/rtc/rtc1
echo 1790000000 > $D/build
pass=0; fail=0
t() {
	echo "$2" > $D/now
	if [ "$3" = - ]; then rm -f $D/epoch; else echo "$3" > $D/epoch; fi
	if [ "$4" = - ]; then rm -f $D/rtc/rtc1/name
	else echo "meson-vrtc c81000a8.rtc" > $D/rtc/rtc1/name; echo "$4" > $D/rtc/rtc1/since_epoch; fi
	out=$($TK restore | tr '\n' ' ')
	case "$out" in
	*"$5"*) pass=$((pass+1)); echo "ok   $1" ;;
	*) fail=$((fail+1)); echo "FAIL $1: want '$5' got '$out'" ;;
	esac
}
s() {
	echo "$2" > $D/now; echo "$3" > $D/epoch; $TK save
	if [ "$(cat $D/epoch)" = "$4" ]; then pass=$((pass+1)); echo "ok   $1"
	else fail=$((fail+1)); echo "FAIL $1: epoch $(cat $D/epoch) want $4"; fi
}
# Literals, not $(( )): the runner's own shell may be 32-bit too.
N=1792273000; NP5=1792278000; NM5=1792268000; NP1D=1792359400
NM1H=1792269400; NP60=1792273060; NP999=1792273999
t "kept beats a later (ahead) saved"  14 $NP5 $N "SET=$N "
t "kept beats an earlier saved"       14 $NM5 $N "SET=$N "
t "saved when no kept record"         14 $NM5 14 "SET=$NM5 "
t "saved when no vrtc at all"         14 $N - "SET=$N "
t "saved ahead is used (backward ok)" 14 $NP1D - "SET=$NP1D "
t "implausible future saved ignored"  14 4102444800 - "no plausible record"
t "pre-build saved ignored"           14 1700000000 - "no plausible record"
t "junk saved ignored"                14 junk - "no plausible record"
t "implausible kept falls to saved"   14 $N 4102444800 "SET=$N "
t "clock already set: skip"           $N $NP999 $NP999 "restore skipped"
t "post-2038 kept is plausible"       14 - 2200000000 "SET=2200000000 "
: > $D/build
t "build unreadable: floor, not 0"    14 $N - "SET=$N "
echo 1790000000 > $D/build
s "save skips unset clock"      17 $N $N
s "save takes an earlier clock" $NM1H $N $NM1H
s "save takes a later clock"    $NP60 $N $NP60
if [ "$HAS_RTC" = 1 ]; then
	# RTC fixture: hwclock -r's output ("2026-10-17 23:10:00+0000" = 1792278600), or FAIL.
	RT="2026-10-17 23:10:00+0000"
	echo "$RT" > $D/rtc0
	t "rtc beats kept and saved"           14 $NP5 $N "SET=1792278600 "
	t "rtc logged as the source"           14 $NP5 $N "to rtc 1792278600"
	echo FAIL > $D/rtc0
	t "rtc read refused -> kept"           14 $NP5 $N "SET=$N "
	echo "2000-01-01 00:00:00+0000" > $D/rtc0
	t "rtc implausible (lost) -> saved"    14 $NM5 - "SET=$NM5 "
	echo "$RT" > $D/rtc0
	t "clock already set (HCTOSYS): skip"  $N $NP5 $N "restore skipped"
	rm -f $D/rtc0
	t "no rtc fitted -> kept"              14 $NP5 $N "SET=$N "
fi
echo "PASS=$pass FAIL=$fail"
[ "$fail" = 0 ]
EOF_RUN

if [ -n "$ADB" ]; then
	$ADB shell "rm -rf $D; mkdir -p $D" || exit 2
	$ADB push "$OUT/tk" "$OUT/run.sh" "$D/" > /dev/null || exit 2
	$ADB shell "su 0 $SH $D/run.sh; echo RC=\$?" | tr -d '\r' > "$OUT/result"
	$ADB shell "rm -rf $D" > /dev/null
else
	cp "$OUT/tk" "$D/tk"
	{ $SH "$OUT/run.sh"; echo RC=$?; } > "$OUT/result"
	rm -rf "$D"
fi
grep -v '^RC=' "$OUT/result"
rc=$(sed -n 's/^RC=//p' "$OUT/result")
rm -rf "$OUT"
[ "$rc" = 0 ]

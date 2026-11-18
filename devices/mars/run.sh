#!/bin/sh
# The one command line for this device, and a guard that refuses to start when the pieces it
# depends on are not in the images.  Each of these three switches has been lost once already:
#
#   adsp=1   the battery comes only from the audio DSP's battery manager.  Without it the battery
#            reads empty (no serial, no charge percentage).
#   gpu=1    the kernel brings the GPU up.  userland/build.sh only installs the GL libraries into
#            the root inside a kernel that did that, so without gpu=1 the next switch does nothing.
#   gl=1     CoreAnimation renders through Mesa on the Adreno instead of the software renderer.
#            Without it every material, blur and widget surface is painted flat white.
#
# And the GL libraries have to be inside root.apfs: `GL=1 userland/build.sh`.  That is easy to
# forget after a rebuild, so this script looks inside the image as well.
#
#   ./devices/mars/run.sh --check    verify only
#   ./devices/mars/run.sh [seconds]  verify, then run (default 300 s)
set -u
cd "$(dirname "$0")/../.." || exit 1
export DEVICE=mars

BASE="cpus=8 rd=md0 touchscreen=1 adsp=1 gpu=1 gl=1 adreno_nomeasure=1 cpufreq=1 sb_delay=600"
SECS=${1:-300}
CHECK=no
[ "${1:-}" = "--check" ] && { CHECK=yes; SECS=300; }

fail() { echo "mars run: $*" >&2; exit 1; }

GL=$(grep -ac 'libOSMesa' userland/root.apfs 2>/dev/null || true)
[ "${GL:-0}" -gt 0 ] || fail "root.apfs has no libOSMesa: rebuild with GL=1 userland/build.sh"
[ -f userland/root/lib/libhakios.gl.dylib ] || fail "userland/root/lib/libhakios.gl.dylib is missing"

case "$BASE" in *adsp=1*) ;; *) fail "adsp=1 is missing: the battery would read empty" ;; esac
case "$BASE" in *gpu=1*)  ;; *) fail "gpu=1 is missing: the GL libraries would not load" ;; esac
case "$BASE" in *gl=1*)   ;; *) fail "gl=1 is missing: the UI would be painted by the CPU" ;; esac
[ "${#BASE}" -le 470 ] || fail "the line is ${#BASE} characters; the buffer is 607 and the loader takes about 130"

echo "mars run: ${#BASE} characters, the GL libraries are in the image ($GL references)"
echo "mars run: $BASE"
[ "$CHECK" = yes ] && { echo "mars run: verified, not starting"; exit 0; }

pkill -9 -f 'x[l] run' 2>/dev/null
sleep 1
./xl state | tail -1
./xl reload 2>&1 | tail -1
./xl enter 2>&1 | tail -1
./xl ramdisk userland/system.apfs 2>&1 | tail -2
XL_FOLLOW= XL_SECS="$SECS" XL_CMDLINE="$BASE" ./xl run 2>&1 | tail -3

#!/bin/bash
# Fetch exactly what the loader says it has, and prove the log is this run's.
set -e
cd /opt/hackios/hackios
export DEVICE=mars
if [ "$1" != "noflash" ]; then
  ./xl reload 2>&1 | tail -1
  ./xl ramdisk userland/system.apfs 2>&1 | tail -1
  xnuloader/mkpayload.py -o /tmp/usb-payload.bin \
    kernel=hakios-kernel ramdisk=userland/root.apfs \
    --cmdline "debug=0x8 cpus=8 rd=md0 touchscreen=1 buttons=1 logwords=button loader_return_after=150 loader_wait=600" >/dev/null
  fastboot stage /tmp/usb-payload.bin >/dev/null 2>&1
  fastboot oem run >/dev/null 2>&1
fi
len=0
for i in $(seq 1 150); do
  r=$(timeout 8 fastboot getvar result 2>/dev/null | tr -d '\r' | tr '\n' ' ')
  case "$r" in
    *returned*)
      len=$(echo "$r" | sed -n 's/.*returned[^0-9]*\([0-9][0-9]*\).*/\1/p' | head -1)
      echo "result: $r"
      break;;
  esac
  sleep 2
done
echo "loader says $len bytes"
: > /tmp/klog.bin
off=0
while [ "$off" -lt "$len" ]; do
  hexoff=$(printf '%x' $off)
  rm -f /tmp/k.part
  timeout 20 fastboot oem log "$hexoff" >/dev/null 2>&1 || true
  timeout 30 fastboot get_staged /tmp/k.part >/dev/null 2>&1 || true
  n=$(stat -c %s /tmp/k.part 2>/dev/null || echo 0)
  [ "$n" -gt 0 ] || break
  want=$((len - off)); [ "$n" -le "$want" ] || n=$want
  head -c "$n" /tmp/k.part >> /tmp/klog.bin
  off=$((off + n))
done
tr -d '\0' < /tmp/klog.bin > /tmp/k.txt
echo "text $(stat -c %s /tmp/k.txt) bytes of $len"
echo "== this run?"
grep -a -o 'hakios_kver=[^ ]*' /tmp/k.txt | head -1
echo "== last stamped line"
grep -a -o '^\[ *[0-9][0-9]*\.[0-9]*\]' /tmp/k.txt | tail -1
echo "== key summary"
grep -a -o 'buttons: so far[^;]*' /tmp/k.txt | tail -1
echo "== done"
timeout 10 fastboot oem done >/dev/null 2>&1 || true

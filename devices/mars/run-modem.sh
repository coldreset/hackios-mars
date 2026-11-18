#!/bin/bash
# One modem round with a command line short enough that nothing is cut: the addresses come first.
set -u
cd /opt/hackios/hackios || exit 1
export DEVICE=mars

spec="cpus=8 rd=md0 touchscreen=1 soc_smem=0x80900000 soc_mpss_mem=0x8f000000 soc_mpss_size=0x10000000"
spec="$spec modem=1 modem_step=8 modem_buffer=0xd6a00000 modem_no_nav=1 modem_quiet_reads=1"
spec="$spec modem_ipa_quiet=1 modem_ipa_skip_qmi=1 modem_ipa_clk_off=1 modem_ipa_da=1 modem_ipa_mem_off=1"
spec="$spec adsp=1 gpu=1 gl=1 adreno_nomeasure=1 cpufreq=1 fb_hz=120 fb_ca_hz=240 sb_delay=600"
echo "=== the command line, ${#spec} characters (the buffer is 607 and the loader takes about 130) ==="
echo "$spec"

pkill -9 -f 'x[l] run' >/dev/null 2>&1
sleep 1
./xl state | tail -1
XL_CMDLINE="$spec" XL_SECS=150 timeout 900 ./xl run 2>&1 | tail -2
./xl state | tail -1

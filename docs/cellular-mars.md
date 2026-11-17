# The modem on mars (Xiaomi 11 Pro, SM8350): what this port knows

This is the state of the modem bring-up on mars as of 17 November 2026, written down because the
findings are in the logs and in the experiments, not in the driver's comments.

## What works

* The image loads and TrustZone takes it: `SCM_PAS_INIT_IMAGE` returns 0, `SCM_PAS_MEM_SETUP`
  returns 0 for `0x8f000000` and the image's 256 MB, and `SCM_PAS_AUTH_AND_RESET` returns 0 **once
  the RMTFS buffer is shared the way TrustZone wants it**.
* The modem runs: `slave-kernel = 0x6` about 80 ms after TrustZone lets it out, GLINK's `IPCRTR`
  opens both ways, QRTR gets a HELLO from node 0, and it then tells of two services.
* It asks for its file systems and we answer: `RMTFS_OPEN` for `/boot/modem_fs1` and
  `/boot/modem_fs2` (modemst1 and modemst2, 8 MiB each) both succeed, served out of the run image
  (`fw/modem/...`) because `ufs=2` gives no storage under this port's loader
  (`ufs: no partition information in this UEFI`).
* The buffer is shared: 3 MiB at `0xFB000000`, which is the one window of DDR that is free, below
  4 GB, and acceptable to TrustZone.

## What does not work

* The modem asks for a 3 MiB RMTFS buffer (`RMTFS_ALLOC_BUFF`, request 0x4) and, once it has it,
  asks to read a sector. **Answering that read with success resets the whole machine** -- the
  watchdog of the modem, not a lock in this kernel: the screen shows the link thread turning three
  times and stopping at the top of the fourth, and no log survives.
* Answering it with a refusal does *not* reset the machine: the modem gives up on its own
  (`slave-kernel = 0x107`) and says so:
  `fs_device_efs_rmts.c:161:[6 ,3145728, -907861464] EFS: rmts_get_buffer api failed`.
* So the reset is caused by what the modem does *after* it receives the data it asked for, not by
  the copy, not by the shared window, not by answering from the receive path (all three were
  ruled out one at a time).

## Two channels that do not work, and why

* **The kernel's log** never reaches the host after such a reset: the machine comes back in Android,
  so the loader never gets to collect it, and every `logs/*-run.log` from those runs is the loader's
  own banner (680 bytes).
* **SMEM's item 421** (the modem's last words) does survive the reset, but Android boots in between
  and its own modem clears the item before anything of ours can read it: at the next U-Boot the item
  is already `not there`.

## What the experiments established, in order

1. `AUTH_AND_RESET` failed with `-3145799` (`0xffcfffb9`) while INIT and MEM_SETUP passed. The
   image, its hash table, the region, and the AOSS/RPMh order were all verified one at a time; the
   cause was the buffer: TrustZone would not share the memory we offered, and then refused to start
   the image at all.
2. `rmtfs_share()` asked for three destinations, `HLOS + MSS_MSA(15) + NAV(0x2b)`. **mars has no
   NAV**: with `modem_no_nav=1` the share is accepted (`THE MODEM MAY USE THE BUFFER`) and the
   image starts. Upstream's `docs/ru/cellular.md` does not mention this difference.
3. `sleep_ms()` overflowed above 2147 ms (`delay_for_interval(2200, NSEC_PER_MSEC)` is 2.2e9, past
   int32) and every step from 2 on stopped inside the annotation's two-second pause. Fixed by
   sleeping in one-second pieces.
4. The RMTFS answer is sent from the link thread, out of the receive path, and that is not what
   stops the machine.

## The next instrument

The screen is the only channel that survives: the kernel draws into the loader's framebuffer
(`PE_state.video.v_baseAddr`, `1440 x 3200` at `0xea600000`) and those pixels outlive the reset.
Reading them back from a photograph is unreliable (sixteen levels of grey, perspective); the two
routes that would make it readable are

* a 5x7 font in the kernel, so the words are drawn as text, or
* a write of the words into the `darwin` partition, which Android can read back with `dd` under
  `su` -- no photograph at all.

## What the night of 11-18 established

* **The refusal was ours.** The modem's `ALLOC_BUFF` was answered `NOT GIVEN` because
  `RMTFS_BUFFER_SIZE` was 2.5 MiB (`0x280000`) while the modem asks for 3 MiB (`0x300000`). With
  the size fixed the modem goes on to its first real EFS read.
* **That read, exactly.** `the read was of file 0 (count 1) from sector 1, 1 sectors, at 0xfb000200`
  -- modemst1's second 512-byte sector, the EFS superblock. The window we filled was byte for byte
  the file's own bytes.
* **What kills the phone.** Answering that read *successfully* resets the SoC within about 80 ms
  (no log at all). Answering *refused* does not: the modem gives up cleanly with
  `fs_device_efs_rmts.c:161:[6 ,3145728, -907861464] EFS: rmts_get_buffer api failed` and
  `slave-kernel = 0x107`. Ruled out one by one: the data (fake success, live), the amount
  (`max_sectors=8`, dead), the mapping (`VM_WIMG_IO`, dead). The only variable left is that we said
  success.
* **Android serves the same bytes.** The EFS is fed by the vendor daemon `vendor.rmt_storage`
  (`/vendor/bin/rmt_storage`, 42,688 B, links `libqmi_csi`, `libqmi_common_so`, `libmdmdetect`;
  no crypto of any kind). Its boot-time sequence is OPEN `modem_fs1`, OPEN `modem_fs2`,
  ALLOC 3,145,728, read `fs1` at 512 (512 B), read `fs2` at 512 (512 B), read `fs2` at 1024
  (3,144,704 B), then a 3 MiB write of each at 12.9 s and 144.5 s. So a successful read is followed
  by more reads in the same session -- which is exactly the part we never reach.
* **The partitions are containers.** `modemst1/2` and `fsg` are `IMGEFS*` images with a 472-byte
  envelope (`version 3`, payload `0x2ffe00`, high-entropy body), so "the bytes look odd" is not the
  explanation either -- Android hands the modem the same container.
* **A channel that survives.** The kernel draws its six notes on the framebuffer *and* leaves them
  in the shared memory's last page (`0x80900000 + 0x1fe000`, magic `HNOT`). The loader now answers
  `fastboot oem notes` with those six lines as `INFO` replies, so a reset no longer costs the
  evidence -- provided the notes are read on the next `xl enter`.

## The command line this needs

```
cpus=8 rd=md0 touchscreen=1 modem=1 modem_step=11 sb_delay=600 modem_ipa_fw=0 \
modem_buffer=0xFB000000 modem_no_nav=1 modem_quiet_reads=1 \
soc_ipa_base=0x1e40000 soc_gsi_base=0x1e04000 soc_ipcc=0x408000 soc_aoss=0xc300000 \
soc_rsc=0x18220000 soc_apps_smmu=0x15000000 soc_smem=0x80900000 soc_imem=0x14600000 \
soc_ipa_stream=0x5c0 soc_ipa_stream_uc=0x5c2 soc_ipa_fw=ipa_fws \
soc_mpss_mem=0x8f000000 soc_mpss_size=0x10000000
```

Without `soc_ipcc` there is no IPC: GLINK never comes up, `IPCRTR` is never opened, and the modem
dies before it says a word to us. That is not a subtlety of the modem -- it is a missing argument,
and it silently produced six runs of "the modem says nothing".

`xl` does not append `devices/mars/soc.args`, the kernel's boot-argument buffer is 607 characters,
and `PE_parse_boot_argn` takes the last occurrence of a name -- so the `soc_*` arguments above have
to be passed by hand and never twice.

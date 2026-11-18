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

## What the same night established afterwards (it corrects the line above)

The first summary of the night blamed "saying success" as such. Two experiments afterwards narrowed
it down, and the narrower answer is a different question to ask next.

* **A refused superblock is survivable; a valid one is not.** With `modem_rmtfs_blank=1` (the window
  is zeroed and the read is answered *successfully*) the phone lives: the modem reads an invalid
  superblock, gives up with `fs_rmts_pm.c:1009:[5, 6] rmts_read_iovec failed`, and everything is in
  the log. So a successful answer does not by itself reset the machine -- the *content* it accepts
  does.
* **It is the very first read, not the second.** `modem_rmtfs_silent_after=N` means "answer N reads,
  then go silent" (the counter is checked before the reply is built). `N=1` therefore answers
  *nothing*: the modem's dog fires (`dog_hal_common.c:177:DOG detects stalled initialization, triage
  with IMAGE OWNER`), `slave-kernel = 0x107`, and the phone lives. `N=2` answers exactly the first
  read -- modemst1's superblock -- and the phone resets within about 80 ms, before the modem asks for
  anything else. So the reset follows the modem *accepting* the first superblock, and no further
  request of ours is involved.
* **The data is genuine.** The four file systems are read at run time from the phone's own
  partitions (`hakios_ufs_read_partition()`; the `fw/modem` image fallback never triggers), and the
  `fsg` checksum the kernel computes (`FNV-1a`, `0x9d4f9e3b04b30f44`) is byte for byte what `dd` on
  the phone gives. `modemst1/2` are rewritten by Android's modem at 12.9 s and 144.5 s after boot,
  so cross-boot checksums of those two mean nothing.
* **Echoing the iovec back does not help** (`answer[]` grown to 256, TLV `0x10` with the pieces
  copied in): still a reset. **`memcpy` instead of the byte loop does not help** either, and neither
  does a 4 KiB-granular `settle()` -- so "we are too slow for the modem's timeout" is not shown.

### What the freeze ladder settled (18 Nov, afternoon)

The live log was lying about *where* it looked: the loader chose the newest window of
the kernel's ring and then indexed it `(from + i) % total`, so every log came back as
the **first** 192 KB with a tail of stale bytes, and nothing the kernel said after about
117 s (the modem up, the four RMTFS requests, the give-up) was ever read. Indexed
absolutely, one late fetch carries the whole exchange.

With that channel working the question "does the modem pull the phone down, or does our
code die?" could be answered by making our side do *nothing at all*:

| what the kernel does after the answer | what happens |
|---|---|
| refuses the read (`modem_rmtfs_rw_off=1`) | lives; the modem gives up 45 ms later (`0x107`, `fs_rmts_pm.c:1009:[5, 6] rmts_read_iovec failed`) |
| answers read 1, then **freezes** (`modem_rmtfs_halt=1`) | lives to the deadline: the first answer is acceptable to it |
| answers reads 1 and 2, then **freezes** (`modem_rmtfs_halt=2`) | the phone is gone, and no log survives |

A frozen kernel sends nothing, touches no MMIO and reads no storage, so the 80 ms that
follow the second answer are the modem's own doing: it **asserts and pulls PS_HOLD**.
Consistently, a window of zeroes is refused cleanly (`rmts_read_iovec failed`, phone
alive) while a window holding the real container sends it down: it is parsing the EFS
container and failing inside itself.

Two dead ends measured the same afternoon:

* **A header with a zero body** (`modem_rmtfs_zero_after=0x1d8`): still a reset. The
  container's 472-byte header alone is enough to send it down that path.
* **The buffer at 0x9c700000** (the CDSP image, where the phone's own heap falls):
  TrustZone *shares* it now that the share names two destinations, and then
  `SCM_PAS_AUTH_AND_RESET` answers `-3145796` — that address sits inside the modem's own
  0x8f000000+0x10000000 region, so the image and the buffer collide. The address is not
  free to choose; 0xFB000000 stays.

What is left, and the order to test it in:

1. **A valid magic with a zero body.** Serve the superblock's first bytes and nothing else: if the
   phone lives, the modem is parsing deeper into EFS; if it dies, a magic alone is enough to send it
   down the fatal path.
2. **Tell a modem assert from a kernel panic.** The PMIC's PON block (`pon_pbs@800`, `pon_hlos@1300`
   on `pmk8350`) keeps the reset reason across a reset; reading it in the loader (or in the kernel at
   the next boot) says whether `PS_HOLD` was pulled or the AP died. `ro.boot.bootreason` is useless
   here -- it reads `bootloader` because the loader was entered through `fastboot boot`.
3. **Compare the two superblocks.** `modemst1` and `modemst2` are written three minutes apart; if the
   modem insists that both carry the same generation and we hand it a pair from two different
   generations, that is a plausible reason for it to die on the first valid read. Waiting past the
   144 s write before starting a run is the cheap half of this experiment.

## The buffer address was the whole story (19 Nov, 01:00)

The modem did not reset the phone because it disliked an answer. It reset the phone because the buffer we
handed it was not a buffer its EFS code can use.

`rmt_storage` on this phone maps a UIO device called `rmtfs`: physical **0xD6A00000**, size **0x300000** --
exactly the 3,145,728 bytes the modem asks for in `ALLOC_BUFF`. We had been handing it `0xFB000000`, a hole in
DRAM between the ramdisk and the resident image: writable, shared with the modem by TrustZone, and useless to
the modem's EFS code, which faulted and took the machine down with it. Every observation that made this look
like a protocol problem is explained by that: all zeros in the window survived (nothing to parse), a real IMGE
header killed the machine (it parsed it), answering the first read and then freezing survived, answering the
second did not, and serving fs1's bytes for fs2 did not help either -- the bytes were never the point.

With 0xD6A00000 the exchange completes and the machine stays up (`logs/004228-run.log`):

    mpss: rmtfs: the modem opens /boot/modem_fs1 (modemst1, 8388608 bytes): caller 1
    mpss: rmtfs: the modem opens /boot/modem_fs2 (modemst2, 8388608 bytes): caller 2
    mpss: rmtfs: the modem asks for a buffer of 3145728 bytes: given ours at 0xd6a00000
    mpss: rmtfs: the modem reads 1 pieces of modemst1: served
    mpss: rmtfs: the modem reads 1 pieces of modemst2: served
    mpss: link: the read was of file 1 (count 1) from sector 2, 6142 sectors, at 0xd6a00400
    mpss: link: rmtfs: 6 requests, 3 reads (6144 sectors), 0 writes, 0 refused
    mpss: link: down; GLINK said 178 and heard 201 messages, QRTR sent 89 and received 75 datagrams

What is left is the modem's own state: its entry still says 0x107 rather than "up", so DMS and NAS are not
started. `devices/mars/run.sh` now carries `MODEM_BUFFER=modem_buffer=0xd6a00000` for every modem experiment,
and the driver's default is that address.

## The IPA's firmware, and what happened once it started (19 Nov, 01:30)

The modem's own last words named the next thing: `ipa_hal.c:7446:IPA Assert: GSI_STATUS_SUCCESS == errval
failed: Failed to initialize GSI channel: CHID 1 IPA EPID 6 errval 5`. That is the modem's IPA, which needs the
IPA's firmware before the modem starts. It was not in our image: the phone keeps it at
`/vendor/firmware/ipa_fws.{mdt,b00..b04}` (7,020 bytes of header and five segments), and `fw-modem/image/`
had never been filled. `userland/build.sh` now puts it in the small image as `fw/image/ipa_fws.*`, the
driver's default name is `ipa_fws` (it was `yupik_ipa_fws`), and the loader looks for it through
`image_file()` -- the small image first, then `/fw` on the root -- so it can be loaded without rebuilding the
2.7 GB system image.

With it there, the IPA comes up (`logs/012034-run.log`):

    mpss: 7: the IPA's image: 3 segments to load, 0x7000 bytes from 0x8b710000
    mpss: 7: THE IPA'S FIRMWARE IS LOADED AND STARTED (image 15, 3 segments)
    mpss: 7: GSI_CFG 0x205 (bit 0 GSI enable, 1 MCS enable, ...), MCS_CFG 0x1 (bit 0: the sequencer runs)
    mpss: 7: THE MODEM SAYS IT IS UP: "slave-kernel" says 0x6, 74.521 ms after TrustZone started it

and the machine stays alive through step 8, where the IPA is shut down again and the SMMU reports that it
refused the IPA nothing.

What is left: a full run (`modem_step=11`) with the IPA up stalls the kernel. The IPA's own tables
(`ipa_memory_init()`, `ipa_hardware_init()`, `hakios_ipa_data_setup()`) are still skipped by
`modem_ipa_raw=1`; those writes are the next thing to attribute, one at a time, with `modem_ipa_quiet=1`
(the reads of the IPA's registers stall the bus when its clock is not really on).

### After the IPA's firmware: where it stops (19 Nov, 02:05)

The IPA's firmware being loaded changed the failure, and the steps say how far each run gets
(`modem_step=N` stops after step N; step 7 starts the IPA, 8 the queues, 9 the link, 10 QRTR, 11 NAS):

| run | IPA firmware | our IPA tables | stop after step | result |
|---|---|---|---|---|
| `logs/012034-run.log` | loaded | skipped (`modem_ipa_raw=1`) | 8 | finishes, the IPA is shut down again |
| `logs/014321-run.log` | loaded | skipped | 9 | finishes |
| this one | loaded | skipped | 10 | **the phone is gone, no log** |
| `logs/015441-run.log` | not loaded | skipped | 10 | finishes, but `THE MODEM DOES NOT ACKNOWLEDGE THE STOP after 5000.026 ms` (its entry 0x106) |
| this one | loaded | enabled | 10 | **the phone is gone, no log** |

So the IPA being started is what makes step 10 fatal, and it is fatal with and without our own IPA
table/hardware initialisation. In step 10 we ask the modem's IPA driver to initialise — `mpss: ipa: about to
ask the modem's IPA driver (node 0 port 28) to initialize with the IPA's memory layout (154 bytes, offset 0)`
— and with the IPA's firmware running that request takes the phone down within a few hundred milliseconds.
Without the firmware the same request leaves the modem alive but stuck (`0x106`, ignoring the stop request),
which is what a modem whose IPA cannot come up looks like from outside.

The next things to try, in order:

1. Do not send that request at all (`modem_ipa_skip_qmi=1`, to be added): if step 10 then completes with the
   IPA's firmware loaded, the request is the trigger and its 154-byte layout is wrong for this phone.
2. Program the IPA's hardware before the modem is started rather than after (the order in which Android does
   it: IPA hands its GSI/CE registers over, then the modem binds channels through QMI).
3. Compare the 154 bytes against what Android's IPA driver sends on this phone (`/sys/kernel/debug/ipa` is
   not available to us; its dmesg does print the IPA's memory layout at boot).

### The modem registers (19 Nov, 02:20)

Leaving the IPA out entirely -- no firmware for it, no INIT_DRIVER sent to it (`modem_ipa_fw=0
modem_ipa_skip_qmi=1`) -- a full run gets further than any run before (`logs/021801-run.log`):

    mpss: 11: about to ask DMS (node 0 port 70) for operating mode 0 (0 online, 1 low power): message 0x2e
    mpss: 11: mode 0: THE MODEM AGREES (result 0, error 0)
    mpss: 11: after 58047 ms: DMS there, the modem online; NAS there (node 0 port 59), asked 21 times,
             answered 22, 2 changes; REGISTERED WITH A NETWORK; signal -119 dBm (NR), 2 bars

That is the phone's own modem, taken out of reset by us, told to go online by us, and answering with a
registration and a signal reading (-119 dBm on NR, two bars).

It does not stay: twenty seconds later it gives up, and its last words say why --

    mpss: link: THE MODEM SAYS IT HAS GIVEN UP ("slave-kernel" = 0x107)
    mpss: 11: THE MODEM'S LAST WORDS (item 421, 256 bytes):
              "ipa_sio.c:2517:IPA Assert: ipa_ipfltr.init_done == TRUE failed: Init message from A7 not yet
               received, failing SIO config"

So the IPA is what keeps the modem alive: its filter tables are initialised by the application processor's
INIT_DRIVER, and without that message the modem's SIO configuration fails and it stops. Sending that message
while the IPA's firmware is running is what takes the whole phone down (the last three runs), and the IPA's
firmware starts at all only now that `fw/image/ipa_fws.*` is in the image.

What is left, then, is the IPA itself: its microcontroller's ready interrupt (`ipa3_uc_ready`, which Android
waits on through `ipa3_uc_init`), and the order Android keeps -- memory and hardware initialisation, then the
microcontroller, then INIT_DRIVER. Our `ipa_memory_init()` and `ipa_hardware_init()` run after the firmware is
already started; whether that order is what kills the phone is the next thing to settle.

### The order of the IPA's own initialisation was not it (19 Nov, 02:30)

`ipa_memory_init()` and `ipa_hardware_init()` now run before TrustZone is asked for the IPA's header, as
Android's driver has them, rather than after the microcontroller has started. With the IPA's firmware in the
image and INIT_DRIVER sent, step 10 still takes the phone down with no log, exactly as before.

So what is left is the IPA's microcontroller handshake itself -- Android waits on `ipa3_uc_ready` (its
`ipa3_uc_init` and the uC's interrupt) before it sends INIT_DRIVER, and we have no such interrupt wired.
Until that is ported, the configuration that works is the one without the IPA at all:

    modem_ipa_fw=0 modem_ipa_quiet=1 modem_ipa_skip_qmi=1 modem_buffer=0xd6a00000

which takes the modem online and registers it on a network, and loses it twenty seconds later to the assert
quoted above (`logs/021801-run.log`).

## The IPA's registers do not answer, before or after its firmware starts (19 Nov, 03:45)

Three runs settled this, each with the machine's own behaviour as the evidence:

* `modem_ipa_status_off=2` (no read at all) + the firmware started: the run gets through step 8 and returns
  (`logs/032121-run.log`, `step 8: done`, and `mpss: ipa: at the end: the SMMU has refused the IPA nothing`).
* the same run with the status read left in: **the machine hangs** (`xl: no answer`).
* a split gate (`modem_ipa_status_off=1`, which leaves out only the read taken *before* the firmware starts):
  the read after `SCM_PAS_AUTH_AND_RESET` hangs too (`logs/033537-run.log` is the last good one; the run after
  it wrote no log at all).

So the IPA's register window at `1e00000` (the address its own device tree gives it: Android's dmesg has
`ipa 1e00000.qcom,ipa:ipa_smmu_ap`) does not answer a read even with its microcontroller running. Writes do not
wait for an answer, which is why every "table written" line in earlier runs proved nothing: they were carried out
on a block nobody was serving.

That is the whole of the remaining cellular work's first step: **make the application processor's access to the
IPA's block work.** Android's driver does `clk_prepare_enable(ipa_clk)` before its first register read; ours votes
for the BCM `IP0` (`ipa_clock()`, `BCM_VOTE(on, 0x3fff)`) and the RSC command completes, but the block still does
not answer, so either that name is not the IPA's clock, or a clock's vote is not a 14-bit level (`RPMH_ARC_CLK_ON`
is one bit, not `0x3fff`). Until that is right, nothing that needs the IPA -- `INIT_DRIVER`, the GSI channels, the
microcontroller's mailbox -- can be done at all, and the modem will keep asserting with
`Failed to initialize GSI channel: CHID 1 IPA EPID 6 errval 5`.

## The IPA, from the phone's own device tree (19 Nov, 05:00)

`devices/mars/prebuilt/device-tree.dtb` holds the node, and it settles three things that were being guessed at
(`dtc -I dtb -O dts -o /tmp/mars.dts devices/mars/prebuilt/device-tree.dtb`):

    qcom,ipa@1e00000 {
        reg = <0x1e00000 0x84000  0x1e04000 0x23000>;
        reg-names = "ipa-base\0gsi-base";
        clock-names = "core_clk";
        clocks = <0x29 0x16>;          /* 0x29 = qcom,lahaina-rpmh-clk, clock 22 */
        qcom,ipa-hw-ver = <0x13>;      /* v4.11, as expected */
        ...

1. **The IPA's register window is 0x1e00000 for 0x84000 bytes**, not the 0x4000 that was mapped. The GSI is
   0x1e04000 for 0x23000. Every read past the first 16 KB was a read of an address nothing answers: on this SoC
   that stalls the bus instead of faulting, which is why "reads hang" came to be a rule. It was not a rule.
2. **The IPA's clock is an RPMh clock (rpmhcc index 22), not a GCC clock.** The command database knows a BCM
   called "IP0" at 0x5007c with the levels `40 0 8 7`, and a clock's vote carries one of those levels: a vote of
   0 asks for 0 kHz, which leaves the clock off. Two attempts were made and both hung the machine with no log
   (a vote of 300, which is not a level the database lists, and a vote of 40 with the window widened); the
   driver was put back to the committed state afterwards.
3. **The IPA's local memory is inside the IPA's own window**, at the offset the register at 0x54 gives
   (bits 31:16, in 8-byte units). There is no separate `0x1e40000` window to write to -- writes there are a data
   abort (pc `_wr`, lr `ipa_memory_init`), which is what the last several runs died of.

What is left, in order: map the IPA's window at its real size, vote IP0 with a level from its own table, then
read 0x54 and zero the local memory at that offset -- and only then the IPA's INIT_DRIVER can be answered.

## The IPA's window and its clock (19 Nov, 06:00)

Two things the device tree settles, and one that a run settled.

The window is 0x84000 bytes, not 0x4000:

    qcom,ipa@1e00000 { reg = <0x1e00000 0x84000  0x1e04000 0x23000>;
                       reg-names = "ipa-base\0gsi-base"; };

Every experiment that read past 0x4000 did not fault: it wedged the bus, which is why "reads hang, writes do not"
looked like a rule about the hardware. With the window mapped at its real size, `ipa_memory_init()` runs and the
panics that used to be at `pc _wr, lr _ipa_memory_init+0x80` are gone.

The clock does not need a vote of ours. `GSI_CFG` at 0x1e04000 reads back 0x205 with no vote at all, and the
firmware loads and starts (its three segments read back byte for byte). And a level on "IP0" -- which the command
database puts in the same slave as MC0, SH0, SN8 and ACV, the interconnect -- takes the machine down three times
out of three, with no log and no answer from the loader. `modem_ipa_clk_off=1` skips the vote; a full run then
returns (logs/055719-run.log, 221,680 bytes).

What is left is the order Android uses and we do not: the microcontroller is started first, and only once it is
running does the driver read the local-memory offset from register 0x54 and zero that memory. Our read of 0x54
comes back 0 -- the microcontroller has not written it yet -- so the offset is meaningless and the run stops at the
IPA's header. So: start the microcontroller (TrustZone's AUTH), then poll 0x54 for a non-zero offset, then zero the
local memory at that offset, then send INIT_DRIVER.

## The two orders, and why neither holds yet (19 Nov, 06:10)

Android does it in this order: start the IPA's microcontroller (TrustZone), then read the local-memory offset from
register 0x54, then zero that memory, then send INIT_DRIVER. We have now tried both halves of it:

* the memory first (the old order): the run returns, but 0x54 is still 0 -- the microcontroller has not written it
  -- so step 7 ends at the IPA's header and the modem never starts (logs/055719-run.log);
* the microcontroller first (`modem_ipa_after=1`): the run hangs, with no log and no answer from the loader.

So the missing piece is not the order but the handshake: something between "TrustZone has started the
microcontroller" and "the local memory may be written" is not done. Android decides `ipa3_uc_ready()` by reading the
microcontroller's command, response and event words in that memory; ours reads them at the wrong time, and one of
those reads is what takes the bus down.

Until that handshake exists, the configuration that works is the one without the IPA: `modem_ipa_fw=0
modem_ipa_skip_qmi=1 modem_buffer=0xd6a00000`. It brings the modem up, registers with a network
(`REGISTERED WITH A NETWORK; signal -119 dBm (NR), 2 bars`) and holds for about twenty seconds before the modem
asserts in its own IPA path (`ipa_sio.c:2517 IPA Assert: ipa_ipfltr.init_done == TRUE failed`).

## The uC's block: reached through a register, not by mapping its address (19 Nov, 06:15)

Both of our orders failed for the same reason, and Android's source says what it is. In
`drivers/platform/msm/ipa/ipa_v3/ipa_uc.c`:

    ipa3_uc_interface_init(): phys_addr = ipa_wrapper_base + ipa_reg_base_ofst +
                                          ipahal_get_reg_n_ofst(IPA_SRAM_DIRECT_ACCESS_n, 0);
                              ioremap(phys_addr, IPA_RAM_UC_SMEM_SIZE);
    send_uc_command():        shared->command = command; shared->command_param = param;
                              shared->response = 0; shared->response_param = 0;
                              write IPA_IRQ_UC.UC_INTR to tell the microcontroller.

The 128-byte block the microcontroller talks through is reached **through a register of the IPA**
(`IPAHAL_SRAM_DIRECT_ACCESS`) with an address/data pair -- not by mapping 0x1e50000 and reading it. Our mapping of
0x1e50000 and the reads at +0x00/+0x0c/+0x14/+0x1c are therefore reads of a window the IPA is not answering, which
is exactly the "reads hang the bus" behaviour that took every run down.

So the next step is not another order: it is the direct-access register pair. Read the uC's response word through
it, poll for `IPA_HW_2_CPU_RESPONSE_INIT_COMPLETED` (0x1) with a bounded wait, and only then read 0x54 and zero the
local memory.

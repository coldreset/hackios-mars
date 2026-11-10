# Handoff: what this project is, what works, what does not

Read this first in a new session. Everything here is verifiable from the
repository; nothing is a guess.

## The project

HackiOS on a Xiaomi 11 Pro (`mars`, M2102K1AC, SM8350 "lahaina", Adreno 660,
12 GB, 1440x3200). It boots XNU 8796.121.2 / Darwin 22.5 with the iOS 16.5
userland, **without flashing or repartitioning**: the system image is uploaded to
RAM and the kernel boots from there.

Repository: `/opt/hackios/hackios` (WSL Ubuntu, run as root). The host is
Windows; helper scripts live in
`D:\Users\Administrator\Documents\deepseek-harness\default-workspace`.
Public mirror: `coldreset/hackios-mars`, branch `main`, pushed from the local
`release` branch (`git push mars release:main`).

## How to run it

```
export DEVICE=mars            # without this: "mkubootimg.py: no u-boot.bin for redwood"
./xl reload                   # ~20 s, puts the loader on the phone
./xl ramdisk userland/system.apfs   # ~72 s for 2381 MB, RAM only
XL_SECS=150 XL_CMDLINE="debug=0x8 cpus=8 rd=md0 touchscreen=1" ./xl run
```

The kernel returns to the loader, which prints the interesting log lines on the
screen and holds a window (`loader_wait`, default 120 s). `fastboot oem done`
resets to Android immediately.

Do not use the `u-boot.bin` named `73875cf8…` (the 01:47 build): the phone
disappears from USB. The working one is `07b428b7…`.

## What works

- **Boot chain**: u-boot → `xnuloader/loader.efi` → XNU → launchd → SpringBoard.
- **Display**: 1440x3200, 60 fps; CoreAnimation runs on Adreno 660 through Mesa
  freedreno (`GL=1 ./build.sh`; `fb_ca_hz=120`, `FD_MESA_DEBUG=sysmem`).
  `gpu_recover` recovers a hung GPU block.
- **Touch**: ST FingerTip FTM5 over SPI (`drivers/input/hakios.focaltech.c`),
  works; gestures depend on the identity strings (`xnuloader/mkidentity.py`).
- **Boot menu**: drawn and functional — `fastboot oem menu up|down|select`,
  three items, the choice actually acts (`b435c27`). The phone's own keys drive it
  too: the loader reads them over SPMI (volume up/down move, power chooses). See
  `docs/boot-menu.md`.
- **Keys**: the kernel reads all three (power, volume up, volume down) over
  SPMI; volume up reaches backboardd as consumer usage `0xE9` (`2ac4a2b`). The
  loader does the same two reads itself, with no PMIC driver of its own.
- **Wi-Fi up to the board-data step**: PCIe, MHI, QRTR, QMI, the memory
  handshake (firmware answers `result=0`), the regulatory database downloads in
  four acknowledged pieces.

## What does not work

- **Wi-Fi board data**: the radio accepts at most four `0x25` messages, 6144
  bytes each, one file per boot — 24,576 bytes — and rejects anything larger
  with `error 19`. The vendor board data is 58 KB. `docs/wifi-board-limit.md`
  has every test that led here, including what has been ruled out (chunk size,
  pacing, `total_size`, file id, BDF type, `end` flag, firmware version, real
  DMA buffers, slices, delayed path). The ROM (`/opt/hackios/rom/NON-HLOS.bin`,
  variant `lahaina.adsp.prod`) contains only `6490.wlanfw.eval_v1/v2_TOQ`, so
  there is no other firmware to try.
- **Audio**: no audio device, so the volume rocker cannot show a volume bar.
- **Wallpaper / Settings**: RunningBoard kills SpringBoard because
  `CollectionsPoster` is never launched.
- **Chinese UI**: `zh-Hans.lproj` does not exist in the image; needs resources
  from an IPSW.
- **Metal**: `metal=3` gives backboardd a device but 24 calls are unimplemented;
  the GL path is what draws today.

## Next steps, in the order that pays off

1. **Wallpaper crash** — find why `CollectionsPoster` is not launched.
2. **Chinese resources** — needs an IPSW the user does not have; park it.
3. **Wi-Fi** — blocked on understanding why the firmware stops after four
   messages. Static analysis of `amss.bin` failed: it is an ELF32/ARM image whose
   code does not disassemble (entry point is not code, Thumb `push` density is
   0.06 %, strings are mixed into executable segments). Needs the vendor's
   loader, or a different delivery mechanism.

## How to work on this without wasting money

- One device round is 6–10 minutes: reload 20 s, upload 74 s, run 150–240 s,
  capture. The loader-only rounds (boot menu) are ~20 s.
- `logwords=a,b,c` is a whitelist on the loader's log screen: a new line that
  does not contain one of the words will not be shown at all. Keep the words
  narrow — a broad word like `wifi-pcie` fills the window and hides what matters.
- The screen dump shows the first 100 and last 50 matching lines when there are
  more than 150.
- **Verify the log belongs to this run** (`hakios_kver=…` and the kernel md5)
  before drawing any conclusion. Several rounds were wasted on logs that came
  from another run.
- `fastboot get_staged` must be given a `/tmp` path: the fastboot in WSL is a
  wrapper around `fastboot.exe`, which cannot write to `/mnt/d/...`.
- Never build with `make kernel` outside `/opt/hackios/hackios`, and always copy
  `drivers/**` into `xnu-src/xnu-8796.121.2/bsd/dev/` afterwards — the kernel is
  compiled from `xnu-src`, not from `drivers`.
- Avoid nested quoting through `wsl.exe bash -lc "…"`: write a `.sh` or `.py`
  file and run it, or PowerShell will eat the `$( )`, the quotes and the pipes.
- After a run the phone sometimes has to be revived with 15 s on the power key.

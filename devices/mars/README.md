# Xiaomi 11 Pro (`mars`)

Qualcomm **SM8350 "lahaina"** / Snapdragon 888, 12 GB RAM, 1440x3200 AMOLED, Adreno 660, UFS 3.1.
This is the first SM8350 device in the tree, and the first device in the project whose touch
controller speaks **SPI ST FingerTip** rather than I2C.

| | |
|---|---|
| Vendor | Xiaomi |
| SoC | `lahaina` (SM8350) |
| Configuration | `python3 tools/hakios-config.py defconfig mars mini touch perf` |
| Boot | from RAM: `fastboot boot`; nothing is flashed, no partition is touched |
| Maintainer | [coldreset](https://github.com/coldreset) (Yunlong Zheng) |

## The SoC arguments

`soc.args` and `soc/configs/lahaina.config` were read out of the phone's own device tree by
`tools/soc-args.py` / `pull-soc.sh`. Three entries the generator could not find are still missing and
matter only for cellular, which this port does not try to bring up: `soc_cmd_db`, `soc_mpss_mem`,
`modem_ipa_fw_at`.

Everything else the kernel needs comes from `devices/mars/configs/mars.config` and the shared
`configs/*.config` fragments.

## What works

| Part | State | |
|---|---|---|
| boot chain | works | the loader comes up in RAM and hands over to XNU |
| display | works | 1440x3200 through the loader's framebuffer; drawn by the GPU |
| touch | works | ST FingerTip FTM5 on `qupv3_se11_spi`; taps land where you tap, drags register (gesture classification is still rough, see Known issues) |
| cpu | works | all 8 cores (`cpus=8`); the A78/X1 cores sit at their top OPP with `cpufreq=1` |
| gpu | works | Adreno 660, all 21 bring-up steps pass; OpenGL ES through Mesa freedreno (`gpu=1 adreno_nomeasure=1 gl=1`) |
| storage | read only | the system image is resident in RAM; `var_store` / UFS writes are not enabled yet |
| buttons, battery, thermal, wifi, audio, camera | not started | the drivers exist in the tree but are off in this device's config |

## Building

```sh
python3 tools/hakios-config.py defconfig mars mini touch perf
make kernel                      # about 3 minutes
(cd xnuloader && ./build.sh)     # loader.efi
```

`configs/mini.config` switches off the parts this port has not brought up, `configs/touch.config`
turns on `CONFIG_HAKIOS_FOCALTECH` (the driver that speaks the ST protocol on this phone) and
`configs/perf.config` turns on `CONFIG_HAKIOS_CPUFREQ`.

## Running

```sh
export DEVICE=mars                   # xl defaults to redwood
adb reboot bootloader
./xl enter                           # the UEFI loader into RAM
./xl ramdisk userland/system.apfs    # 2.3 GB resident system image, about 75 s
XL_RAMDISK=userland/root.apfs XL_SECS=240 \
XL_CMDLINE="debug=0x8 cpus=8 rd=md0 touchscreen=1 cpufreq=1" ./xl run
```

Boot arguments that matter on this phone:

| Argument | Why |
|---|---|
| `cpus=8` | all eight cores; the slower ones join about 28 s into the boot |
| `cpufreq=1` | without it the A78/X1 stay at their lowest OPP and the UI runs at 7 fps instead of 18 |
| `touchscreen=1` | the touch driver does nothing without it |
| `touch_reset_ms=N` | how long the controller is given after reset (default 200 ms) |
| `touch_bitbang=1` | drive the pads by hand instead of through the serial engine |
| `adreno_nomeasure=1` | SM8350 has no usable GCC debug counter to check the GPU clock |
| `quiet=1` | suppress periodic counters so the tail of the kernel log stays readable |
| `touch_watch=1` | start the touch watchdog: a line every 5 s with the stage it is in, and the periodic counters |
| `touch_trace=1` | print every raw touch event with the time it was read |

## Touch: what this phone actually has

The vendor device tree says `fts@0 { compatible = "st,spi" }` on `qupv3_se11_spi` (`spi@a8c000`):
an **ST FingerTip FTM5**, not a FocalTech part, even though several Xiaomi trees call the driver
folder `focaltech_spi`. Three things have to be right:

1. **Reads use `0xFB`, writes use `0xFA`.** The I2C driver uses `0xFA` for both.
2. **The chip starts in 3-wire SPI.** It must be told to use 4 wires, by writing five registers:
   `0x20000032=0x10`, `0x20000034=0x02`, `0x2000003f=0x07`, `0x2000003d=0x30`, `0x2000002d=0x02`.
3. **Do not system-reset it after that.** A reset puts it back into 3-wire mode, and the SPI
   controller then reads back its own bytes instead of the chip's.

The chip ID reads `36 39` (`DCHIP_ID_0` / `DCHIP_ID_1`). Events are 8 bytes: `0x13` enter, `0x23`
motion, `0x33` leave; `x = ((e[3] & 0x0f) << 8) | e[2]`, `y = (e[4] << 4) | ((e[3] & 0xf0) >> 4)`.
The FIFO has to be drained in bursts (`FTS_CMD_READONE_FIFO 0x87` + dummy + n x 8 bytes); one event
per transfer leaves the queue standing still.

## Known issues

- **Home swipe is misread.** A swipe up from the bottom sometimes opens Control Center instead of
  going home, and a swipe often needs a second try. During a drag the panel re-sends an *enter*
  event (`0x13`) for every sample, with the x coordinate jumping, so the system sees a series of
  taps rather than one finger. Under investigation.
- **10-18 fps.** The compositor is CPU-bound: the GPU and the framebuffer driver are not the limit
  (about 1.7 ms of GL and 5.8 ms of present per frame). Process-level CPU accounting is not working
  yet, so the biggest consumer is still unknown.
- **A run usually ends with the loader wedged in fastboot** (`fastboot devices` shows
  `????????????`); a 15 s power-button hold recovers it. The loader's cold-reset path is being made
  reliable.
- **Logs are read off the screen.** After the kernel returns, the loader prints the last 64 KB of
  the kernel log and waits 300 s, then cold-resets. USB is not available in that window.

## Where the numbers come from

- SoC layer: the phone's own device tree (`soc.args`, `soc/configs/lahaina.config`).
- Panel, touch pins, rails: the vendor device tree, `mars-sm8350.dtsi` in LineageOS's SM8350 tree.
- GPU: `fw-gpu/a660_*` from the phone's own firmware partition (not distributed here).
- Touch protocol: `MiCode/Xiaomi_Kernel_OpenSource`, branch `star-r-oss`,
  `drivers/input/touchscreen/fts_spi/`.

## Maintainer

Yunlong Zheng ([@coldreset](https://github.com/coldreset)) — issues and pull requests are welcome;
a log from the screen (see Known issues) and the phone model are the two things that help most.

## Thanks

The device repository layout, the loader, the driver framework and the user space are the HakiOS
project's. The display-out and Metal notes came from reading the [raphael port](https://github.com/HeliumStudio-Dev/hackios-xnu-kernel)
and the [OnePlus 9 port](https://github.com/b1ackm4x/hackios-xnu-kernel); the USB/log and cold-reset
lessons from [hakios-thyme-mi10s](https://github.com/Liquidation0619/hakios-thyme-mi10s). No code
from those repositories is included in this tree.

## What is not in this repository

- `prebuilt/device-tree.dtb` — the phone's own device tree (3.2 MB), read by `tools/mkubootimg.py`. Xiaomi's, so it
  is not redistributed here: pull it from your own phone (`adb pull /sys/firmware/fdt` on a rooted ROM, or out of the
  stock `dtbo`/`boot` image) and put it at `devices/mars/prebuilt/device-tree.dtb`.
- `fw-gpu/`, `fw-touch/`, `fw-modem/` — vendor firmware, loaded from the phone at run time, never committed.

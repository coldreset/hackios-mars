# Xiaomi 11 Pro (`mars`)

Qualcomm **SM8350 "lahaina"** / Snapdragon 888, 12 GB RAM, 1440x3200 AMOLED, Adreno 660, UFS 3.1.
This is the first SM8350 device in the tree, and the first device in the project whose touch
controller speaks **SPI ST FingerTip** rather than I2C.

| | |
|---|---|
| Vendor | Xiaomi |
| SoC | `lahaina` (SM8350) |
| Configuration | `python3 tools/hakios-config.py defconfig mars mini touch perf power` |
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
| touch | works | ST FingerTip FTM5 on `qupv3_se11_spi`; taps land where you tap, drags and the home / app-switcher swipe work |
| cpu | works | all 8 cores (`cpus=8`); the A78/X1 cores sit at their top OPP with `cpufreq=1` |
| gpu | works | Adreno 660, all 21 bring-up steps pass; OpenGL ES through Mesa freedreno (`gpu=1 adreno_nomeasure=1 gl=1`, and the libraries must be in the image: `GL=1 userland/build.sh`) |
| storage | read only | the system image is resident in RAM; `var_store` / UFS writes are not enabled yet |
| battery | works | the audio DSP's battery manager (`adsp=1`): capacity, voltage, current, temperature, charge and the charger's state, every 10 s |
| buttons | partial | the power and volume keys are read; presses reach `backboardd` as consumer events |
| wifi | partial | ath11k's bring-up stages pass up to the board data, which does not fit the radio's message limit (`docs/wifi-board-limit.md`) |
| widgets | works | home-screen and Today-view widgets fill in about ten seconds after SpringBoard;
  they need the `com.apple.chronod` job in `/redwood/LaunchDaemons` (see Running) |
| thermal, audio, camera | not started | the drivers exist in the tree but are off in this device's config |

## Building

```sh
python3 tools/hakios-config.py defconfig mars mini touch perf power
make kernel                      # about 3 minutes
(cd xnuloader && ./mkidentity.sh /path/to/iPhone10,3_16.5_20F66_Restore.ipsw && ./build.sh)
```

`mkidentity.sh` lifts the `/product` node out of the device tree inside the IPSW -- on this phone
61 properties, among them `thin-bezel`, `oled-display` and `artwork-device-idiom`. Without it the
loader builds with a placeholder identity, and Darwin then reads the machine as an older phone with
a home button: **no home indicator, and a swipe up from the bottom brings up Control Center instead
of the home screen.** Nothing from Apple is downloaded or committed; the header stays out of the
tree (`.gitignore`).

`configs/mini.config` switches off the parts this port has not brought up, `configs/touch.config`
turns on `CONFIG_HAKIOS_FOCALTECH` (the driver that speaks the ST protocol on this phone) and
`configs/perf.config` turns on `CONFIG_HAKIOS_CPUFREQ`.

## Running

```sh
export DEVICE=mars                   # xl defaults to redwood
adb reboot bootloader
./devices/mars/run.sh 300            # verify, then: xl enter, xl ramdisk, xl run with the line below
```

`devices/mars/run.sh` is the one command line, and it refuses to start when a piece of it is
missing from the images. Three of these switches have been lost once each, and each loss is
**silent** -- nothing fails, the feature is just absent:

| Switch | What is lost without it |
|---|---|
| `adsp=1` | the battery is blank: no percentage, no charge state. It comes only from the audio DSP's battery manager |
| `gpu=1` | the kernel does not bring the GPU up, and `userland/build.sh` only installs the GL libraries into a root built for a kernel that did |
| `gl=1` | CoreAnimation stays on the software renderer: every material, blur and widget surface is painted flat white |

A fourth thing lives in the image rather than in the line: the job that produces widgets.
`build.sh` replaces `/System/Library/LaunchDaemons` with a link to `/redwood/LaunchDaemons` and
copies a chosen few jobs into it, and `com.apple.chronod` has to be one of them -- it owns
`com.apple.chronoservices` and `com.apple.chrono.widgetcenterconnection`. Without it SpringBoard
asks launchd for the label every second (`Could not find job with label com.apple.chronod`) and
every widget is drawn empty, while the rest of the interface looks finished.

The GL libraries also have to be **inside** the image: `GL=1 userland/build.sh` puts
`libOSMesa.8.dylib` and `libhakios.gl.dylib` into `root/lib/`. A plain rebuild drops them, which is
why the script looks inside `root.apfs` before it starts anything.

```sh
export DEVICE=mars
adb reboot bootloader
./xl enter                           # the UEFI loader into RAM
./xl ramdisk userland/system.apfs    # 2.6 GB resident system image, about 80 s
XL_SECS=240 XL_CMDLINE="cpus=8 rd=md0 touchscreen=1 adsp=1 gpu=1 gl=1 adreno_nomeasure=1 cpufreq=1" ./xl run
```

Boot arguments that matter on this phone:

| Argument | Why |
|---|---|
| `cpus=8` | all eight cores; the slower ones join about 28 s into the boot |
| `cpufreq=1` | without it the A78/X1 stay at their lowest OPP and the UI runs at 7 fps instead of 18 |
| `touchscreen=1` | the touch driver does nothing without it |
| `touch_reset_ms=N` | how long the controller is given after reset (default 200 ms) |
| `touch_bitbang=1` | drive the pads by hand instead of through the serial engine |
| `gpu=1` | starts the Adreno 3D core and registers the `HakiosGPU` service; without it CoreAnimation keeps the software renderer |
| `adreno_nomeasure=1` | SM8350 has no usable GCC debug counter to check the GPU clock |
| `quiet=1` | suppress periodic counters so the tail of the kernel log stays readable |
| `touch_watch=1` | start the touch watchdog: a line every 5 s with the stage it is in, and the periodic counters |
| `touch_trace=1` | print every raw touch event with the time it was read |
| `gpu_recover=1` | reset and restart the 3D core when a submission stops making progress (on by default) |
| `gpu_flush=0` | write the fence with a plain memory write instead of waiting for a cache flush |
| `glbin=1` | let freedreno use binning (GMEM) again; the default is `FD_MESA_DEBUG=sysmem` |
| `glquiet=1` | no five-second GPU report, so the end of the log is left to other messages |
| `procexit=1` | one line for every process that exits with an error, and for the signals that kill it |
| `adsp=1` | the audio DSP, which is where the battery is read on this phone (`adsp_memory=0x86100000` on mars); the small image needs `ADSP=1 userland/build.sh` |
| `pmic=1` / `thermal=1` / `buttons=1` | the peripheral drivers |

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
motion, `0x33` leave -- and the id is the **high nibble** of the first byte, so during a drag the
panel keeps sending enters and that is normal; the low nibble carries the touch count.
`x = ((e[3] & 0x0f) << 8) | e[2]`, `y = (e[4] << 4) | ((e[3] & 0xf0) >> 4)`.
The FIFO has to be drained in bursts (`FTS_CMD_READONE_FIFO 0x87` + dummy + n x 8 bytes); one event
per transfer leaves the queue standing still.

## Known issues

- **The identity has to be built in.** Without `xnuloader/mkidentity.sh` the phone is taken for a
  home-button model: no home indicator, and a swipe up from the bottom opens Control Center. The
  touch pipeline itself was never at fault -- the panel reports one finger from the bottom edge up
  the screen, and the driver posts it sample by sample.
- **The clock is moved out from under the camera.** iOS lays the status bar out for a notch in the
  middle of the top edge, so the clock sits in the left flank about 27 pt in; this phone's front
  camera is a round hole in that same place and the clock was drawn underneath it. The identity
  says `thin-bezel`, which is a notch, and nothing in the system describes a hole off to the side,
  so `userland/src/inject_sb.c` installs `-[_UIStatusBar layoutSubviews]` and moves the string views
  in that left flank 34 pt to the right after the bar has laid itself out.
- **freedreno's binning path hangs the 3D core.** With GMEM rendering on, a draw never retires and
  the core stops (the CP sits in `CP_WAIT_FOR_IDLE`, the fence is never written); the driver then
  resets it, which shows up as 1-2 fps. The default is therefore sysmem (`FD_MESA_DEBUG=sysmem`,
  `glbin=1` asks for the old path back), which gives 60 fps.
- **CoreAnimation draws at half the rate it is told**, so the framebuffer driver reports 120 Hz by
  default (`fb_ca_hz`, `xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp`).
- **The battery is read from the audio DSP, not from the loader.** The loader's shared page needs UEFI
  charger protocols this boot chain does not have, so `battery=1` reads nothing; with `adsp=1` the DSP's
  battery manager answers over `PMIC_RTR_ADSP_APPS` (`adsp: battmgr: the DSP says: capacity ... uV, uA,
  temperature ...`) and the percentage follows it.
- **The buttons are seen by the driver but do nothing yet.** Presses and releases (with their
  duration) are logged, and userland turns them into HID consumer events, but neither the lock nor
  the volume keys have an effect on the system.
- **A few pages of Settings exit at once**: wallpaper, Emergency SOS, software update. Wallpaper is
  killed by RunningBoard (`reason namespace 15`) while the poster extension never starts; the SOS
  page draws coloured boxes. Both are tracked in the project's problem list.
- **A run usually ends with the loader wedged in fastboot** (`fastboot devices` shows
  `????????????`); a 15 s power-button hold recovers it.
- **The log comes over the cable, and only the newest 192 KB of it.** The kernel writes into a
  ring; `oem log <offset>` hands back a window and `xl` pages through it. One 192 KB fetch is all the
  loader's USB command channel will take -- after that `fastboot` stops answering while `getvar`
  still does, so ask for the window once at the end (`XL_FOLLOW=` and a long `XL_SECS`) rather than
  polling every few seconds.

## Wi-Fi and Bluetooth

Both halves of the WCN6750 are built into the kernel with `configs/net.config`
(`CONFIG_HAKIOS_WIFI=y`, `CONFIG_HAKIOS_BT=y`, `CONFIG_HAKIOS_QMI=y`): ath11k from Linux
(`tools/mk-ath11k.sh`, 509 KB of object from 18 files, no stubs) on the LinuxKPI layer in
`wifi/lx`, and the Bluetooth file, which so far only reads its three pins (`bt=1`).

What they lack is the firmware, which is not in the tree and cannot be read off the phone as the
`shell` user (`/vendor/firmware` is denied by SELinux). The files the drivers ask for:

- Wi-Fi: `wpss.mdt` with its `wpss.b00`..`wpss.b07` segments (the DSP image), `bd_m20gl.elf`,
  `board-2.bin`, `board.bin`, `regdb_xiaomi.bin` -- into `fw-wifi/` beside the repository.
- Bluetooth: the `msbtfw*.mbn` rampatch and `msnv*.bin` NVM files of the same chip.

None of these may be committed (they are the vendor's). With them in place, `wifi=1` starts the
DSP, and `bt=1`/`bt=2` walk the Bluetooth half one step at a time.

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

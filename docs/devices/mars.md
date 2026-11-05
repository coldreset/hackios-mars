<!-- Made by tools/docs-gen.py from devices/mars/device.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# Xiaomi 11 Pro (`mars`)

| | |
|---|---|
| Vendor | Xiaomi |
| SoC | `lahaina` |
| Configuration | `make mars_defconfig` |
| Maintainer | the mars port (README.md) |
| Repository | [`devices/mars/`](https://github.com/oosexgang/hakios-xnu-kernel/tree/main/devices/mars) |

## What works

| Part | State | |
|---|---|---|
| display | works | the panel through UEFI's framebuffer; frames drawn by the GPU with gpu=1 |
| touch | works | ST FingerTip FTM5 on SPI: taps and drags (CONFIG_HAKIOS_FOCALTECH, touchscreen=1); home-swipe classification still rough |
| cpu | works | all 8 cores (cpus=8); A78/X1 at their top OPP with cpufreq=1 |
| gpu | works | Adreno 660, all 21 bring-up steps; OpenGL ES through Mesa freedreno (gpu=1 adreno_nomeasure=1) |
| storage | partly | the system image is resident in RAM; var_store / UFS writes not enabled |
| buttons | no | the driver is in the tree, off in this device's config |
| battery | no | the driver is in the tree, off in this device's config |
| thermal | no | the driver is in the tree, off in this device's config |
| wifi | no | the driver is in the tree, off in this device's config |
| audio | no | the driver is in the tree, off in this device's config |
| camera | no | the driver is in the tree, off in this device's config |

The state is what has been seen on this device, written down in `devices/mars/device.conf`; a part the table does not name has not been looked at.

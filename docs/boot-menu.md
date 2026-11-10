# Boot menu: where it stands, and how to drive it by the phone's own keys

The menu on the boot screen is not a picture any more. It moves, it is chosen,
and the choice does something (`b435c27`, verified on the device by reading the
framebuffer back over USB).

What is missing is the last step: the physical keys. This note is the handoff
for that step, so it does not have to be rediscovered.

## What the menu does today

Host commands, handled in `xnuloader/loader.c` in the `oem menu` branch of the
command loop:

| command | effect |
|---|---|
| `fastboot oem menu up` / `down` | moves `splash_choice` through 3 items and redraws |
| `oem menu select` on item 1 (`诊断`) | prints `diagnostics: display WxH at base, stride, log N bytes, stage P` |
| `oem menu select` on item 2 (`重启到安卓`) | `EfiResetCold` after 0.4 s |
| `oem menu select` on item 0 (`启动 HackiOS`) | back to `等待主机连接` |

Drawing is `splash_draw()` → `splash_menu()`; the state is `static int splash_choice`
(declared next to the `splash_status` prototype so the command loop can see it).
Status text must only use glyphs present in `xnuloader/cjk.h` — anything else
renders as gaps.

## Why the keys need work

`xnuloader/loader.c` has no SPMI and no PMIC code. This u-boot build has
`drivers/spmi/spmi-msm.c` in the tree but no SPMI options in
`u-boot/build-mars/.config`, no `drivers/pmic` content, and no `pm8xxx`/`qpnp`
driver at all. The loader is a UEFI application on top of u-boot, so it cannot
reach the controller through u-boot either.

The protocol, however, is already solved inside the kernel:

| anchor | what it is |
|---|---|
| `drivers/input/hakios.buttons.c:127` | all transfers serialise through `hakios_spmi_lock()` / `hakios_spmi_unlock()` (defined in `drivers/core/hakios.platform.c`) |
| `drivers/input/hakios.buttons.c:135` | `static struct peripheral pon = { .name = "the PMK8350's power-on block", .id = 0x013 };` — registers `0x1300` of slave 0 |
| `drivers/input/hakios.buttons.c:148` | `spmi_read(struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count)` — the function to port |
| `drivers/input/hakios.buttons.c:320` | power key: `REG_RT_STATUS` of the PON block |
| `drivers/input/hakios.buttons.c:330` | volume keys: same register of the PM7325 GPIO block |
| `drivers/input/hakios.buttons.c:367` | `pon_inputs()` reads `0x1310`–`0x1319` (status + interrupt setup) |

The device tree node is `qcom,spmi@c440000`; on mars the PON block is the
PMK8350 and the volume keys hang off the PM7325's GPIOs.

## The order to do it in

1. Add `spmi_read()` to `xnuloader/loader.c`, copying `hakios.buttons.c:148`;
   replace the kernel's lock with a poll of the controller's busy bit, since the
   loader is single-threaded and there is nothing to serialise against.
2. Add `keys_poll()` returning none / up / down / confirm; read the two
   `REG_RT_STATUS` registers every 50 ms and debounce the same way the kernel
   does.
3. Call `keys_poll()` from the wait loop and feed the result into the existing
   `splash_choice` machinery — no new drawing code is needed.
4. Keep `oem menu` as the host fallback.

## How to test it cheaply

The menu lives in the loader, so a round costs `./xl reload` (about 20 s) and a
framebuffer grab — **no 2.3 GB upload**:

```
./xl reload
fastboot oem menu down          # or press the key
fastboot oem screenshot
fastboot get_staged /tmp/s.bin  # write to /tmp, copy to Windows afterwards
```

`get_staged` given a Windows path does not work from WSL: the fastboot here is a
wrapper around `fastboot.exe`, so stage to `/tmp` and copy from there.

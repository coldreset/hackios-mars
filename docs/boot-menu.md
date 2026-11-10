# Boot menu: the phone's own keys drive it

The menu on the boot screen is real: it moves, it is chosen, the choice acts, and
since 11 November 2026 it answers the phone's own keys — volume up and volume down
move the highlight, power chooses. The host keeps its commands as a fallback.

All of it is in `xnuloader/loader.c`, in the loader's command loop, and a round of
testing costs `./xl reload` (about 20 s) plus a framebuffer grab — **no kernel
upload**.

## What drives the menu

| input | effect |
|---|---|
| volume up / `fastboot oem menu up` | `splash_choice = (splash_choice + 2) % 3`, redraw |
| volume down / `oem menu down` | `splash_choice = (splash_choice + 1) % 3`, redraw |
| power / `oem menu select` on item 0 (`启动 HackiOS`) | status back to `等待主机连接` |
| `oem menu select` on item 1 (`诊断`) | prints `diagnostics: display WxH at base, stride, log N bytes, stage P` and shows `诊断` |
| `oem menu select` on item 2 (`重启到安卓`) | `EfiResetCold` after 0.4 s |

Both paths end in the same two places: `splash_choice` for movement, and
`menu_select()` (`xnuloader/loader.c:4174`) for the choice, so a key and a host
command cannot drift apart. Drawing is `splash_draw()` → `splash_menu()`; status
text must only use glyphs present in `xnuloader/cjk.h` — anything else renders as
gaps.

Each press is also written to the loader's log as `keys: volume up`,
`keys: volume down` or `keys: power`, so a press can be verified from the host with
`./xl log` even when the screen is not being watched.

## Where the code is

| anchor | what it is |
|---|---|
| `xnuloader/loader.c:3924` | the whole keys block: arbiter, `spmi_read`, debounce, polling |
| `xnuloader/loader.c:4009` | `spmi_read(channel, reg, bytes, count)` — one SPMI register read |
| `xnuloader/loader.c:4043` | `arb_channel(id, channels)` — which arbiter channel a PMIC peripheral is on |
| `xnuloader/loader.c:4084` | `key_identify()` — the peripheral's type/subtype must be what the keys are |
| `xnuloader/loader.c:4106` | `keys_start()` — once, after the splash, before the payload is parsed |
| `xnuloader/loader.c:4197` | `key_look()` — the debounce |
| `xnuloader/loader.c:4221` | `keys_poll()` — one look at both PMIC peripherals |
| `xnuloader/loader.c:4256` | `keys_service()` — at most 20 looks a second, then the menu acts |
| `xnuloader/loader.c:4324` | the call in `usb_wait()`, guarded by `keys_live` |
| `xnuloader/loader.c:5038`, `:5050` | `keys_live` is set only around the waits for a command |
| `xnuloader/loader.c:5698` | `keys_start()` in `efi_main` |

The mechanism is the kernel's, cut down: `drivers/input/hakios.buttons.c`.

## How the reads work

Neither key is a pin of the SoC. Both are inputs of PMICs on the SPMI bus, and the
way to that bus is the SoC's PMIC arbiter at `qcom,spmi@c440000`:

- `0x0c440000` — the arbiter's own registers: version at `+0x000` (`0x50020000` on
  mars), channel count at `+0x004` (`512`), and for channel *n*, `+0x900 + 4n` says
  which slave and peripheral it is ([19:8]).
- `0x0c40a000` — which execution environment may write through each channel;
  `+0x700 + 4n` ([2:0]).
- `0x0e600000` + `0x10000 × 0` + `0x80 × channel` — this system's observer block:
  command at `+0x00`, status at `+0x08`, the data read at `+0x18`/`+0x1c`.

A read is the command word `(1 << 27) | reg << 4 | (count - 1)` written to the
command register, then a poll of the status register until bit 0 (`DONE`) is set,
then the two data words, deepest byte first. `DENIED` (bit 2), `FAILURE` (bit 1) and
`DROPPED` (bit 3) are reported as failures, not as key states.

The keys:

| key | peripheral | register | bit |
|---|---|---|---|
| power | PMK8350 power-on block, slave 0 peripheral `0x13` (channel 83 on mars) | `0x10` (`REG_RT_STATUS`) | `0x80` (`KPDPWR`), set while held |
| volume down | same block | `0x10` | `0x40` (`RESIN`), set while held |
| volume up | PM7325 GPIO6, slave 1 peripheral `0x8d` (channel 117 on mars) | `0x10` | `0x01` (`LEVEL`), **clear** while held |

Only reads are issued, at init and then while the loader waits. The PMICs make the
phone's supplies, and the firmware leaves these inputs as the keys need them; the
loader does not write to them.

## Rules it follows (each one learned the hard way in the kernel)

- **20 looks a second**, not 50: a hand is not faster, and every look is two bus
  transactions. `freq / 20` of `CNTVCT` is the interval.
- **A press needs two identical looks in a row** (`key_look()`), and is acted on
  when it becomes pressed, not while it stays pressed.
- **A key held from boot is not believed.** If a key reads as held when
  `keys_start()` runs, it is ignored until it has been seen released, and the
  release is logged (`keys: … is released now: it works from here on`). This
  matters when the phone is started with a finger already on volume down to reach
  fastboot.
- **2 ms per read, then the read is a failure**, not a hang: a failed read leaves
  the other key working; more than a hundred failures and the keys are given up for
  the session with a line in the log, rather than a menu that stutters.
- **Keys are watched only while the loader waits for a command** — the idle
  `usb_rx` and the wait for the cable. Not while a command runs, so a key pressed
  half-way through a download cannot reset the phone under the host.
- **Not while a kernel runs** (`run_active`): the kernel is on another core, its own
  buttons driver owns the arbiter, the loader has no lock to share with it, and the
  screen belongs to the kernel.
- **No lock at all**: the loader is one thread and nothing else uses the bus while
  it waits for the host.
- `keys_start()` returns silently on QEMU (`PLAT` is neither `redwood` nor `mars`),
  where there is no arbiter to read.

## How to test it

```
./xl reload                     # ~20-25 s, ends with the menu on the screen
./xl log                        # "keys: PMIC arbiter version …" and the two channels
# press volume down, then volume up, then power
./xl log                        # "keys: volume down", "keys: volume up", "keys: power"
./xl screenshot logs/menu.png   # the highlight follows the presses
fastboot oem menu down          # the host still works
```

The startup lines to expect on mars, from the device:

```
keys: PMIC arbiter version 50020000, 512 channels
keys: power volume up volume down on channels 83 and 117: volume up and down move the menu, power chooses
```

If one of the two peripherals is not found, or its type/subtype is not what the
keys are, the loader says which and carries on with whatever it can read; when
nothing can be read it says so and the menu stays on the host's commands. All of
these are one line each in the log, so a phone that behaves oddly leaves the
evidence behind.

`get_staged` given a Windows path does not work from WSL: the fastboot here is a
wrapper around `fastboot.exe`, so stage to `/tmp` and copy from there.

## The panel, the cache, and why a screenshot lied

For a while the boot screen was 花屏 on the phone while `xl screenshot` returned a
perfect menu. Both were true, and the reason is worth keeping:

The loader draws the menu into the framebuffer through the firmware's **cached**
mapping, and nothing wrote those lines back to DRAM. The display controller reads
DRAM, so the panel showed what ABL had left in the buffer with the few lines the
cache happened to evict scattered over it — a screen that looks like it never
loaded. A screenshot is read back through the very same cache, so it returned the
picture the loader thought it had. Correct in memory, garbage on glass.

`xnuloader/loader.c:5597` `splash_present()` now writes the whole framebuffer back
(`dc cvac` over `SCREEN.stride * SCREEN.height`) at the end of `splash_draw()`
(`:5605`) and of `splash_status()`, which is to say on every change of the screen:
the first draw, each redraw before a command, each key press, each status line.
18.4 MB of lines is a few milliseconds, and only when the screen changes.

Two lessons:

- **A screenshot is not evidence that the panel shows anything.** Until this was
  found, the menu had been "verified" that way: it had never actually been seen on
  the phone. Look at the phone.
- Any new code that writes pixels goes through `splash_draw()`/`splash_status()`,
  or has to call `splash_present()` itself. Nothing else in the loader touches the
  framebuffer.

What still paints over the splash is u-boot's own `printf` console (the
`[oosexgang] host: upload / sending N KiB / 20%` lines): the firmware writes the
same buffer during a fastboot transfer. The loader repaints at the top of the next
`usb_command_loop` iteration, so those lines flicker during a transfer and are gone
after it. That is the firmware's, not the menu's.

## What is verified

Verified on the device (11 November 2026), on mars: the arbiter answers (version
`0x50020000`, 512 channels), the PON block is on channel 83 and the PM7325 GPIO on
channel 117, both identify as what the keys are, all three keys read as usable, and
no read failed in minutes of 20 Hz polling.

Then a person pressed each key once. The log, in the order the presses were made:

```
keys: volume down
keys: volume up
keys: power
```

The highlight went to `诊断` on the first, back to `启动 HackiOS` on the second, and
the third left the status at `等待主机连接` — the three lines say the menu followed
all three keys. The host's `oem menu up|down|select` was still checked in the same
session: it moves and selects as before.

The screen itself was looked at, twice: 花屏 before `splash_present()` existed, the
menu — ring, title, three items, hint, status — after it, and still correct after
two redraws driven from the host.

The ring's arc (`splash_progress`, 130° when the loader starts) is decorative: no
progress is fed into it. The `download:` handler (`xnuloader/loader.c:4453`) has
`total` and `got` in its receive loop (`:4485`–`:4502`), so a live arc would be a
few lines there — draw the new arc segment and present just the ring's bounding box,
because a full-screen write-back per update (18 MB) is not affordable at transfer
speed.

# The screen draws and does not listen

On 2026-11-11 the HackiOS desktop on mars comes up in Chinese, with the wallpaper, and it
is a photograph: no swipe, no tap, and the power button does nothing. This is where the
investigation stands; it was stopped on purpose, not finished.

## What the log said while it could still be read

From `logs/050641-run.log` (05:06, the last run whose log came back whole):

- `hakios: no GPU to draw with: CoreAnimation keeps its software renderer` — from
  `userland/src/inject.c:1063-1070`, where `IOServiceGetMatchingService(0,
  IOServiceMatching("HakiosGPU"))` answers 0: the kernel's GPU driver published nothing.
  The display then runs at `MiniIOMobileFramebuffer: 51 frames in 5004 ms (10 a second)`.
- SpringBoard tells backboardd the screen is off, twice, and never says otherwise again:
  `reason: SB + boot; digitizerEnabled: NO` (score 4108 against backboardd's baseline 1) at
  05:07:35.852, then `reason: SB + backlight-off; digitizerEnabled: NO; displayState: off`
  at 05:07:35.855. Nine and a half minutes later there is still no `displayState: on`.
- backboardd obeys: `send mode (wakeOnTouch/0xFE) to digitizer service 101A082200001`, and
  at 05:07:36.046 `[TouchEvents] create touch stream <BKDigitizerTouchStreamClient: … pid:
  7(vD); SpringBoard>`. The digitizer service exists and is addressed; it is switched off.

Across the day: `logs/023709-run.log` (02:37) and `logs/031748-run.log` (03:17) have **no**
`displayState: off` line; `logs/034809-run.log` (03:48) and `logs/050641-run.log` (05:06)
have two each. Every log of 11 November carries `no GPU to draw with` once; the logs of
4 November (`141031`, `114910`, `114338`, `112850`, `112141`, `111059`) carry it not at
all. So the GPU service went missing during 11 November, before the touch did.

## Three runs, side by side — and why "the backlight" is not the whole story

| run | `displayState: off` | our virtual touch service made | `create touch stream` | touch |
|---|---|---|---|---|
| `logs/031748-run.log` 03:17 | none | yes (`hakios: input: touch screen service`) | none logged | worked |
| `logs/034809-run.log` 03:48 | two | **no** — the input thread never got that far | yes | dead |
| `logs/050641-run.log` 05:06 | two | no | yes | dead |
| `logs/060439-run.log` 06:04 (with the backlight hooks) | **none** | yes | none | dead |

The 06:04 run settles it: with `-[SBBacklightController screenIsOn]` answering `YES`, the
`displayState: off` publication disappears — and the phone still takes nothing. So the
digitizer state is not sufficient either. What is left is at least two failure modes: the
injection's input thread sometimes never creates the virtual touch service (03:48, 05:06),
and when it does (06:04) no touch reaches anyone. Both are consistent with a startup race:
boot got slower when the interface became Chinese and the renderer fell back to software,
and backboardd now scans for a digitizer before our service exists.

## What was tried, and did not help

1. **The two probe jobs.** `com.hakios.languagetest.plist` and `com.hakios.fonttest.plist`
   were removed from the boot volume and a round run: no change. They are back.
2. **SpringBoard's opinion of the backlight.** Hooks on `-[SBBacklightController screenIsOn]`
   (answer `YES`) and `-[SBBacklightController backlightLevel]` (answer `1.0`), which is the
   only state SpringBoard can be reading, since nothing in this tree ever sets a backlight
   (`grep -rli backlight drivers include userland xnuloader` finds nothing). The publication
   went away, the touch did not come back. Reverted.

## The next experiment — one change, one round

`userland/src/inject.c:930` sleeps three seconds before it creates the virtual HID services:

```c
    sleep(3);   /* let backboardd get as far as starting its event server */
```

Change it to `sleep(1)`, rebuild the boot volume only (`cd userland && GL=1 ./build.sh`,
about a minute), and run one round. A person has to look at the phone: a screenshot during
a run is impossible, and the log is only a witness (did `touch screen service` appear, did
`create touch stream`, was there a `displayState` publication) — not the verdict.

If that fails, in order:

1. The other direction: turn `brightness_fixes()` back on (`userland/src/inject.c:569`,
   which installs `wait_for_als_arrival`, `:560`); `:1226` records why it was switched off —
   "with it backboardd no longer took our touch" — so read that comment before believing it.
2. Find out whether the Chinese work is the cause at all: go back to `9c83878` (no Chinese,
   no wallpaper, touch known good), rebuild the system image (~10 minutes) and run a round.

## The blocker that is behind all of this

**The loader's log cannot be read reliably.** Between 05:16 and 06:04, `oem log <offset>`
answered pieces of the right *length* whose *content* was all zeros, so `./xl log` and the
log `xl run` saves at the end both came back 0 bytes; the 06:14 round was readable again.
Two rounds in that window left no evidence at all.

The shape is familiar: the kernel writes the log at a physical address (`kernel returned:
log at f7ae89c0, …`) and the loader reads it back through its own cached mapping. That is
the same class of mistake as the framebuffer, which was invisible for the same reason
(`docs/boot-menu.md`, "The panel, the cache, and why a screenshot lied"). Worth fixing on
its own: a round that answers nothing is the most expensive kind.

## What still works, and is committed

`e31ad40` — the interface speaks Chinese, the home screen has its wallpaper, the boot menu
answers the volume and power keys and its first item starts HackiOS, the ring on the boot
screen is the transfer. None of that is in the phone: the image is uploaded into RAM each
round and the phone's own storage is untouched.

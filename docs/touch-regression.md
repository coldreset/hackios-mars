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

## What was tried, and did not help

1. **The two probe jobs.** `com.hakios.languagetest.plist` and `com.hakios.fonttest.plist`
   were removed from the boot volume and a round run: no change. They are back.
2. **SpringBoard's opinion of the backlight.** Hooks on `-[SBBacklightController screenIsOn]`
   (answer `YES`) and `-[SBBacklightController backlightLevel]` (answer `1.0`), which is the
   only state SpringBoard can be reading, since nothing in this tree ever sets a backlight
   (`grep -rli backlight drivers include userland xnuloader` finds nothing). A round with
   those hooks in: the phone still would not take a touch. Reverted.

## The blocker, before anything else

**The loader's log cannot be read any more.** From about 05:16 on, `oem log <offset>`
answers pieces of the right *length* whose *content* is all zeros, so `./xl log` and the
log that `xl run` saves at the end both come back 0 bytes. Every fact above came from a log
fetched before that moment; the two rounds after it left no evidence at all.

This is worth fixing as its own bug, and the shape of it is familiar: the kernel writes the
log at a physical address (`kernel returned: log at f7ae89c0, …` in the loader's own output)
and the loader reads it back through its own cached mapping. That is the same class of
mistake as the framebuffer, which was invisible for the same reason (`docs/boot-menu.md`,
"The panel, the cache, and why a screenshot lied"): the length is right because it comes
from the kernel's return value, and the bytes are zeros because nobody cleaned the lines.

## What to do next, in order

1. Make the log readable again — clean or invalidate the loader's view of the returned
   region before `oem log` serves it, or read the same boot over a serial console and
   compare. Do not spend another phone round on touch until this works: without it a round
   answers nothing.
2. With a readable log, find out why `HakiosGPU` stopped being published. The driver source
   did not change (`git log -- drivers` is quiet), so the difference is in what the kernel
   does at boot: compare the boot arguments and the firmware path
   (`/redwood/fw/a660_zap.b00/.b01/.b02/.mdt/.elf` are all in the image).
3. If the GPU is innocent, take the digitizer race seriously. `userland/src/inject.c:930`
   sleeps three seconds before it creates the virtual HID services, and
   `wait_for_als_arrival()` (`:559-566`, installed by `brightness_fixes()`) is the only
   thing that ever made backboardd wait for them; it is switched off, and `:1226` says why:
   "brightness_fixes() is not used for now: with it backboardd no longer took our touch".
   One round with the wait re-enabled and one with `sleep(0)` would tell them apart.

## What still works, and is committed

`e31ad40` — the interface speaks Chinese, the home screen has its wallpaper, the boot menu
answers the volume and power keys and its first item starts HackiOS, the ring on the boot
screen is the transfer. None of that is in the phone: the image is uploaded into RAM each
round and the phone's own storage is untouched.

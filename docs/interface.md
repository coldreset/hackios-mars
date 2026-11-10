# The interface: its language, its wallpaper, its cards

Three things that looked like one: the interface speaks English, the home screen has
no wallpaper, and the switcher's cards are empty. Each had its own cause, and only
one of them was where the handoff said it was.

## The language: the strings were here all along

`select-system.py` keeps five `.lproj` directories (`KEEP_LANG`, `sysimage/select-system.py:96`)
and drops the other two hundred, so the image has no `zh-Hans.lproj` — and the handoff
concluded that the Chinese resources had to come from an IPSW. They did not. Every
bundle's strings live in a `.loctable` file, one file per table holding **all** its
languages: `SpringBoard.loctable` (1 117 760 bytes) carries 43 of them, keyed by name,
`zh_CN` among them, with the real translations (`LOW_BATT_TITLE` → `电池电量不足`). The
`.lproj` directories Apple ships beside those tables are empty; they are markers, and
CoreFoundation only looks a language up if one is there.

`userland/mklocalizations.py` makes them (`zh-Hans.lproj`, `zh_CN.lproj` beside every
directory that holds a `.loctable`: 1621 directories, 3242 markers).

That alone changed nothing, and the reason is worth writing down, because it is not
what the documentation suggests. Measured from inside the running system with
`userland/src/language_test.m`:

- `NSLocale preferredLanguages` is `zh-Hans,en`; `AppleLocale` is `zh_CN`;
  `.GlobalPreferences.plist` in `/private/var/mobile/Library/Preferences` says
  `AppleLanguages = (zh-Hans, en)`. `init.c`'s `set_language()` writes it every boot
  and it is right.
- `[NSBundle preferredLocalizationsFromArray:(zh-Hans, en)]` answers `zh-Hans`.
- but `[NSBundle preferredLocalizationsFromArray:<the bundle's own localizations>]`
  answers **`en`**, and `LOW_BATT_TITLE` comes back `Low Battery`.

So the bundle is asked for its language without the preferences that say Chinese, and
it answers with its `CFBundleDevelopmentRegion`. Hooking `+[NSBundle
preferredLocalizationsFromArray:]`, `+…forPreferences:` and `-[NSBundle
preferredLocalizations]` in `libhakios.sb.dylib` never saw a call: SpringBoard's
localisation lookup is CoreFoundation's own, not those Objective-C methods. Rewriting
the preference plists changed nothing either (measured).

What works is answering at the level that is actually used: `mklocalizations.py` writes
the `zh_CN` strings **into each table's `en` and `Base` entries** (2884 tables, 188 184
entries). The lookup then finds Chinese in the language it does ask for. The `zh_CN`
entries stay where they are, and so do the markers, which is what makes an explicit
`zh-Hans-CN` preference work as well.

## The font: a hollow box for every word

With the strings in Chinese the interface drew a white box for every character. The
cause is one line in a boot log: `Requested font not available from
file:///System/Library/Fonts/LanguageSupport/PingFang.ttc#postscript-name=.PingFangSC-Medium
… fallback to LastResort`. LastResort is the font that has a hollow rectangle for every
character it is asked for — which is exactly what the screen showed.

`select-system.py` trims PingFang to what fits in 25 MB (`MAX_FONT`, `TRIMMED_FONTS`)
and keeps the Regular and Semibold faces; the Medium weight, which is what the interface
asks for, is not in the file. Two ways to put it back do not work, and both were tried:

- copying an existing face and renaming its `name` table: the lookup still fails;
- the same, renaming the CFF `fontNames` and `FullName` too: the CFF compiler turns the
  copy into a font of **seven glyphs**, which CoreText reports faithfully and draws
  nothing with.

`tools/cjkface.py` adds the face as **TrueType**, from another font's outlines, and
leaves the collection's CFF faces untouched:

    tools/cjkface.py sysimage/rootfs/System/Library/Fonts/LanguageSupport/PingFang.ttc MiSansVF.ttf

The source this tree was built with is `MiSansVF.ttf` from the phone's own Android ROM
(`super.img` → `system_a.img`, erofs → `system/fonts/`), instantiated at `wght=500`
(29 108 glyphs), renamed to `PingFangSC-Medium` and `.PingFangSC-Medium`, and appended:
14 faces, 35 709 332 bytes. After that, `Requested font not available` is a line a boot
never prints, and `CTFontCreateWithName(CFSTR("PingFangSC-Medium"))` answers a font of
29 108 glyphs.

The build does it itself when `CJK_FONT` names the font to take outlines from, and
otherwise says once that the interface's Chinese will be boxes:

    CJK_FONT=/opt/hackios/rom/MiSansVF.ttf ./userland/build.sh

`sysimage/` is not in the repository (`.gitignore`), so the face is a build input, like
the GPU firmware.

Dot-prefixed names (`.PingFangSC-Medium`) do not resolve through `CTFontCreateWithName`
at all — asking for one answers TimesNewRoman — so they cannot be tested that way. The
system's own list of faces (`CTFontManagerCopyAvailablePostScriptNames`, 282 faces, 18
of them PingFang) advertises `PingFangSC-Medium` before the file has it: the list comes
from the system's manifest, and the file is what has to answer.

## The wallpaper: the poster job was never in the image

SpringBoard looked the wallpaper up as `com.apple.posterboardservices.services` and got
`error = 3: No such process`, so `RunningBoard` killed it and the home screen had
nothing behind the icons. Apple ships that job as a **launch angel**,
`System/Library/LaunchAngels/com.apple.PosterBoard.plist`, and `mkjobs.py` only takes
the angels with `REST=1`. The hand-made `com.hakios.posterboard.plist` started the same
application but claimed no Mach service, so it could not answer the lookup.

`userland/build.sh` now copies Apple's own plist into `root/LaunchDaemons/`. The log
then shows the chain the name needed:

    (user/501/com.apple.PosterBoard) service will attempt to elide xpcproxy
    (pid/38/com.apple.WallpaperKit.CollectionsPoster [41]) service spawned with pid 41

`CollectionsPoster` — the process the handoff recorded as never launching — runs, and
the home screen has its wallpaper.

## The cards: still empty, and what is known

The switcher's cards are white frames. Two facts, both measured here:

- **No application snapshot is ever written.** `/var/mobile/Library/SplashBoard/Snapshots`
  and `/var/mobile/Library/Caches/com.apple.SpringBoard` do not exist after a boot.
  `+[XBApplicationSnapshotManifestImpl _outputFormatForSnapshot:]` (hooked in
  `inject_sb.c` to log the format) is never called, because nothing gets that far.
- **The switcher cannot be opened from a script.** A drag from the bottom edge does not
  reach Darwin's system gestures, and
  `-[SBMainSwitcherControllerCoordinator activateMainSwitcherNoninteractivelyWithSource:animated:]`
  answers that it took the request (every source tried) and changes nothing. So the
  cards can only be looked at by a person swiping up, which is why every experiment
  here needs a phone round and a pair of hands — QEMU cannot show them at all.

The wash of wallpaper the cards used to carry is still suppressed
(`switcher_wallpaper_overlay`, see the long comment at `userland/src/inject_sb.c:822`):
PaperBoardUI fills it from an image provider that has no picture of the wallpaper, and
what it hands over is 846 × 1827 pixels of (179, 77, 179) at 15 per cent opacity —
purple plates. `switcher_wallpaper=1` among the boot-args puts that back.

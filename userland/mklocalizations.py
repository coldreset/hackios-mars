#!/usr/bin/env python3
"""mklocalizations.py -- make the interface Chinese without the IPSW it would take to re-cut it.

Two things stand between this image and a Chinese SpringBoard, and neither is the translations:

1. A bundle advertises a language only by having a <language>.lproj directory. sysimage/select-system.py
   keeps KEEP_LANG = en, so the image has 290 .lproj directories and every one is en or Base. The
   strings themselves never left: the .loctable files carry 43 languages each (SpringBoard's has
   zh_CN with its 438 keys). The markers are made here, beside every .loctable: zh-Hans, the
   language src/init.c's set_language() asks for first, and zh_CN, the name the tables use for it.

2. That was not enough (a round with the markers in the image was still English), and
   src/language_test.m, run as a job of its own, found why: the lookup does not consult
   AppleLanguages when a bundle asks without naming preferences. Measured from inside the system,
   with the language list in the job's own domain, in the any-application domain, and written again
   through CFPreferences itself:

       [NSBundle preferredLocalizationsFromArray:(zh-Hans, en)]                 -> (zh-Hans)
       [NSBundle preferredLocalizationsFromArray:the bundle's localizations]    -> (en)

   i.e. the bundle is asked in English whatever the preference says, and the answer to "which
   language do I speak" is its CFBundleDevelopmentRegion, English in every Apple bundle here.
   (Hooking NSBundle's localisation methods inside SpringBoard was tried first; those methods are
   not on the path -- the lookup is CoreFoundation's own -- and the hook never ran.)

So the strings are put where the lookup does land: the language the bundle does choose (en, and
Base with it) is given the Chinese entries, key by key, with the English kept for anything not
translated. Nothing is lost: the other 41 languages stay in the table, and a later, proper
localisation (the IPSW, select-system.py's KEEP_LANG) makes this script unnecessary rather than
wrong -- it would find the entries already translated.

	python3 mklocalizations.py sysroot
"""
import os
import plistlib
import sys

LANGS = ("zh-Hans", "zh_CN")
SOURCE = "zh_CN"                # the name the .loctable files keep the Chinese under
TARGETS = ("en", "Base")        # what CoreFoundation asks for in this image (see above)


def main(root):
	markers = 0
	tables = 0
	files = 0
	entries = 0
	# no followlinks: sysroot's own links point into /redwood, the boot volume, which this does not touch
	for dirpath, dirnames, filenames in os.walk(root):
		if not any(name.endswith(".loctable") for name in filenames):
			continue
		tables += 1
		for lang in LANGS:
			path = os.path.join(dirpath, lang + ".lproj")
			if not os.path.isdir(path):
				os.mkdir(path)
				markers += 1
		for name in filenames:
			if not name.endswith(".loctable"):
				continue
			path = os.path.join(dirpath, name)
			try:
				with open(path, "rb") as f:
					table = plistlib.load(f)
			except Exception as error:
				print("mklocalizations.py: %s: %s" % (path, error))
				continue
			chinese = table.get(SOURCE) if isinstance(table, dict) else None
			if not isinstance(chinese, dict) or not chinese:
				continue
			before = len(chinese)
			changed = False
			# every name this bundle can be asked for now answers in Chinese
			for lang in LANGS:
				if table.get(lang) != chinese:
					table[lang] = chinese
					changed = True
			for lang in TARGETS:
				english = table.get(lang)
				if not isinstance(english, dict):
					continue
				merged = dict(english)
				merged.update(chinese)
				if merged != english:
					table[lang] = merged
					changed = True
			if changed:
				with open(path, "wb") as f:
					plistlib.dump(table, f, fmt=plistlib.FMT_BINARY, sort_keys=False)
				files += 1
				entries += before
	print("mklocalizations.py: %d directories hold a .loctable, %d %s markers made"
		% (tables, markers, " and ".join(lang + ".lproj" for lang in LANGS)))
	print("mklocalizations.py: %d tables given the Chinese (%d %s entries), %d files rewritten"
		% (files, entries, SOURCE, files))


if __name__ == "__main__":
	main(sys.argv[1] if len(sys.argv) > 1 else "sysroot")

#!/usr/bin/env python3
"""Give the image's PingFang collection the Medium face SpringBoard asks for.

The interface draws its Chinese with `.PingFangSC-Medium`, and sysimage/select-system.py's font
trim kept only the Regular and Semibold faces of PingFang (MAX_FONT, TRIMMED_FONTS), so the lookup
ends at LastResort, which draws a hollow box for every character: a Chinese interface that is
nothing but white frames. Measured inside the system, that one request is the only font complaint in
a whole boot.

This adds a Medium face to the collection, built from any font that has Chinese outlines (MiSans
from the phone's own Android ROM is what this tree was built with, 29 108 glyphs). It is added as a
TrueType face, not as a copy of one of the collection's CFF faces: a renamed copy comes out of the
CFF compiler with seven glyphs, which CoreText then reports as a font of seven glyphs and draws
nothing with.

    tools/cjkface.py PingFang.ttc MiSansVF.ttf      # add the face, in place
    tools/cjkface.py --check PingFang.ttc           # is it there? (for build.sh)

The build does this itself when CJK_FONT names the font to take the outlines from.
"""
import os
import sys

from fontTools.ttLib import TTCollection, TTFont
from fontTools.varLib import instancer

# the names CoreText looks the interface's Chinese up by: the public one the system's own list of
# faces advertises, and the private one SpringBoard asks for
FACES = (
	("PingFangSC-Medium", "PingFang SC", "Medium"),
	(".PingFangSC-Medium", ".PingFang SC", "Medium"),
)
WEIGHT = 500


def names(font, postscript, family, subfamily):
	for name_id, value in ((1, family), (2, subfamily), (3, postscript + ";2026"),
			       (4, family + " " + subfamily), (5, "Version 1.000"), (6, postscript),
			       (16, family.replace(" ", "")), (17, subfamily)):
		for platform, encoding, language in ((3, 1, 0x409), (1, 0, 0)):
			font["name"].setName(value, name_id, platform, encoding, language)


def present(collection_path):
	collection = TTCollection(collection_path)
	return [face["name"].getDebugName(6) for face in collection.fonts]


def check(collection_path):
	have = present(collection_path)
	missing = [postscript for postscript, _, _ in FACES if postscript not in have]
	if missing:
		print("cjkface.py: %s has no %s" % (os.path.basename(collection_path), ", ".join(missing)))
		return 1
	print("cjkface.py: %s has the Medium faces (%d faces)" % (os.path.basename(collection_path), len(have)))
	return 0


def add(collection_path, source_path):
	collection = TTCollection(collection_path)
	faces = list(collection.fonts)
	have = [face["name"].getDebugName(6) for face in faces]
	wanted = [postscript for postscript, _, _ in FACES if postscript not in have]
	if not wanted:
		print("cjkface.py: the Medium faces are already there")
		return 0

	source = TTFont(source_path)
	if "fvar" in source:
		source = instancer.instantiateVariableFont(source, {"wght": WEIGHT}, updateFontNames=False)
		if "fvar" in source:
			del source["fvar"]
	source["OS/2"].usWeightClass = WEIGHT
	first = os.path.join("/tmp", "cjkface-medium.ttf")
	source.save(first)
	glyphs = source["maxp"].numGlyphs

	for postscript, family, subfamily in FACES:
		if postscript not in wanted:
			continue
		face = TTFont(first)
		names(face, postscript, family, subfamily)
		faces.append(face)

	collection.fonts = faces
	# through a new file: the tree that is being built is link-copied (cp -al) from sysimage/rootfs, so
	# writing in place would change the build input as well as what gets packed
	written = collection_path + ".cjkface"
	collection.save(written)
	os.replace(written, collection_path)
	print("cjkface.py: %s now has %d faces, %d of them the Medium (%d glyphs from %s)"
	    % (os.path.basename(collection_path), len(faces), len(wanted), glyphs,
	       os.path.basename(source_path)))
	return 0


def main(argv):
	if len(argv) == 3 and argv[1] == "--check":
		return check(argv[2])
	if len(argv) == 3:
		return add(argv[1], argv[2])
	print(__doc__, file=sys.stderr)
	return 2


if __name__ == "__main__":
	sys.exit(main(sys.argv))

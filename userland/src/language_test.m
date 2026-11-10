// What Darwin here believes the interface language is, asked from a job that runs as the same user
// SpringBoard runs as (mobile), and in another process: the same question SpringBoard asks itself,
// but an answer that cannot take the home screen down with it (src/inject_sb.c's first attempt at
// this crashed SpringBoard before it printed a word).
//
// The question is worth asking because the image now holds zh-Hans.lproj and zh_CN.lproj markers
// (mklocalizations.py), init writes AppleLanguages (zh-Hans, en) into mobile's .GlobalPreferences,
// and the interface is still English. These lines say which of those the system actually accepted.

#import <Foundation/Foundation.h>
#import <CoreText/CoreText.h>
#import <stdio.h>
#import <unistd.h>
#import <stdarg.h>

static void say(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	fputs("language_test: ", stderr);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
	fflush(stderr);
}

static const char *text(NSString *string)
{
	return string ? string.UTF8String : "(nil)";
}

/*
 * The font question: the strings are Chinese now (mklocalizations.py), so what draws them? Asked
 * first, because it is the question the screen is waiting on: the interface shows white frames where
 * its words should be, which is what a glyph with nothing to draw it looks like.
 *
 * Asked of the font system rather than by drawing: rasterising a sample into a bitmap told us
 * nothing (it painted nothing even for "Hello"), while the name of the face a name resolves to, and
 * whether that face has a glyph for 中, say exactly which of the two is missing.
 */
static void
font_question(void)
{
	CTFontRef system = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 51.0, NULL);
	UniChar letter = 0x4E2D;                        /* 中 */
	CGGlyph glyph = 0;
	BOOL system_has_it = system != NULL && CTFontGetGlyphsForCharacters(system, &letter, &glyph, 1);
	CTFontRef medium = CTFontCreateWithName(CFSTR(".PingFangSC-Medium"), 51.0, NULL);
	CTFontRef regular = CTFontCreateWithName(CFSTR("PingFangSC-Regular"), 51.0, NULL);
	CTFontRef hidden = CTFontCreateWithName(CFSTR(".PingFangSC-Regular"), 51.0, NULL);
	CTFontRef public_medium = CTFontCreateWithName(CFSTR("PingFangSC-Medium"), 51.0, NULL);
	CFArrayRef names = CTFontManagerCopyAvailablePostScriptNames();
	CFIndex count = names ? CFArrayGetCount(names) : 0;
	int pings = 0;
	NSString *joined = @"";
	BOOL medium_listed = NO;

	for (CFIndex i = 0; i < count; i++) {
		NSString *name = (__bridge NSString *)CFArrayGetValueAtIndex(names, i);

		if ([name rangeOfString:@"PingFang"].location != NSNotFound) {
			pings++;
			joined = [joined stringByAppendingFormat:@"%@ ", name];
		}
		if ([name isEqualToString:@".PingFangSC-Medium"]) {
			medium_listed = YES;
		}
	}

	say("font: the system font is %s, it has a glyph for 中: %d",
	    text(system ? CFBridgingRelease(CTFontCopyPostScriptName(system)) : nil), system_has_it);
	say("font: .PingFangSC-Medium asks back %s (%lu glyphs)",
	    text(medium ? CFBridgingRelease(CTFontCopyPostScriptName(medium)) : nil),
	    (unsigned long)(medium ? CTFontGetGlyphCount(medium) : 0));
	say("font: PingFangSC-Regular asks back %s (%lu glyphs)",
	    text(regular ? CFBridgingRelease(CTFontCopyPostScriptName(regular)) : nil),
	    (unsigned long)(regular ? CTFontGetGlyphCount(regular) : 0));
	say("font: .PingFangSC-Regular asks back %s (%lu glyphs)",
	    text(hidden ? CFBridgingRelease(CTFontCopyPostScriptName(hidden)) : nil),
	    (unsigned long)(hidden ? CTFontGetGlyphCount(hidden) : 0));
	say("font: PingFangSC-Medium asks back %s (%lu glyphs)",
	    text(public_medium ? CFBridgingRelease(CTFontCopyPostScriptName(public_medium)) : nil),
	    (unsigned long)(public_medium ? CTFontGetGlyphCount(public_medium) : 0));
	say("font: the system lists %ld faces, %d PingFang, .PingFangSC-Medium among them: %d",
	    (long)count, pings, medium_listed);
	say("font: the PingFang it lists: %s", text(joined));
	if (medium != NULL) {
		CFRelease(medium);
	}
	if (regular != NULL) {
		CFRelease(regular);
	}
	if (system != NULL) {
		CFRelease(system);
	}
	if (names != NULL) {
		CFRelease(names);
	}
}

int main(void)
{
	@autoreleasepool {
		say("uid %d, HOME %s", getuid(), getenv("HOME") ? getenv("HOME") : "(unset)");
		font_question();

		NSArray *preferred = [NSLocale preferredLanguages];
		say("preferredLanguages %s", text([preferred componentsJoinedByString:@","]));
		say("currentLocale %s", text([[NSLocale currentLocale] localeIdentifier]));

		NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
		id languages = [defaults objectForKey:@"AppleLanguages"];
		say("defaults AppleLanguages %s", languages ? text([languages description]) : "(absent)");
		id nslanguages = [defaults objectForKey:@"NSLanguages"];
		say("defaults NSLanguages %s", nslanguages ? text([nslanguages description]) : "(absent)");
		id locale = [defaults objectForKey:@"AppleLocale"];
		say("defaults AppleLocale %s", locale ? text([locale description]) : "(absent)");

		NSString *path = @"/private/var/mobile/Library/Preferences/.GlobalPreferences.plist";
		NSDictionary *global = [NSDictionary dictionaryWithContentsOfFile:path];
		say("%s %s", text(path), global ? text([global description]) : "(unreadable)");

		NSString *root = @"/System/Library/CoreServices/SpringBoard.app";
		NSArray *files = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:root error:NULL];
		NSMutableArray *markers = [NSMutableArray array];
		for (NSString *file in files) {
			if ([[file pathExtension] isEqualToString:@"lproj"]) [markers addObject:file];
		}
		say("on disk %lu entries, .lproj: %s", (unsigned long)files.count,
		    text(markers.count ? [markers componentsJoinedByString:@","] : @"(none)"));

		NSBundle *board = [NSBundle bundleWithPath:root];
		if (board == nil) {
			say("SpringBoard.app: not a bundle");
			return 0;
		}
		NSArray *localizations = [board localizations];
		say("bundle localizations (%lu) %s", (unsigned long)localizations.count,
		    text([localizations componentsJoinedByString:@","]));
		say("bundle has zh-Hans %d, zh_CN %d, en %d",
		    [localizations containsObject:@"zh-Hans"], [localizations containsObject:@"zh_CN"],
		    [localizations containsObject:@"en"]);
		NSArray *chosen = [NSBundle preferredLocalizationsFromArray:localizations];
		say("preferredLocalizations %s", text(chosen ? [chosen componentsJoinedByString:@","] : nil));
		say("LOW_BATT_TITLE %s",
		    text([board localizedStringForKey:@"LOW_BATT_TITLE" value:@"(absent)" table:@"SpringBoard"]));
		say("CFBundleDevelopmentRegion %s", text([[[board infoDictionary] objectForKey:@"CFBundleDevelopmentRegion"] description]));

		// Which spellings of "Chinese" this system will actually match, asked of CoreFoundation
		// directly: the preference list init writes today is only one of the ways to say it.
		NSArray *spellings = @[
			@[@"zh-Hans", @"en"],
			@[@"zh-Hans-CN", @"en"],
			@[@"zh-Hans_CN", @"en"],
			@[@"zh_CN", @"en"],
			@[@"zh", @"en"],
			@[@"zh-Hant", @"en"],
			@[@"zh-Hant-TW", @"en"],
		];
		for (NSArray *spelling in spellings) {
			NSArray *picked = [NSBundle preferredLocalizationsFromArray:localizations forPreferences:spelling];
			say("preference %s -> %s", text([spelling componentsJoinedByString:@","]),
			    text(picked ? [picked componentsJoinedByString:@","] : nil));
		}

		// The bundle asks for its own preference list with a NULL array, and its answer above was
		// English while an explicit (zh-Hans, en) gets zh-Hans. So: where does each question look?
		NSArray *byDefault = [NSBundle preferredLocalizationsFromArray:localizations];
		say("preference (default) -> %s", text(byDefault ? [byDefault componentsJoinedByString:@","] : nil));
		id anyApplication = CFBridgingRelease(CFPreferencesCopyAppValue(CFSTR("AppleLanguages"), kCFPreferencesAnyApplication));
		say("CFPreferences any application AppleLanguages %s",
		    anyApplication ? text([anyApplication description]) : "(absent)");
		id currentApplication = CFBridgingRelease(CFPreferencesCopyAppValue(CFSTR("AppleLanguages"), kCFPreferencesCurrentApplication));
		say("CFPreferences current application (%s) AppleLanguages %s", [[[NSProcessInfo processInfo] processName] UTF8String],
		    currentApplication ? text([currentApplication description]) : "(absent)");
		say("CFLocaleCopyPreferredLanguages %s",
		    text([(NSArray *)CFBridgingRelease(CFLocaleCopyPreferredLanguages()) componentsJoinedByString:@","]));
		id springboard = CFBridgingRelease(CFPreferencesCopyAppValue(CFSTR("AppleLanguages"), CFSTR("com.apple.springboard")));
		say("CFPreferences com.apple.springboard AppleLanguages %s", springboard ? text([springboard description]) : "(absent)");

		// ...and does the answer change if the preference is written through CoreFoundation itself
		// (cfprefsd's own path, the way a running system writes it) instead of by hand into the file?
		NSArray *chinese = @[@"zh-Hans", @"en"];
		CFPreferencesSetValue(CFSTR("AppleLanguages"), (__bridge CFPropertyListRef)chinese, kCFPreferencesAnyApplication,
		    kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
		CFPreferencesSetValue(CFSTR("NSLanguages"), (__bridge CFPropertyListRef)chinese, kCFPreferencesAnyApplication,
		    kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
		BOOL synced = CFPreferencesAppSynchronize(kCFPreferencesAnyApplication);
		id reread = CFBridgingRelease(CFPreferencesCopyAppValue(CFSTR("AppleLanguages"), kCFPreferencesAnyApplication));
		say("after CFPreferencesSetValue: %s, re-read %s", synced ? "synchronised" : "NOT synchronised",
		    reread ? text([reread description]) : "(absent)");
		NSBundle *boardAgain = [NSBundle bundleWithPath:root];
		NSArray *afterWrite = [NSBundle preferredLocalizationsFromArray:[boardAgain localizations]];
		say("preference (default, after the write) -> %s", text(afterWrite ? [afterWrite componentsJoinedByString:@","] : nil));
		say("LOW_BATT_TITLE (after the write) %s",
		    text([boardAgain localizedStringForKey:@"LOW_BATT_TITLE" value:@"(absent)" table:@"SpringBoard"]));

		// Is it the domain that matters? Put the same pair in the application's own domain.
		CFPreferencesSetValue(CFSTR("AppleLanguages"), (__bridge CFPropertyListRef)chinese, kCFPreferencesCurrentApplication,
		    kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
		CFPreferencesSetValue(CFSTR("NSLanguages"), (__bridge CFPropertyListRef)chinese, kCFPreferencesCurrentApplication,
		    kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
		CFPreferencesAppSynchronize(kCFPreferencesCurrentApplication);
		NSBundle *boardThird = [NSBundle bundleWithPath:root];
		NSArray *afterOwn = [NSBundle preferredLocalizationsFromArray:[boardThird localizations]];
		say("preference (default, after the write to our own domain) -> %s",
		    text(afterOwn ? [afterOwn componentsJoinedByString:@","] : nil));
		say("preference (explicit zh-Hans-CN) -> %s",
		    text([[NSBundle preferredLocalizationsFromArray:[boardThird localizations]
						    forPreferences:@[@"zh-Hans-CN", @"en"]] componentsJoinedByString:@","]));
		say("LOW_BATT_TITLE (own domain) %s",
		    text([boardThird localizedStringForKey:@"LOW_BATT_TITLE" value:@"(absent)" table:@"SpringBoard"]));

		// And where the switcher keeps the pictures of the apps it cannot draw a card for.
		NSString *splash = @"/var/mobile/Library/SplashBoard";
		NSArray *snapshots = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:[splash stringByAppendingPathComponent:@"Snapshots"]
										  error:NULL];
		NSString *snapshotText = snapshots ? [NSString stringWithFormat:@"%lu entries", (unsigned long)snapshots.count]
						   : @"(not there)";
		say("%s/Snapshots %s", text(splash), text(snapshotText));
		NSArray *cache = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:@"/var/mobile/Library/Caches/com.apple.SpringBoard"
									    error:NULL];
		NSString *cacheText = cache ? [NSString stringWithFormat:@"%lu entries", (unsigned long)cache.count]
					    : @"(not there)";
		say("/var/mobile/Library/Caches/com.apple.SpringBoard %s", text(cacheText));


	}
	return 0;
}

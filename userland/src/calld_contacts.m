/*
 * calld's names: the caller's name from iOS's own contacts (Contacts.framework, served by contactsd -- the people
 * entered in the Contacts application), for the call's alerts. calld is signed for the address book
 * (src/calld.entitlements: com.apple.private.tcc.allow kTCCServiceAddressBook).
 *
 * Numbers are matched by their last 10 digits (a number dialled as 808..., +1808... and 1808... is the same one).
 */
#import <Contacts/Contacts.h>
#include <string.h>

#define NAMES "/private/var/mobile/Library/HakiOS/names.txt"

static NSString *
tail_digits(NSString *number)
{
	NSMutableString *d = [NSMutableString string];

	for (NSUInteger i = 0; i < number.length; i++) {
		unichar c = [number characterAtIndex:i];

		if (c >= '0' && c <= '9') {
			[d appendFormat:@"%C", c];
		}
	}
	return d.length > 10 ? [d substringFromIndex:d.length - 10] : d;
}

/* the name of the contact with this number into out; 0 if there is none (or no access to the contacts) */
int
contact_name(const char *number, char *out, size_t size)
{
	__block int found = 0, seen = 0;

	@autoreleasepool {
		NSString *want = tail_digits(@(number));
		CNContactStore *store = [CNContactStore new];
		NSArray *keys = @[ CNContactPhoneNumbersKey, CNContactOrganizationNameKey,
		    [CNContactFormatter descriptorForRequiredKeysForStyle:CNContactFormatterStyleFullName] ];
		CNContactFetchRequest *request = [[CNContactFetchRequest alloc] initWithKeysToFetch:keys];
		NSError *error = nil;

		if (want.length < 4) {
			return 0;
		}
		[store enumerateContactsWithFetchRequest:request error:&error usingBlock:^(CNContact *c, BOOL *stop) {
			seen++;
			for (CNLabeledValue<CNPhoneNumber *> *p in c.phoneNumbers) {
				if ([tail_digits(p.value.stringValue) isEqualToString:want]) {
					NSString *name = [CNContactFormatter stringFromContact:c style:CNContactFormatterStyleFullName];

					if (name.length == 0) {
						name = c.organizationName;
					}
					if (name.length != 0) {
						strlcpy(out, name.UTF8String, size);
						found = 1;
						*stop = YES;
					}
					return;
				}
			}
		}];
		if (error != nil && !found) {
			printf("calld: the contacts cannot be read: %s\n", error.localizedDescription.UTF8String);
		}
		if (!found) {
			printf("calld: %d contacts read, none with this number\n", seen);
			/* (Phone Pad's copy of the names, kept whenever it reads the contacts: NAMES, a 10-digit tail and a name a line) */
			NSString *text = [NSString stringWithContentsOfFile:@NAMES encoding:NSUTF8StringEncoding error:NULL];

			for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
				NSArray<NSString *> *f = [line componentsSeparatedByString:@"\t"];

				if (f.count == 2 && [f[0] isEqualToString:want] && f[1].length != 0) {
					strlcpy(out, f[1].UTF8String, size);
					found = 1;
					printf("calld: the name from Phone Pad's list (the contacts gave none)\n");
					break;
				}
			}
		}
	}
	return found;
}

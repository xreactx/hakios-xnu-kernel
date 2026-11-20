/*
 * Texts: the 6T's text messages (/Applications/Texts.app): the conversations, and new texts written and sent.
 *
 * Messages needs CommCenter and imagent, which speak Apple's baseband protocol and do not run here; the 6T's modem is
 * driven by the kernel (drivers/modem/hakios.mpss.c, QMI WMS). The conversations are a file: received texts are
 * written into it by smsd (src/smsd.c), sent ones by this application -- /var/mobile/Library/HakiOS/texts.txt, a line
 * each: seconds since 1970, in / out / failed, the number, the text (\\, \n and \t escaped). A text is sent by writing
 * "NUMBER\nTEXT" to kern.hakios.sms_send; kern.hakios.sms_send_state then says when it is sent (2) or not (3).
 * Conversations are told apart by the number's last 10 digits, and named from iOS's contacts.
 */
#import <UIKit/UIKit.h>
#import <Contacts/Contacts.h>
#import <ImageIO/ImageIO.h>
#include <sys/sysctl.h>

#define TEXTS @"/private/var/mobile/Library/HakiOS/texts.txt"
#define MMS_DIR @"/private/var/mobile/Library/HakiOS/mms"
#define OUTBOX @"/private/var/mobile/Library/HakiOS/outbox"
#define PHOTOS @"/private/var/mobile/Media/DCIM"

/* a picture made small enough to send (MMS: some 300 KB): its longer side 1280 pixels, JPEG, upright; NULL if not read */
static NSData *
picture_to_send(NSString *path)
{
	CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], NULL);
	NSDictionary *o = @{ (id)kCGImageSourceCreateThumbnailFromImageAlways: @YES,
		(id)kCGImageSourceCreateThumbnailWithTransform: @YES, (id)kCGImageSourceThumbnailMaxPixelSize: @1280 };
	CGImageRef small;
	NSData *jpeg = nil;

	if (source == NULL) {
		return nil;
	}
	small = CGImageSourceCreateThumbnailAtIndex(source, 0, (__bridge CFDictionaryRef)o);
	CFRelease(source);
	if (small != NULL) {
		for (CGFloat quality = 0.7; quality > 0.2; quality -= 0.15) {
			jpeg = UIImageJPEGRepresentation([UIImage imageWithCGImage:small], quality);
			if (jpeg.length < 300 * 1024) {
				break;
			}
		}
		CGImageRelease(small);
	}
	return jpeg;
}

/* a thumbnail of the picture at path, side pixels at most */
static UIImage *
thumbnail(NSString *path, CGFloat side)
{
	CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], NULL);
	NSDictionary *o = @{ (id)kCGImageSourceCreateThumbnailFromImageAlways: @YES,
		(id)kCGImageSourceCreateThumbnailWithTransform: @YES, (id)kCGImageSourceThumbnailMaxPixelSize: @(side) };
	CGImageRef small;
	UIImage *image = nil;

	if (source == NULL) {
		return nil;
	}
	small = CGImageSourceCreateThumbnailAtIndex(source, 0, (__bridge CFDictionaryRef)o);
	CFRelease(source);
	if (small != NULL) {
		image = [UIImage imageWithCGImage:small];
		CGImageRelease(small);
	}
	return image;
}

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

static NSString *
escaped(NSString *s)
{
	s = [s stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"];
	s = [s stringByReplacingOccurrencesOfString:@"\n" withString:@"\\n"];
	return [s stringByReplacingOccurrencesOfString:@"\t" withString:@"\\t"];
}

static NSString *
unescaped(NSString *s)
{
	NSMutableString *o = [NSMutableString string];

	for (NSUInteger i = 0; i < s.length; i++) {
		unichar c = [s characterAtIndex:i];

		if (c == '\\' && i + 1 < s.length) {
			unichar d = [s characterAtIndex:++i];

			[o appendString:d == 'n' ? @"\n" : d == 't' ? @"\t" : [NSString stringWithFormat:@"%C", d]];
		} else {
			[o appendFormat:@"%C", c];
		}
	}
	return o;
}

/* ---- the texts ---- */

@interface Text : NSObject
@property (nonatomic, strong) NSDate *when;
@property (nonatomic, copy) NSString *kind, *number, *body;
@end

@implementation Text
@end

@interface Store : NSObject
+ (NSArray<Text *> *)texts;
+ (void)add:(NSString *)kind number:(NSString *)number body:(NSString *)body;
+ (NSDictionary<NSString *, NSString *> *)names;
@end

@implementation Store

+ (NSArray<Text *> *)texts
{
	NSString *file = [NSString stringWithContentsOfFile:TEXTS encoding:NSUTF8StringEncoding error:NULL];
	NSMutableArray<Text *> *texts = [NSMutableArray array];

	for (NSString *line in [file componentsSeparatedByString:@"\n"]) {
		NSArray<NSString *> *f = [line componentsSeparatedByString:@"\t"];
		Text *t;

		if (f.count < 4) {
			continue;
		}
		t = [Text new];
		t.when = [NSDate dateWithTimeIntervalSince1970:f[0].doubleValue];
		t.kind = f[1];
		t.number = f[2];
		t.body = unescaped(f[3]);
		[texts addObject:t];
	}
	return texts;
}

+ (void)add:(NSString *)kind number:(NSString *)number body:(NSString *)body
{
	NSString *line = [NSString stringWithFormat:@"%ld\t%@\t%@\t%@\n", (long)NSDate.date.timeIntervalSince1970, kind, number,
	    escaped(body)];
	NSFileHandle *h;

	[[NSFileManager defaultManager] createDirectoryAtPath:TEXTS.stringByDeletingLastPathComponent
	    withIntermediateDirectories:YES attributes:nil error:NULL];
	if (![[NSFileManager defaultManager] fileExistsAtPath:TEXTS]) {
		[[NSFileManager defaultManager] createFileAtPath:TEXTS contents:nil attributes:@{ NSFilePosixPermissions: @0644 }];
	}
	h = [NSFileHandle fileHandleForWritingAtPath:TEXTS];
	[h seekToEndOfFile];
	[h writeData:[line dataUsingEncoding:NSUTF8StringEncoding]];
	[h closeFile];
}

+ (NSDictionary<NSString *, NSString *> *)names
{
	CNContactStore *store = [CNContactStore new];
	NSArray *keys = @[ CNContactPhoneNumbersKey, CNContactOrganizationNameKey,
	    [CNContactFormatter descriptorForRequiredKeysForStyle:CNContactFormatterStyleFullName] ];
	CNContactFetchRequest *request = [[CNContactFetchRequest alloc] initWithKeysToFetch:keys];
	NSMutableDictionary *names = [NSMutableDictionary dictionary];

	[store enumerateContactsWithFetchRequest:request error:NULL usingBlock:^(CNContact *c, BOOL *stop) {
		NSString *name = [CNContactFormatter stringFromContact:c style:CNContactFormatterStyleFullName];

		if (name.length == 0) {
			name = c.organizationName;
		}
		for (CNLabeledValue<CNPhoneNumber *> *n in c.phoneNumbers) {
			if (name.length != 0) {
				names[tail_digits(n.value.stringValue)] = name;
			}
		}
	}];
	return names;
}

@end

static NSString *
when_text(NSDate *when)
{
	NSDateFormatter *f = [NSDateFormatter new];

	if ([NSCalendar.currentCalendar isDateInToday:when]) {
		f.timeStyle = NSDateFormatterShortStyle;
	} else {
		f.dateStyle = NSDateFormatterShortStyle;
		f.timeStyle = NSDateFormatterShortStyle;
	}
	return [f stringFromDate:when];
}

/* ---- one conversation ---- */

@interface Bubble : UITableViewCell
@property (nonatomic, strong) UILabel *body, *note;
@property (nonatomic, strong) UIView *back;
@property (nonatomic, strong) NSLayoutConstraint *left, *right;
@end

@implementation Bubble

- (instancetype)initWithStyle:(UITableViewCellStyle)style reuseIdentifier:(NSString *)reuse
{
	self = [super initWithStyle:style reuseIdentifier:reuse];
	self.selectionStyle = UITableViewCellSelectionStyleNone;
	self.backgroundColor = UIColor.clearColor;
	self.back = [UIView new];
	self.back.layer.cornerRadius = 17;
	self.back.translatesAutoresizingMaskIntoConstraints = NO;
	self.body = [UILabel new];
	self.body.numberOfLines = 0;
	self.body.font = [UIFont systemFontOfSize:17];
	self.body.translatesAutoresizingMaskIntoConstraints = NO;
	self.note = [UILabel new];
	self.note.font = [UIFont systemFontOfSize:11];
	self.note.textColor = UIColor.secondaryLabelColor;
	self.note.translatesAutoresizingMaskIntoConstraints = NO;
	[self.contentView addSubview:self.back];
	[self.back addSubview:self.body];
	[self.contentView addSubview:self.note];
	self.left = [self.back.leadingAnchor constraintEqualToAnchor:self.contentView.leadingAnchor constant:12];
	self.right = [self.back.trailingAnchor constraintEqualToAnchor:self.contentView.trailingAnchor constant:-12];
	[NSLayoutConstraint activateConstraints:@[
		[self.back.topAnchor constraintEqualToAnchor:self.contentView.topAnchor constant:3],
		[self.back.widthAnchor constraintLessThanOrEqualToAnchor:self.contentView.widthAnchor multiplier:0.75],
		[self.body.topAnchor constraintEqualToAnchor:self.back.topAnchor constant:8],
		[self.body.bottomAnchor constraintEqualToAnchor:self.back.bottomAnchor constant:-8],
		[self.body.leadingAnchor constraintEqualToAnchor:self.back.leadingAnchor constant:12],
		[self.body.trailingAnchor constraintEqualToAnchor:self.back.trailingAnchor constant:-12],
		[self.note.topAnchor constraintEqualToAnchor:self.back.bottomAnchor constant:2],
		[self.note.bottomAnchor constraintEqualToAnchor:self.contentView.bottomAnchor constant:-4],
		[self.note.leadingAnchor constraintEqualToAnchor:self.back.leadingAnchor constant:4],
		[self.note.trailingAnchor constraintEqualToAnchor:self.back.trailingAnchor constant:-4],
	]];
	return self;
}

/* a text's body: its pictures (MMS: lines "\001IMG:PATH", smsd's) shown in it, at most 220 points wide */
static NSAttributedString *
body_with_pictures(NSString *body, UIColor *color)
{
	NSMutableAttributedString *out = [NSMutableAttributedString new];
	NSDictionary *style = @{ NSFontAttributeName: [UIFont systemFontOfSize:17], NSForegroundColorAttributeName: color };
	NSArray<NSString *> *lines = [body componentsSeparatedByString:@"\n"];

	for (NSUInteger i = 0; i < lines.count; i++) {
		NSString *line = lines[i];

		if (i > 0) {
			[out appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n" attributes:style]];
		}
		if ([line hasPrefix:@"\001IMG:"]) {
			UIImage *image = [UIImage imageWithContentsOfFile:[line substringFromIndex:5]];

			if (image != nil) {
				NSTextAttachment *a = [NSTextAttachment new];
				CGFloat w = MIN(220, image.size.width), h = image.size.width > 0 ? w * image.size.height / image.size.width : 0;

				a.image = image;
				a.bounds = CGRectMake(0, 0, w, h);
				[out appendAttributedString:[NSAttributedString attributedStringWithAttachment:a]];
			} else {
				[out appendAttributedString:[[NSAttributedString alloc] initWithString:@"[Picture]" attributes:style]];
			}
		} else {
			[out appendAttributedString:[[NSAttributedString alloc] initWithString:line attributes:style]];
		}
	}
	return out;
}

/* a text's body as one line of plain text (the conversations' list, a picture said) */
static NSString *
body_plain(NSString *body)
{
	NSMutableArray *parts = [NSMutableArray array];

	for (NSString *line in [body componentsSeparatedByString:@"\n"]) {
		[parts addObject:[line hasPrefix:@"\001IMG:"] ? @"[Picture]" : line];
	}
	return [parts componentsJoinedByString:@" "];
}

- (void)show:(Text *)t
{
	BOOL mine = ![t.kind isEqualToString:@"in"];

	self.body.attributedText = body_with_pictures(t.body, mine ? UIColor.whiteColor : UIColor.labelColor);
	self.back.backgroundColor = [t.kind isEqualToString:@"failed"] ? UIColor.systemRedColor :
	    mine ? UIColor.systemGreenColor : UIColor.systemGray5Color;
	self.note.text = [t.kind isEqualToString:@"failed"] ? @"Not sent" : [t.kind isEqualToString:@"sending"] ? @"Sending..." :
	    when_text(t.when);
	self.note.textAlignment = mine ? NSTextAlignmentRight : NSTextAlignmentLeft;
	self.left.active = !mine;
	self.right.active = mine;
}

@end

/* ---- a picture to send: the camera's (DCIM) and the ones received, newest first ---- */

@interface PickerController : UICollectionViewController
@property (nonatomic, strong) NSArray<NSString *> *paths;
@property (nonatomic, copy) void (^picked)(NSString *path);
@property (nonatomic, strong) NSCache *thumbnails;
@end

@implementation PickerController

- (instancetype)init
{
	UICollectionViewFlowLayout *layout = [UICollectionViewFlowLayout new];
	CGFloat side = (UIScreen.mainScreen.bounds.size.width - 4) / 3;

	layout.itemSize = CGSizeMake(side, side);
	layout.minimumInteritemSpacing = 2;
	layout.minimumLineSpacing = 2;
	return [super initWithCollectionViewLayout:layout];
}

- (void)viewDidLoad
{
	NSMutableArray<NSString *> *found = [NSMutableArray array];
	NSMutableDictionary<NSString *, NSDate *> *when = [NSMutableDictionary dictionary];
	NSFileManager *fm = NSFileManager.defaultManager;

	[super viewDidLoad];
	self.title = @"Choose a Picture";
	self.thumbnails = [NSCache new];
	self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemCancel
	    target:self action:@selector(cancel:)];
	[self.collectionView registerClass:[UICollectionViewCell class] forCellWithReuseIdentifier:@"picture"];
	for (NSString *root in @[ PHOTOS, MMS_DIR ]) {
		NSDirectoryEnumerator *e = [fm enumeratorAtPath:root];

		for (NSString *name in e) {
			NSString *ext = name.pathExtension.lowercaseString;

			if ([@[ @"jpg", @"jpeg", @"heic", @"png" ] containsObject:ext]) {
				NSString *path = [root stringByAppendingPathComponent:name];

				[found addObject:path];
				when[path] = [fm attributesOfItemAtPath:path error:NULL].fileModificationDate ?: NSDate.distantPast;
			}
		}
	}
	self.paths = [found sortedArrayUsingComparator:^NSComparisonResult(NSString *a, NSString *b) {
		return [when[b] compare:when[a]];
	}];
	NSLog(@"Texts: %lu pictures to choose from", (unsigned long)self.paths.count);
}

- (void)cancel:(id)sender
{
	[self dismissViewControllerAnimated:YES completion:nil];
}

- (NSInteger)collectionView:(UICollectionView *)collectionView numberOfItemsInSection:(NSInteger)section
{
	return (NSInteger)self.paths.count;
}

- (UICollectionViewCell *)collectionView:(UICollectionView *)collectionView cellForItemAtIndexPath:(NSIndexPath *)indexPath
{
	UICollectionViewCell *cell = [collectionView dequeueReusableCellWithReuseIdentifier:@"picture" forIndexPath:indexPath];
	NSString *path = self.paths[(NSUInteger)indexPath.item];
	UIImageView *view = (UIImageView *)[cell.contentView viewWithTag:1];
	UIImage *small = [self.thumbnails objectForKey:path];

	if (view == nil) {
		view = [[UIImageView alloc] initWithFrame:cell.contentView.bounds];
		view.tag = 1;
		view.contentMode = UIViewContentModeScaleAspectFill;
		view.clipsToBounds = YES;
		[cell.contentView addSubview:view];
	}
	view.image = small;
	if (small == nil) {
		dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
			UIImage *made = thumbnail(path, 300);

			if (made != nil) {
				[self.thumbnails setObject:made forKey:path];
				dispatch_async(dispatch_get_main_queue(), ^{
					[collectionView reloadItemsAtIndexPaths:@[ indexPath ]];
				});
			}
		});
	}
	return cell;
}

- (void)collectionView:(UICollectionView *)collectionView didSelectItemAtIndexPath:(NSIndexPath *)indexPath
{
	NSString *path = self.paths[(NSUInteger)indexPath.item];

	[self dismissViewControllerAnimated:YES completion:^{
		if (self.picked != nil) {
			self.picked(path);
		}
	}];
}

@end

@interface ThreadController : UIViewController <UITableViewDataSource, UITextFieldDelegate>
@property (nonatomic, copy) NSString *number, *tail;
@property (nonatomic, strong) NSArray<Text *> *texts;
@property (nonatomic, strong) UITableView *table;
@property (nonatomic, strong) UITextField *field;
@property (nonatomic, strong) UIButton *send;
@property (nonatomic, strong) Text *sending;
@property (nonatomic, copy) NSString *mmsRequest;       /* the MMS being sent: its request in OUTBOX, until smsd takes it */
@property (nonatomic) int sends;
@property (nonatomic, strong) NSTimer *timer;
@end

@implementation ThreadController

- (void)viewDidLoad
{
	UIView *bar = [UIView new];

	[super viewDidLoad];
	self.view.backgroundColor = UIColor.systemBackgroundColor;
	self.tail = tail_digits(self.number);
	self.table = [[UITableView alloc] initWithFrame:self.view.bounds style:UITableViewStylePlain];
	self.table.dataSource = self;
	self.table.separatorStyle = UITableViewCellSeparatorStyleNone;
	self.table.rowHeight = UITableViewAutomaticDimension;
	self.table.estimatedRowHeight = 60;
	self.table.keyboardDismissMode = UIScrollViewKeyboardDismissModeInteractive;
	[self.table registerClass:[Bubble class] forCellReuseIdentifier:@"bubble"];
	self.table.translatesAutoresizingMaskIntoConstraints = NO;
	bar.backgroundColor = UIColor.secondarySystemBackgroundColor;
	bar.translatesAutoresizingMaskIntoConstraints = NO;
	self.field = [UITextField new];
	self.field.placeholder = @"Text Message";
	self.field.borderStyle = UITextBorderStyleRoundedRect;
	self.field.delegate = self;
	self.field.returnKeyType = UIReturnKeySend;
	self.field.translatesAutoresizingMaskIntoConstraints = NO;
	self.send = [UIButton buttonWithType:UIButtonTypeSystem];
	[self.send setImage:[UIImage systemImageNamed:@"arrow.up.circle.fill" withConfiguration:[UIImageSymbolConfiguration
	    configurationWithPointSize:30]] forState:UIControlStateNormal];
	self.send.tintColor = UIColor.systemGreenColor;
	[self.send addTarget:self action:@selector(sendText) forControlEvents:UIControlEventTouchUpInside];
	self.send.translatesAutoresizingMaskIntoConstraints = NO;
	UIButton *photo = [UIButton buttonWithType:UIButtonTypeSystem];

	[photo setImage:[UIImage systemImageNamed:@"photo.on.rectangle" withConfiguration:[UIImageSymbolConfiguration
	    configurationWithPointSize:22]] forState:UIControlStateNormal];
	photo.tintColor = UIColor.systemGrayColor;
	photo.accessibilityLabel = @"Picture";
	[photo addTarget:self action:@selector(choosePicture) forControlEvents:UIControlEventTouchUpInside];
	photo.translatesAutoresizingMaskIntoConstraints = NO;
	[self.view addSubview:self.table];
	[self.view addSubview:bar];
	[bar addSubview:photo];
	[bar addSubview:self.field];
	[bar addSubview:self.send];
	[NSLayoutConstraint activateConstraints:@[
		[self.table.topAnchor constraintEqualToAnchor:self.view.topAnchor],
		[self.table.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
		[self.table.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
		[self.table.bottomAnchor constraintEqualToAnchor:bar.topAnchor],
		[bar.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
		[bar.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
		[bar.bottomAnchor constraintEqualToAnchor:self.view.keyboardLayoutGuide.topAnchor],
		[self.field.topAnchor constraintEqualToAnchor:bar.topAnchor constant:8],
		[self.field.bottomAnchor constraintEqualToAnchor:bar.bottomAnchor constant:-8],
		[photo.leadingAnchor constraintEqualToAnchor:bar.leadingAnchor constant:10],
		[photo.centerYAnchor constraintEqualToAnchor:self.field.centerYAnchor],
		[self.field.leadingAnchor constraintEqualToAnchor:photo.trailingAnchor constant:8],
		[self.field.heightAnchor constraintEqualToConstant:36],
		[self.send.leadingAnchor constraintEqualToAnchor:self.field.trailingAnchor constant:8],
		[self.send.trailingAnchor constraintEqualToAnchor:bar.trailingAnchor constant:-10],
		[self.send.centerYAnchor constraintEqualToAnchor:self.field.centerYAnchor],
	]];
	[self reload];
}

- (void)viewWillAppear:(BOOL)animated
{
	[super viewWillAppear:animated];
	self.timer = [NSTimer scheduledTimerWithTimeInterval:1 target:self selector:@selector(tick) userInfo:nil repeats:YES];
}

- (void)viewWillDisappear:(BOOL)animated
{
	[super viewWillDisappear:animated];
	[self.timer invalidate];
}

- (void)reload
{
	NSMutableArray<Text *> *mine = [NSMutableArray array];
	NSUInteger before = self.texts.count;

	for (Text *t in [Store texts]) {
		if ([tail_digits(t.number) isEqualToString:self.tail]) {
			[mine addObject:t];
		}
	}
	if (self.sending != nil) {
		[mine addObject:self.sending];
	}
	self.texts = mine;
	if (mine.count != before) {
		[self.table reloadData];
		if (mine.count != 0) {
			[self.table scrollToRowAtIndexPath:[NSIndexPath indexPathForRow:(NSInteger)mine.count - 1 inSection:0]
			    atScrollPosition:UITableViewScrollPositionBottom animated:before != 0];
		}
	}
}

/* every second: a text that came, and the one being sent */
- (void)tick
{
	if (self.sending != nil && self.mmsRequest != nil) {
		/* (an MMS: smsd removes the request, then writes the text out or failed; a while to send a picture) */
		if (![NSFileManager.defaultManager fileExistsAtPath:self.mmsRequest] &&
		    [[Store texts].lastObject.body isEqualToString:self.sending.body]) {
			self.sending = nil;
			self.mmsRequest = nil;
			self.texts = @[];
		}
	} else if (self.sending != nil) {
		int state = 0, sends = 0;
		size_t size = sizeof(int);

		sysctlbyname("kern.hakios.sms_send_state", &state, &size, NULL, 0);
		size = sizeof(int);
		sysctlbyname("kern.hakios.sms_sends", &sends, &size, NULL, 0);
		if (sends != self.sends && (state == 2 || state == 3)) {
			NSLog(@"Texts: the text to %@ is %@", self.number, state == 2 ? @"sent" : @"NOT sent");
			[Store add:state == 2 ? @"out" : @"failed" number:self.number body:self.sending.body];
			self.sending = nil;
			self.texts = @[];
		}
	}
	[self reload];
}

- (void)sendText
{
	NSString *body = self.field.text, *request;
	int sends = 0;
	size_t size = sizeof(sends);
	const char *bytes;

	if (body.length == 0 || self.sending != nil) {
		return;
	}
	request = [NSString stringWithFormat:@"%@\n%@", self.number, body];
	bytes = request.UTF8String;
	sysctlbyname("kern.hakios.sms_sends", &sends, &size, NULL, 0);
	if (sysctlbyname("kern.hakios.sms_send", NULL, NULL, (void *)bytes, strlen(bytes)) != 0) {
		NSString *why = errno == E2BIG ? @"Too long to send" : errno == EBUSY ? @"Another text is being sent" :
		    errno == ENOENT ? @"No modem on this phone" : [NSString stringWithFormat:@"Not sent (%s)", strerror(errno)];
		UIAlertController *a = [UIAlertController alertControllerWithTitle:why message:nil
		    preferredStyle:UIAlertControllerStyleAlert];

		[a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleCancel handler:nil]];
		[self presentViewController:a animated:YES completion:nil];
		return;
	}
	NSLog(@"Texts: sending %lu characters to %@", (unsigned long)body.length, self.number);
	self.sends = sends;
	self.sending = [Text new];
	self.sending.when = NSDate.date;
	self.sending.kind = @"sending";
	self.sending.number = self.number;
	self.sending.body = body;
	self.field.text = @"";
	[self reload];
}

- (void)choosePicture
{
	PickerController *picker = [PickerController new];
	__weak ThreadController *weak = self;

	picker.picked = ^(NSString *path) {
		[weak sendPicture:path];
	};
	[self presentViewController:[[UINavigationController alloc] initWithRootViewController:picker] animated:YES completion:nil];
}

/* the picture made small, saved, and an MMS asked of smsd (OUTBOX: number, picture, text); the field's text with it */
- (void)sendPicture:(NSString *)path
{
	NSData *jpeg = picture_to_send(path);
	NSString *saved, *request, *name, *text = self.field.text ?: @"";
	NSFileManager *fm = NSFileManager.defaultManager;

	if (jpeg == nil || self.sending != nil) {
		return;
	}
	[fm createDirectoryAtPath:MMS_DIR withIntermediateDirectories:YES attributes:nil error:NULL];
	[fm createDirectoryAtPath:OUTBOX withIntermediateDirectories:YES attributes:nil error:NULL];
	name = [NSString stringWithFormat:@"out-%ld", (long)NSDate.date.timeIntervalSince1970];
	saved = [MMS_DIR stringByAppendingPathComponent:[name stringByAppendingPathExtension:@"jpg"]];
	[jpeg writeToFile:saved atomically:YES];
	request = [NSString stringWithFormat:@"%@\n%@\n%@", self.number, saved, text];
	self.mmsRequest = [OUTBOX stringByAppendingPathComponent:[name stringByAppendingPathExtension:@"req"]];
	[request writeToFile:[self.mmsRequest stringByAppendingString:@".tmp"] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
	rename([self.mmsRequest stringByAppendingString:@".tmp"].fileSystemRepresentation, self.mmsRequest.fileSystemRepresentation);
	NSLog(@"Texts: an MMS to %@: a picture of %lu bytes", self.number, (unsigned long)jpeg.length);
	self.sending = [Text new];
	self.sending.when = NSDate.date;
	self.sending.kind = @"sending";
	self.sending.number = self.number;
	self.sending.body = text.length != 0 ? [NSString stringWithFormat:@"%@\n\001IMG:%@", text, saved] :
	    [@"\001IMG:" stringByAppendingString:saved];
	self.field.text = @"";
	[self reload];
}

- (BOOL)textFieldShouldReturn:(UITextField *)field
{
	[self sendText];
	return NO;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section
{
	return (NSInteger)self.texts.count;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
	Bubble *cell = [tableView dequeueReusableCellWithIdentifier:@"bubble" forIndexPath:indexPath];

	[cell show:self.texts[(NSUInteger)indexPath.row]];
	return cell;
}

@end

/* ---- the conversations ---- */

@interface ThreadsController : UITableViewController
@property (nonatomic, strong) NSArray<Text *> *latest;        /* each conversation's last text, newest first */
@property (nonatomic, strong) NSDictionary<NSString *, NSString *> *names;
@property (nonatomic) NSUInteger count;
@end

@implementation ThreadsController

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.title = @"Texts";
	self.navigationController.navigationBar.prefersLargeTitles = YES;
	self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc]
	    initWithBarButtonSystemItem:UIBarButtonSystemItemCompose target:self action:@selector(compose:)];
	[NSTimer scheduledTimerWithTimeInterval:2 target:self selector:@selector(reload) userInfo:nil repeats:YES];
}

- (void)viewWillAppear:(BOOL)animated
{
	[super viewWillAppear:animated];
	self.names = [Store names];
	self.count = NSUIntegerMax;
	[self reload];
}

- (void)reload
{
	NSArray<Text *> *texts = [Store texts];
	NSMutableDictionary<NSString *, Text *> *last = [NSMutableDictionary dictionary];

	if (texts.count == self.count) {
		return;
	}
	self.count = texts.count;
	for (Text *t in texts) {
		last[tail_digits(t.number)] = t;
	}
	self.latest = [last.allValues sortedArrayUsingComparator:^NSComparisonResult(Text *a, Text *b) {
		return [b.when compare:a.when];
	}];
	[self.tableView reloadData];
}

- (NSString *)nameOf:(NSString *)number
{
	return self.names[tail_digits(number)] ?: number;
}

- (void)open:(NSString *)number
{
	ThreadController *t = [ThreadController new];

	t.number = number;
	t.title = [self nameOf:number];
	[self.navigationController pushViewController:t animated:YES];
}

- (void)compose:(id)sender
{
	UIAlertController *a = [UIAlertController alertControllerWithTitle:@"New Message" message:@"To a number, or a contact's name"
	    preferredStyle:UIAlertControllerStyleAlert];
	__weak UIAlertController *weak = a;

	[a addTextFieldWithConfigurationHandler:^(UITextField *t) {
		t.placeholder = @"Number or name";
	}];
	[a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
	[a addAction:[UIAlertAction actionWithTitle:@"Next" style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) {
		NSString *to = [weak.textFields[0].text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
		NSMutableString *number = [NSMutableString string];

		/* a contact's name: its number */
		for (NSString *tail in self.names) {
			if ([self.names[tail] localizedCaseInsensitiveCompare:to] == NSOrderedSame) {
				to = tail;
				break;
			}
		}
		for (NSUInteger i = 0; i < to.length; i++) {
			unichar c = [to characterAtIndex:i];

			if ((c >= '0' && c <= '9') || (c == '+' && number.length == 0)) {
				[number appendFormat:@"%C", c];
			}
		}
		if (tail_digits(number).length >= 3) {
			[self open:number];
		}
	}]];
	[self presentViewController:a animated:YES completion:nil];
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section
{
	return (NSInteger)MAX(self.latest.count, 1);
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
	UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"thread"];
	Text *t;

	if (cell == nil) {
		cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"thread"];
		cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
		cell.detailTextLabel.numberOfLines = 2;
	}
	if (self.latest.count == 0) {
		cell.textLabel.text = @"No texts yet";
		cell.textLabel.textColor = UIColor.secondaryLabelColor;
		cell.detailTextLabel.text = @"The compose button writes a new one";
		cell.accessoryType = UITableViewCellAccessoryNone;
		return cell;
	}
	t = self.latest[(NSUInteger)indexPath.row];
	cell.textLabel.text = [self nameOf:t.number];
	cell.textLabel.textColor = UIColor.labelColor;
	cell.detailTextLabel.text = [NSString stringWithFormat:@"%@ · %@", when_text(t.when), body_plain(t.body)];
	cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
	return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
	[tableView deselectRowAtIndexPath:indexPath animated:YES];
	if (self.latest.count != 0) {
		[self open:self.latest[(NSUInteger)indexPath.row].number];
	}
}

@end

@interface TextsDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation TextsDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options
{
	self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
	self.window.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
	self.window.rootViewController = [[UINavigationController alloc] initWithRootViewController:[ThreadsController new]];
	[self.window makeKeyAndVisible];
	return YES;
}

@end

int
main(int argc, char *argv[])
{
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([TextsDelegate class]));
	}
}

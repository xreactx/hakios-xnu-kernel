/*
 * Phone Pad: the 6T's phone application (/Applications/PhonePad.app): Recents, Contacts and a keypad.
 *
 * The Phone app needs CommCenter, which speaks Apple's baseband protocol and does not run here; the 6T's modem is
 * driven by the kernel (drivers/modem/hakios.mpss.c, QMI VOICE). A number typed, or picked from Recents or Contacts,
 * is written to kern.hakios.call_dial, the kernel has the modem dial it (over VoLTE when IMS is registered), and calld
 * (src/calld.c) shows the call -- "Calling..." with End, then the call's alert -- while its audio goes through the
 * audio DSP (drivers/audio/hakios.mic.c).
 *
 * Recents: calld's call history (/var/mobile/Library/HakiOS/calls.txt: start, kind, number, seconds talked), newest
 * first, with the contacts' names. Contacts: iOS's own (Contacts.framework, as the Contacts application keeps them),
 * those with a phone number; + adds one (a name and a number).
 */
#import <UIKit/UIKit.h>
#import <Contacts/Contacts.h>
#include <sys/sysctl.h>

#define HISTORY @"/private/var/mobile/Library/HakiOS/calls.txt"
#define NAMES @"/private/var/mobile/Library/HakiOS/names.txt"

/* a number's last 10 digits: how numbers are matched (808..., 1808... and +1808... are the same one) */
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

/* the number to the kernel: nil when it is being dialled, else why not */
static NSString *
dial(NSString *number)
{
	NSMutableString *clean = [NSMutableString string];
	int state = 0;
	size_t size = sizeof(state);

	for (NSUInteger i = 0; i < number.length; i++) {
		unichar c = [number characterAtIndex:i];

		if ((c >= '0' && c <= '9') || c == '+' || c == '*' || c == '#') {
			[clean appendFormat:@"%C", c];
		}
	}
	if (clean.length == 0 || clean.length > 20) {
		return @"Not a number to call";
	}
	if (sysctlbyname("kern.hakios.call_state", &state, &size, NULL, 0) != 0) {
		return @"No modem on this phone";
	}
	if (state != 0 && state != 9) {
		return @"A call is already up";
	}
	if (sysctlbyname("kern.hakios.call_dial", NULL, NULL, (void *)clean.UTF8String, strlen(clean.UTF8String) + 1) != 0) {
		return [NSString stringWithFormat:@"Not dialled (%s)", strerror(errno)];
	}
	NSLog(@"PhonePad: dialling %@", clean);
	return nil;
}

static void
dial_from(UIViewController *from, NSString *number)
{
	NSString *problem = dial(number);

	if (problem != nil) {
		UIAlertController *a = [UIAlertController alertControllerWithTitle:problem message:number
		    preferredStyle:UIAlertControllerStyleAlert];

		[a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleCancel handler:nil]];
		[from presentViewController:a animated:YES completion:nil];
	}
}

/* ---- the contacts ---- */

@interface Person : NSObject
@property (nonatomic, copy) NSString *name;
@property (nonatomic, copy) NSArray<NSString *> *numbers;
@property (nonatomic, copy) NSArray<NSString *> *labels;
@end

@implementation Person
@end

@interface Book : NSObject
+ (NSArray<Person *> *)people:(NSString **)problem;
+ (NSDictionary<NSString *, NSString *> *)names;
+ (NSString *)add:(NSString *)name number:(NSString *)number;
@end

@implementation Book

+ (NSArray<Person *> *)people:(NSString **)problem
{
	CNContactStore *store = [CNContactStore new];
	NSArray *keys = @[ CNContactPhoneNumbersKey, CNContactOrganizationNameKey,
	    [CNContactFormatter descriptorForRequiredKeysForStyle:CNContactFormatterStyleFullName] ];
	CNContactFetchRequest *request = [[CNContactFetchRequest alloc] initWithKeysToFetch:keys];
	NSMutableArray<Person *> *people = [NSMutableArray array];
	NSError *error = nil;

	request.sortOrder = CNContactSortOrderGivenName;
	[store enumerateContactsWithFetchRequest:request error:&error usingBlock:^(CNContact *c, BOOL *stop) {
		NSMutableArray *numbers = [NSMutableArray array], *labels = [NSMutableArray array];
		Person *p = [Person new];

		for (CNLabeledValue<CNPhoneNumber *> *n in c.phoneNumbers) {
			[numbers addObject:n.value.stringValue];
			[labels addObject:n.label != nil ? [CNLabeledValue localizedStringForLabel:n.label] : @"phone"];
		}
		if (numbers.count == 0) {
			return;
		}
		p.name = [CNContactFormatter stringFromContact:c style:CNContactFormatterStyleFullName];
		if (p.name.length == 0) {
			p.name = c.organizationName.length != 0 ? c.organizationName : numbers[0];
		}
		p.numbers = numbers;
		p.labels = labels;
		[people addObject:p];
	}];
	if (error != nil && problem != NULL) {
		*problem = error.localizedDescription;
		NSLog(@"PhonePad: the contacts cannot be read: %@", error);
	}
	return people;
}

+ (NSDictionary<NSString *, NSString *> *)names
{
	NSMutableDictionary *names = [NSMutableDictionary dictionary];
	NSMutableString *list = [NSMutableString string];

	for (Person *p in [self people:NULL]) {
		for (NSString *n in p.numbers) {
			names[tail_digits(n)] = p.name;
		}
	}
	/* (a copy for calld's alerts, should the contacts not be open to it: a 10-digit tail and a name a line) */
	for (NSString *tail in names) {
		NSString *name = [[names[tail] componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]
		    componentsJoinedByString:@" "];

		[list appendFormat:@"%@\t%@\n", tail, [name stringByReplacingOccurrencesOfString:@"\t" withString:@" "]];
	}
	[[NSFileManager defaultManager] createDirectoryAtPath:NAMES.stringByDeletingLastPathComponent
	    withIntermediateDirectories:YES attributes:nil error:NULL];
	[list writeToFile:NAMES atomically:YES encoding:NSUTF8StringEncoding error:NULL];
	return names;
}

+ (NSString *)add:(NSString *)name number:(NSString *)number
{
	CNMutableContact *c = [CNMutableContact new];
	NSArray<NSString *> *words = [name componentsSeparatedByString:@" "];
	CNSaveRequest *save = [CNSaveRequest new];
	NSError *error = nil;

	c.givenName = words[0];
	if (words.count > 1) {
		c.familyName = [[words subarrayWithRange:NSMakeRange(1, words.count - 1)] componentsJoinedByString:@" "];
	}
	c.phoneNumbers = @[ [CNLabeledValue labeledValueWithLabel:CNLabelPhoneNumberMobile
	    value:[CNPhoneNumber phoneNumberWithStringValue:number]] ];
	[save addContact:c toContainerWithIdentifier:nil];
	if (![[CNContactStore new] executeSaveRequest:save error:&error]) {
		NSLog(@"PhonePad: the contact was not saved: %@", error);
		return error.localizedDescription ?: @"Not saved";
	}
	return nil;
}

@end

static UITableViewCell *
cell_for(UITableView *table, NSString *reuse)
{
	UITableViewCell *cell = [table dequeueReusableCellWithIdentifier:reuse];

	if (cell == nil) {
		cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:reuse];
		cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
	}
	return cell;
}

/* ---- Recents ---- */

@interface Call : NSObject
@property (nonatomic, strong) NSDate *start;
@property (nonatomic, copy) NSString *kind, *number;
@property (nonatomic) long seconds;
@end

@implementation Call
@end

@interface RecentsController : UITableViewController
@property (nonatomic, strong) NSArray<Call *> *calls;
@property (nonatomic, strong) NSDictionary<NSString *, NSString *> *names;
@end

@implementation RecentsController

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.title = @"Recents";
	self.calls = @[];
	[[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(reload)
	    name:UIApplicationWillEnterForegroundNotification object:nil];
	/* (a call made from here is in the history once it is over) */
	[NSTimer scheduledTimerWithTimeInterval:3 target:self selector:@selector(reload) userInfo:nil repeats:YES];
}

- (void)viewWillAppear:(BOOL)animated
{
	[super viewWillAppear:animated];
	[self reload];
}

- (void)reload
{
	NSString *text = [NSString stringWithContentsOfFile:HISTORY encoding:NSUTF8StringEncoding error:NULL];
	NSMutableArray<Call *> *calls = [NSMutableArray array];

	for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
		NSArray<NSString *> *f = [line componentsSeparatedByString:@"\t"];
		Call *c;

		if (f.count < 4) {
			continue;
		}
		c = [Call new];
		c.start = [NSDate dateWithTimeIntervalSince1970:f[0].doubleValue];
		c.kind = f[1];
		c.number = f[2];
		c.seconds = f[3].integerValue;
		[calls insertObject:c atIndex:0];        /* (newest first) */
	}
	if (calls.count != self.calls.count || self.names == nil) {
		self.names = [Book names];
		self.calls = calls;
		[self.tableView reloadData];
	}
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section
{
	return (NSInteger)MAX(self.calls.count, 1);
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
	UITableViewCell *cell = cell_for(tableView, @"call");
	Call *c;
	NSString *name, *what, *when;
	NSDateFormatter *f = [NSDateFormatter new];

	if (self.calls.count == 0) {
		cell.textLabel.text = @"No calls yet";
		cell.textLabel.textColor = UIColor.secondaryLabelColor;
		cell.detailTextLabel.text = nil;
		cell.imageView.image = nil;
		cell.accessoryView = nil;
		return cell;
	}
	c = self.calls[(NSUInteger)indexPath.row];
	name = self.names[tail_digits(c.number)];
	cell.textLabel.text = name ?: c.number;
	cell.textLabel.textColor = [c.kind isEqualToString:@"missed"] ? UIColor.systemRedColor : UIColor.labelColor;
	if ([c.kind isEqualToString:@"out"]) {
		what = c.seconds > 0 ? @"Outgoing" : @"Outgoing, not answered";
	} else if ([c.kind isEqualToString:@"missed"]) {
		what = @"Missed";
	} else if ([c.kind isEqualToString:@"declined"]) {
		what = @"Declined";
	} else {
		what = @"Incoming";
	}
	if ([NSCalendar.currentCalendar isDateInToday:c.start]) {
		f.timeStyle = NSDateFormatterShortStyle;
	} else {
		f.dateStyle = NSDateFormatterShortStyle;
		f.timeStyle = NSDateFormatterShortStyle;
	}
	when = [f stringFromDate:c.start];
	cell.detailTextLabel.text = c.seconds > 0 ?
	    [NSString stringWithFormat:@"%@%@ · %@ · %ld:%02ld", name != nil ? [c.number stringByAppendingString:@" · "] : @"",
	    what, when, c.seconds / 60, c.seconds % 60] :
	    [NSString stringWithFormat:@"%@%@ · %@", name != nil ? [c.number stringByAppendingString:@" · "] : @"", what, when];
	cell.imageView.image = [UIImage systemImageNamed:[c.kind isEqualToString:@"out"] ? @"phone.arrow.up.right" :
	    [c.kind isEqualToString:@"in"] ? @"phone.arrow.down.left" : @"phone.down"];
	cell.imageView.tintColor = [c.kind isEqualToString:@"missed"] ? UIColor.systemRedColor : UIColor.secondaryLabelColor;
	return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
	[tableView deselectRowAtIndexPath:indexPath animated:YES];
	if (self.calls.count != 0) {
		dial_from(self, self.calls[(NSUInteger)indexPath.row].number);
	}
}

@end

/* ---- Contacts ---- */

@interface ContactsController : UITableViewController
@property (nonatomic, strong) NSArray<Person *> *people;
@property (nonatomic, copy) NSString *problem;
@end

@implementation ContactsController

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.title = @"Contacts";
	self.people = @[];
	self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc]
	    initWithBarButtonSystemItem:UIBarButtonSystemItemAdd target:self action:@selector(add:)];
}

- (void)viewWillAppear:(BOOL)animated
{
	[super viewWillAppear:animated];
	[self reload];
}

- (void)reload
{
	NSString *problem = nil;

	self.people = [Book people:&problem];
	self.problem = problem;
	(void)[Book names];     /* (calld's copy of the names, up to date) */
	[self.tableView reloadData];
}

- (void)add:(id)sender
{
	UIAlertController *a = [UIAlertController alertControllerWithTitle:@"New Contact" message:nil
	    preferredStyle:UIAlertControllerStyleAlert];
	__weak UIAlertController *weak = a;

	[a addTextFieldWithConfigurationHandler:^(UITextField *t) {
		t.placeholder = @"Name";
		t.autocapitalizationType = UITextAutocapitalizationTypeWords;
	}];
	[a addTextFieldWithConfigurationHandler:^(UITextField *t) {
		t.placeholder = @"Phone number";
		t.keyboardType = UIKeyboardTypePhonePad;
	}];
	[a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
	[a addAction:[UIAlertAction actionWithTitle:@"Save" style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) {
		NSString *name = [weak.textFields[0].text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
		NSString *number = weak.textFields[1].text, *problem;

		if (name.length == 0 || tail_digits(number).length == 0) {
			return;
		}
		problem = [Book add:name number:number];
		if (problem != nil) {
			UIAlertController *e = [UIAlertController alertControllerWithTitle:@"Not saved" message:problem
			    preferredStyle:UIAlertControllerStyleAlert];

			[e addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleCancel handler:nil]];
			[self presentViewController:e animated:YES completion:nil];
		}
		[self reload];
	}]];
	[self presentViewController:a animated:YES completion:nil];
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section
{
	return (NSInteger)MAX(self.people.count, 1);
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
	UITableViewCell *cell = cell_for(tableView, @"person");
	Person *p;

	if (self.people.count == 0) {
		cell.textLabel.text = self.problem != nil ? @"The contacts cannot be read" : @"No contacts: + adds one";
		cell.textLabel.textColor = UIColor.secondaryLabelColor;
		cell.detailTextLabel.text = self.problem;
		return cell;
	}
	p = self.people[(NSUInteger)indexPath.row];
	cell.textLabel.text = p.name;
	cell.textLabel.textColor = UIColor.labelColor;
	cell.detailTextLabel.text = p.numbers.count == 1 ? p.numbers[0] :
	    [NSString stringWithFormat:@"%@ and %lu more", p.numbers[0], (unsigned long)p.numbers.count - 1];
	return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
	Person *p;
	UIAlertController *a;

	[tableView deselectRowAtIndexPath:indexPath animated:YES];
	if (self.people.count == 0) {
		return;
	}
	p = self.people[(NSUInteger)indexPath.row];
	if (p.numbers.count == 1) {
		dial_from(self, p.numbers[0]);
		return;
	}
	a = [UIAlertController alertControllerWithTitle:p.name message:nil preferredStyle:UIAlertControllerStyleActionSheet];
	for (NSUInteger i = 0; i < p.numbers.count; i++) {
		NSString *number = p.numbers[i];

		[a addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"%@: %@", p.labels[i], number]
		    style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { dial_from(self, number); }]];
	}
	[a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
	[self presentViewController:a animated:YES completion:nil];
}

@end

/* ---- the keypad ---- */

@interface PadController : UIViewController
@property (nonatomic, strong) UILabel *display;
@property (nonatomic, strong) UILabel *status;
@property (nonatomic, copy) NSString *number;
@end

@implementation PadController

- (UIButton *)key:(NSString *)title letters:(NSString *)letters frame:(CGRect)frame
{
	UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
	NSMutableAttributedString *text = [[NSMutableAttributedString alloc] initWithString:title attributes:@{
		NSFontAttributeName: [UIFont systemFontOfSize:34 weight:UIFontWeightRegular],
		NSForegroundColorAttributeName: UIColor.whiteColor }];

	if (letters.length != 0) {
		[text appendAttributedString:[[NSAttributedString alloc] initWithString:[@"\n" stringByAppendingString:letters]
		    attributes:@{ NSFontAttributeName: [UIFont systemFontOfSize:10 weight:UIFontWeightSemibold],
		    NSForegroundColorAttributeName: UIColor.whiteColor, NSKernAttributeName: @2 }]];
	}
	b.frame = frame;
	b.backgroundColor = [UIColor colorWithWhite:0.2 alpha:1];
	b.layer.cornerRadius = frame.size.width / 2;
	b.titleLabel.numberOfLines = 2;
	b.titleLabel.textAlignment = NSTextAlignmentCenter;
	[b setAttributedTitle:text forState:UIControlStateNormal];
	[b addTarget:self action:@selector(pressed:) forControlEvents:UIControlEventTouchUpInside];
	b.accessibilityLabel = title;
	return b;
}

- (void)viewDidLoad
{
	[super viewDidLoad];
	NSArray *titles = @[ @"1", @"2", @"3", @"4", @"5", @"6", @"7", @"8", @"9", @"*", @"0", @"#" ];
	NSArray *letters = @[ @"", @"ABC", @"DEF", @"GHI", @"JKL", @"MNO", @"PQRS", @"TUV", @"WXYZ", @"", @"+", @"" ];
	CGRect bounds = self.view.bounds;
	CGFloat size = MIN(80, (bounds.size.width - 4 * 24) / 3), gap = 24;
	CGFloat left = (bounds.size.width - 3 * size - 2 * gap) / 2;
	CGFloat top = bounds.size.height * 0.26;    /* (above the tab bar) */

	self.number = @"";
	self.view.backgroundColor = UIColor.blackColor;

	self.display = [[UILabel alloc] initWithFrame:CGRectMake(16, top - 110, bounds.size.width - 32, 50)];
	self.display.textColor = UIColor.whiteColor;
	self.display.font = [UIFont systemFontOfSize:38 weight:UIFontWeightRegular];
	self.display.textAlignment = NSTextAlignmentCenter;
	self.display.adjustsFontSizeToFitWidth = YES;
	[self.view addSubview:self.display];

	self.status = [[UILabel alloc] initWithFrame:CGRectMake(16, top - 60, bounds.size.width - 32, 24)];
	self.status.textColor = [UIColor colorWithWhite:0.6 alpha:1];
	self.status.font = [UIFont systemFontOfSize:15];
	self.status.textAlignment = NSTextAlignmentCenter;
	[self.view addSubview:self.status];

	for (NSUInteger i = 0; i < titles.count; i++) {
		CGRect frame = CGRectMake(left + (i % 3) * (size + gap), top + (i / 3) * (size + gap * 0.7), size, size);
		UIButton *b = [self key:titles[i] letters:letters[i] frame:frame];

		b.tag = (NSInteger)i;
		if ([titles[i] isEqualToString:@"0"]) {
			/* a long press on 0 types '+', as on an iPhone */
			[b addGestureRecognizer:[[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(plus:)]];
		}
		[self.view addSubview:b];
	}

	CGFloat row = top + 4 * (size + gap * 0.7);
	UIButton *call = [UIButton buttonWithType:UIButtonTypeSystem];
	call.frame = CGRectMake(left + size + gap, row, size, size);
	call.backgroundColor = [UIColor colorWithRed:0.20 green:0.78 blue:0.35 alpha:1];
	call.layer.cornerRadius = size / 2;
	call.tintColor = UIColor.whiteColor;
	[call setImage:[UIImage systemImageNamed:@"phone.fill" withConfiguration:[UIImageSymbolConfiguration
	    configurationWithPointSize:30]] forState:UIControlStateNormal];
	call.accessibilityLabel = @"Call";
	[call addTarget:self action:@selector(dial:) forControlEvents:UIControlEventTouchUpInside];
	[self.view addSubview:call];

	UIButton *back = [UIButton buttonWithType:UIButtonTypeSystem];
	back.frame = CGRectMake(left + 2 * (size + gap), row, size, size);
	back.tintColor = [UIColor colorWithWhite:0.7 alpha:1];
	[back setImage:[UIImage systemImageNamed:@"delete.left.fill" withConfiguration:[UIImageSymbolConfiguration
	    configurationWithPointSize:24]] forState:UIControlStateNormal];
	back.accessibilityLabel = @"Delete";
	[back addTarget:self action:@selector(erase:) forControlEvents:UIControlEventTouchUpInside];
	[back addGestureRecognizer:[[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(clear:)]];
	[self.view addSubview:back];

	[self refresh];
}

- (void)refresh
{
	self.display.text = self.number;
}

- (void)pressed:(UIButton *)sender
{
	static NSString *const keys = @"123456789*0#";

	if (self.number.length < 20) {
		self.number = [self.number stringByAppendingString:[keys substringWithRange:NSMakeRange((NSUInteger)sender.tag, 1)]];
	}
	self.status.text = @"";
	[self refresh];
}

- (void)plus:(UILongPressGestureRecognizer *)g
{
	if (g.state == UIGestureRecognizerStateBegan && self.number.length < 20) {
		/* (the tap's '0' came first: replaced) */
		if ([self.number hasSuffix:@"0"]) {
			self.number = [self.number substringToIndex:self.number.length - 1];
		}
		self.number = [self.number stringByAppendingString:@"+"];
		[self refresh];
	}
}

- (void)erase:(id)sender
{
	if (self.number.length != 0) {
		self.number = [self.number substringToIndex:self.number.length - 1];
	}
	[self refresh];
}

- (void)clear:(UILongPressGestureRecognizer *)g
{
	if (g.state == UIGestureRecognizerStateBegan) {
		self.number = @"";
		[self refresh];
	}
}

- (void)dial:(id)sender
{
	NSString *problem;

	if (self.number.length == 0) {
		return;
	}
	problem = dial(self.number);
	self.status.text = problem ?: @"Calling...";
}

@end

@interface PadDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation PadDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options
{
	UITabBarController *tabs = [UITabBarController new];
	UINavigationController *recents = [[UINavigationController alloc] initWithRootViewController:[RecentsController new]];
	UINavigationController *contacts = [[UINavigationController alloc] initWithRootViewController:[ContactsController new]];
	PadController *pad = [PadController new];

	recents.tabBarItem = [[UITabBarItem alloc] initWithTitle:@"Recents" image:[UIImage systemImageNamed:@"clock.fill"] tag:0];
	contacts.tabBarItem = [[UITabBarItem alloc] initWithTitle:@"Contacts"
	    image:[UIImage systemImageNamed:@"person.crop.circle.fill"] tag:1];
	pad.tabBarItem = [[UITabBarItem alloc] initWithTitle:@"Keypad" image:[UIImage systemImageNamed:@"circle.grid.3x3.fill"] tag:2];
	tabs.viewControllers = @[ recents, contacts, pad ];
	tabs.selectedIndex = 2;
	self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
	self.window.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
	self.window.rootViewController = tabs;
	[self.window makeKeyAndVisible];
	return YES;
}

@end

int
main(int argc, char *argv[])
{
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([PadDelegate class]));
	}
}

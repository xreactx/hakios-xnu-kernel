/*
 * Phone Pad: the 6T's dialer (/Applications/PhonePad.app).
 *
 * The Phone app needs CommCenter, which speaks Apple's baseband protocol and does not run here; the 6T's modem is
 * driven by the kernel (drivers/modem/hakios.mpss.c, QMI VOICE). This is a keypad: the number typed is written to
 * kern.hakios.call_dial, the kernel has the modem dial it (over VoLTE when IMS is registered), and calld
 * (src/calld.c) shows the call -- "Calling..." with End, then the call's alert -- while its audio goes through the
 * audio DSP (drivers/audio/hakios.mic.c).
 */
#import <UIKit/UIKit.h>
#include <sys/sysctl.h>

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
	CGFloat top = bounds.size.height * 0.30;

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
	const char *n = self.number.UTF8String;
	int state = 0;
	size_t size = sizeof(state);

	if (self.number.length == 0) {
		return;
	}
	if (sysctlbyname("kern.hakios.call_state", &state, &size, NULL, 0) != 0) {
		self.status.text = @"No modem on this phone";
		return;
	}
	if (state != 0 && state != 9) {
		self.status.text = @"A call is already up";
		return;
	}
	if (sysctlbyname("kern.hakios.call_dial", NULL, NULL, (void *)n, strlen(n) + 1) != 0) {
		self.status.text = [NSString stringWithFormat:@"Not dialled (%s)", strerror(errno)];
		return;
	}
	NSLog(@"PhonePad: dialling %@", self.number);
	self.status.text = @"Calling...";
}

@end

@interface PadDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation PadDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options
{
	self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
	self.window.rootViewController = [PadController new];
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

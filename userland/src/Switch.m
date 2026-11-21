/*
 * Switch: restart the phone into Android, from HakiOS's home screen.
 *
 * Asks first; then writes "android" into kern.hakios.next_boot. The kernel ends the run the orderly way (the data
 * volume saved, the modem and DSPs stopped), and the loader marks this slot unbootable in the partition table before
 * it resets, so that the bootloader starts Android's slot by itself (xnuloader/loader.c, slot_to_android). Back to
 * HakiOS: "fastboot set_active a" from a computer.
 */
#import <UIKit/UIKit.h>
#include <sys/sysctl.h>

@interface SwitchController : UIViewController
@end

@implementation SwitchController
{
	UILabel *_label;
}

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.view.backgroundColor = UIColor.blackColor;
	_label = [[UILabel alloc] initWithFrame:CGRectInset(self.view.bounds, 24, 0)];
	_label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
	_label.textAlignment = NSTextAlignmentCenter;
	_label.numberOfLines = 0;
	_label.textColor = UIColor.whiteColor;
	_label.font = [UIFont systemFontOfSize:20 weight:UIFontWeightMedium];
	[self.view addSubview:_label];
}

- (void)viewDidAppear:(BOOL)animated
{
	[super viewDidAppear:animated];
	[self ask];
}

- (void)ask
{
	UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Restart into Android?"
	    message:@"The phone saves its data and restarts into Android. To come back to HakiOS, run "
	    "\"fastboot set_active a\" from a computer." preferredStyle:UIAlertControllerStyleAlert];

	[alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction *a) {
		self->_label.text = @"";
		[[UIApplication sharedApplication] performSelector:@selector(suspend)];
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.6 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
			exit(0);
		});
	}]];
	[alert addAction:[UIAlertAction actionWithTitle:@"Restart" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *a) {
		const char *word = "android";

		if (sysctlbyname("kern.hakios.next_boot", NULL, NULL, (void *)word, strlen(word)) == 0) {
			self->_label.text = @"Saving and restarting into Android…";
		} else {
			self->_label.text = [NSString stringWithFormat:@"This kernel cannot restart into Android (%s).", strerror(errno)];
		}
	}]];
	[self presentViewController:alert animated:YES completion:nil];
}

@end

@interface SwitchDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation SwitchDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options
{
	self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
	self.window.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
	self.window.rootViewController = [SwitchController new];
	[self.window makeKeyAndVisible];
	return YES;
}

@end

int
main(int argc, char *argv[])
{
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([SwitchDelegate class]));
	}
}

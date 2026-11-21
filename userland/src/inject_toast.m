/*
 * libhakios.toast.dylib: a banner at the top of the screen for a text that arrives, inside SpringBoard.
 *
 * smsd (src/smsd.c) writes the text's title (the sender, by the contact's name) and its words into TOAST_FILE and posts
 * the Darwin notification "com.hakios.toast"; here, in SpringBoard, a banner then slides down from the top -- the
 * title, the first lines of the text -- stays 5 seconds and slides away. A tap opens the Texts application. (iOS's own
 * banners come from an application's notifications, through usernotificationsd and the bulletin board; smsd is not an
 * application, and this is the plain way.)
 */
#import <UIKit/UIKit.h>
#include <dlfcn.h>
#include <notify.h>

#define TOAST_FILE      @"/private/var/mobile/Library/HakiOS/toast.txt"
#define TOAST_SECONDS   5.0

@interface HKToastWindow : UIWindow
@end

@implementation HKToastWindow
/* (only the banner takes touches: the rest of the screen stays SpringBoard's) */
- (UIView *)hitTest:(CGPoint)point withEvent:(UIEvent *)event
{
	UIView *hit = [super hitTest:point withEvent:event];

	return hit == self || hit == self.rootViewController.view ? nil : hit;
}
@end

static HKToastWindow *window;
static UIView *banner;
static NSUInteger shown;                /* banners shown: an older one's timer leaves a newer one up */

static void
open_texts(void)
{
	static int (*launch)(CFStringRef, Boolean);

	if (launch == NULL) {
		void *sbs = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", RTLD_LAZY);

		launch = sbs != NULL ? (int (*)(CFStringRef, Boolean))dlsym(sbs, "SBSLaunchApplicationWithIdentifier") : NULL;
	}
	if (launch != NULL) {
		/* (off the main thread: the request is served by SpringBoard itself) */
		dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
			launch(CFSTR("com.hakios.texts"), false);
		});
	}
}

@interface HKToastTarget : NSObject
@end

@implementation HKToastTarget
+ (void)tapped:(UITapGestureRecognizer *)g
{
	[UIView animateWithDuration:0.2 animations:^{
		banner.transform = CGAffineTransformMakeTranslation(0, -200);
	} completion:^(BOOL done) {
		window.hidden = YES;
	}];
	open_texts();
}
@end

static void
show(NSString *title, NSString *body)
{
	CGRect screen = UIScreen.mainScreen.bounds;
	CGFloat width = MIN(screen.size.width - 16, 420), top = 44;
	UIVisualEffectView *blur;
	UILabel *t, *b;
	NSUInteger mine;

	if (window == nil) {
		UIWindowScene *scene = nil;

		for (UIScene *s in UIApplication.sharedApplication.connectedScenes) {
			if ([s isKindOfClass:[UIWindowScene class]]) {
				scene = (UIWindowScene *)s;
				break;
			}
		}
		window = scene != nil ? [[HKToastWindow alloc] initWithWindowScene:scene] : [[HKToastWindow alloc] initWithFrame:screen];
		window.frame = screen;
		window.windowLevel = UIWindowLevelAlert + 100;
		window.backgroundColor = UIColor.clearColor;
		window.rootViewController = [UIViewController new];
		window.rootViewController.view.backgroundColor = UIColor.clearColor;
	}
	[banner removeFromSuperview];
	banner = [[UIView alloc] initWithFrame:CGRectMake((screen.size.width - width) / 2, top, width, 76)];
	banner.layer.cornerRadius = 18;
	banner.clipsToBounds = YES;
	blur = [[UIVisualEffectView alloc] initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemMaterialDark]];
	blur.frame = banner.bounds;
	[banner addSubview:blur];
	t = [[UILabel alloc] initWithFrame:CGRectMake(16, 10, width - 32, 22)];
	t.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
	t.textColor = UIColor.whiteColor;
	t.text = title;
	[banner addSubview:t];
	b = [[UILabel alloc] initWithFrame:CGRectMake(16, 32, width - 32, 38)];
	b.font = [UIFont systemFontOfSize:14];
	b.textColor = [UIColor colorWithWhite:0.9 alpha:1];
	b.numberOfLines = 2;
	b.text = body;
	[banner addSubview:b];
	[banner addGestureRecognizer:[[UITapGestureRecognizer alloc] initWithTarget:[HKToastTarget class]
	    action:@selector(tapped:)]];
	[window.rootViewController.view addSubview:banner];
	window.hidden = NO;
	banner.transform = CGAffineTransformMakeTranslation(0, -(top + 90));
	[UIView animateWithDuration:0.3 delay:0 usingSpringWithDamping:0.85 initialSpringVelocity:0 options:0 animations:^{
		banner.transform = CGAffineTransformIdentity;
	} completion:nil];
	mine = ++shown;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(TOAST_SECONDS * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
		if (mine != shown) {
			return;
		}
		[UIView animateWithDuration:0.3 animations:^{
			banner.transform = CGAffineTransformMakeTranslation(0, -(top + 90));
		} completion:^(BOOL done) {
			if (mine == shown) {
				window.hidden = YES;
			}
		}];
	});
}

__attribute__((constructor)) static void
toast_init(void)
{
	/* (once SpringBoard's UI is up) */
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		int token;

		notify_register_dispatch("com.hakios.toast", &token, dispatch_get_main_queue(), ^(int t) {
			NSString *text = [NSString stringWithContentsOfFile:TOAST_FILE encoding:NSUTF8StringEncoding error:NULL];
			NSRange newline = [text rangeOfString:@"\n"];

			if (text.length == 0) {
				return;
			}
			if (newline.location == NSNotFound) {
				show(text, @"");
			} else {
				show([text substringToIndex:newline.location], [text substringFromIndex:newline.location + 1]);
			}
		});
	});
}

/*
 * Level: a spirit level, and the 6T's motion sensors as an application sees them (/Applications/Level.app).
 *
 * It uses CoreMotion as any application would -- CMMotionManager's device motion, 30 times a second -- which on this
 * phone is answered by libhakios.motion.dylib (src/inject_motion.m) from the SLPI's sensors. Lying flat, a bubble moves
 * in a circle and the tilt is said in degrees; standing on an edge, the angle from upright. Below it: the rotation
 * rate (the gyroscope), the magnetic field and the heading.
 */
#import <UIKit/UIKit.h>
#import <CoreMotion/CoreMotion.h>

@interface LevelController : UIViewController
@property (nonatomic, strong) CMMotionManager *motion;
@property (nonatomic, strong) UIView *dial, *bubble, *cross;
@property (nonatomic, strong) UILabel *angle, *mode, *sensors;
@end

@implementation LevelController

- (UILabel *)label:(CGFloat)size weight:(UIFontWeight)weight
{
	UILabel *l = [UILabel new];

	l.textColor = UIColor.whiteColor;
	l.textAlignment = NSTextAlignmentCenter;
	l.font = [UIFont monospacedDigitSystemFontOfSize:size weight:weight];
	l.numberOfLines = 0;
	[self.view addSubview:l];
	return l;
}

- (void)viewDidLoad
{
	CGRect b;
	CGFloat d;

	[super viewDidLoad];
	self.view.backgroundColor = UIColor.blackColor;
	b = self.view.bounds;
	d = MIN(b.size.width - 60, 300);

	self.angle = [self label:72 weight:UIFontWeightLight];
	self.angle.frame = CGRectMake(0, b.size.height * 0.10, b.size.width, 90);
	self.mode = [self label:15 weight:UIFontWeightRegular];
	self.mode.textColor = [UIColor colorWithWhite:0.6 alpha:1];
	self.mode.frame = CGRectMake(0, CGRectGetMaxY(self.angle.frame), b.size.width, 22);

	self.dial = [[UIView alloc] initWithFrame:CGRectMake((b.size.width - d) / 2, b.size.height * 0.30, d, d)];
	self.dial.layer.cornerRadius = d / 2;
	self.dial.layer.borderWidth = 2;
	self.dial.layer.borderColor = [UIColor colorWithWhite:0.4 alpha:1].CGColor;
	[self.view addSubview:self.dial];
	self.cross = [[UIView alloc] initWithFrame:CGRectMake(d / 2 - 22, d / 2 - 22, 44, 44)];
	self.cross.layer.cornerRadius = 22;
	self.cross.layer.borderWidth = 1;
	self.cross.layer.borderColor = [UIColor colorWithWhite:0.5 alpha:1].CGColor;
	[self.dial addSubview:self.cross];
	self.bubble = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 40, 40)];
	self.bubble.layer.cornerRadius = 20;
	self.bubble.backgroundColor = UIColor.systemGreenColor;
	self.bubble.center = CGPointMake(d / 2, d / 2);
	[self.dial addSubview:self.bubble];

	self.sensors = [self label:13 weight:UIFontWeightRegular];
	self.sensors.textColor = [UIColor colorWithWhite:0.75 alpha:1];
	self.sensors.textAlignment = NSTextAlignmentLeft;
	self.sensors.frame = CGRectMake(24, CGRectGetMaxY(self.dial.frame) + 24, b.size.width - 48, 140);

	self.motion = [CMMotionManager new];
	if (!self.motion.deviceMotionAvailable) {
		self.angle.text = @"—";
		self.mode.text = @"No motion sensors";
		return;
	}
	self.motion.deviceMotionUpdateInterval = 1.0 / 30;
	__weak LevelController *weak = self;
	[self.motion startDeviceMotionUpdatesUsingReferenceFrame:CMAttitudeReferenceFrameXMagneticNorthZVertical
	    toQueue:NSOperationQueue.mainQueue withHandler:^(CMDeviceMotion *m, NSError *error) {
		if (m != nil) {
			[weak show:m];
		}
	}];
}

- (void)show:(CMDeviceMotion *)m
{
	CMAcceleration g = m.gravity;
	double norm = sqrt(g.x * g.x + g.y * g.y + g.z * g.z), tilt, shown;
	CGFloat d = self.dial.bounds.size.width, r = d / 2 - 22;
	BOOL flat;

	if (norm < 0.1) {
		/* (nothing measured: the sensors DSP did not come up this boot) */
		self.angle.text = @"—";
		self.mode.text = @"no readings from the sensors";
		return;
	}
	g.x /= norm; g.y /= norm; g.z /= norm;
	tilt = acos(fmin(1, fabs(g.z))) * 180 / M_PI;     /* from lying flat */
	flat = tilt < 50;
	if (flat) {
		/* the bubble goes where the phone is high: against gravity's part in the screen's plane */
		CGFloat k = (CGFloat)fmin(1, tilt / 30);
		double len = sqrt(g.x * g.x + g.y * g.y);
		CGFloat dx = len > 0 ? (CGFloat)(-g.x / len) * k * r : 0, dy = len > 0 ? (CGFloat)(g.y / len) * k * r : 0;

		self.dial.hidden = NO;
		self.bubble.center = CGPointMake(d / 2 + dx, d / 2 + dy);
		shown = tilt;
		self.mode.text = @"lying flat";
	} else {
		/* on an edge: the angle from upright, turning in the screen's plane */
		shown = atan2(g.x, -g.y) * 180 / M_PI;
		if (shown > 90) {
			shown -= 180;
		} else if (shown < -90) {
			shown += 180;
		}
		self.dial.hidden = YES;
		self.mode.text = @"on its edge";
	}
	self.angle.text = [NSString stringWithFormat:@"%.0f°", fabs(shown)];
	self.view.backgroundColor = fabs(shown) < 1 ? [UIColor colorWithRed:0 green:0.35 blue:0.1 alpha:1] : UIColor.blackColor;
	self.bubble.backgroundColor = fabs(shown) < 1 ? UIColor.whiteColor : UIColor.systemGreenColor;
	self.sensors.text = [NSString stringWithFormat:
	    @"gravity      x %+.2f  y %+.2f  z %+.2f g\n"
	    @"rotation     x %+.2f  y %+.2f  z %+.2f rad/s\n"
	    @"magnetic     x %+.0f  y %+.0f  z %+.0f µT\n"
	    @"heading      %.0f°\n"
	    @"attitude     roll %+.0f°  pitch %+.0f°  yaw %+.0f°",
	    m.gravity.x, m.gravity.y, m.gravity.z, m.rotationRate.x, m.rotationRate.y, m.rotationRate.z,
	    m.magneticField.field.x, m.magneticField.field.y, m.magneticField.field.z, m.heading,
	    m.attitude.roll * 180 / M_PI, m.attitude.pitch * 180 / M_PI, m.attitude.yaw * 180 / M_PI];
}

@end

@interface LevelDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation LevelDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options
{
	self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
	self.window.rootViewController = [LevelController new];
	[self.window makeKeyAndVisible];
	return YES;
}

@end

int
main(int argc, char *argv[])
{
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([LevelDelegate class]));
	}
}

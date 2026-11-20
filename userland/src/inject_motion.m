/*
 * libhakios.motion.dylib: CoreMotion for applications, from the 6T's own sensors.
 *
 * On an iPhone, CMMotionManager gets its samples from locationd, which reads them from Apple's motion coprocessor
 * (the SPU's HID services). There is none here: the 6T's accelerometer, gyroscope and magnetometer sit behind the
 * SLPI, read by the kernel (devices/fajita/soc/drivers/sensors/hakios.slpi.c) into sysctls -- kern.hakios.accel_*
 * (mm/s^2), gyro_* (mrad/s), mag_* (nT) and the fused rotation vector rv_* (a quaternion x 10^6, Android's East-North-
 * Up). So inside every application (etc/inject.conf: "*app", the kernel's name for a program in an .app bundle)
 * CMMotionManager is answered here: each sensor is available, and its updates -- pushed to a queue at the interval
 * asked, or pulled from accelerometerData, gyroData, magnetometerData and deviceMotion -- are made from the sysctls.
 *
 * The axes are the same on both (x right, y up, z out of the screen); what differs is the accelerometer's sign and
 * unit: Android gives the push that holds the phone up (+9.8 m/s^2 on z lying face up), iOS the pull of gravity in g
 * (-1 on z). Device motion: the attitude is the rotation vector's quaternion; gravity and user acceleration are the
 * accelerometer split by a low-pass filter; the magnetic field is the magnetometer's, its accuracy the rotation
 * vector's calibration status.
 */
#import <CoreMotion/CoreMotion.h>
#import <Foundation/Foundation.h>
#include <math.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <sys/sysctl.h>

static int
value(const char *name)
{
	int v = 0;
	size_t size = sizeof(v);

	sysctlbyname(name, &v, &size, NULL, 0);
	return v;
}

static BOOL
sensors_there(void)
{
	static int there = -1;

	if (there < 0) {
		int v = 0;
		size_t size = sizeof(v);

		there = sysctlbyname("kern.hakios.accel_samples", &v, &size, NULL, 0) == 0;
	}
	return there;
}

static NSTimeInterval
uptime(void)
{
	return NSProcessInfo.processInfo.systemUptime;
}

static CMAcceleration
read_acceleration(void)
{
	/* (Android's push up in mm/s^2, iOS's pull down in g) */
	return (CMAcceleration){ -value("kern.hakios.accel_x") / 9806.65, -value("kern.hakios.accel_y") / 9806.65,
		-value("kern.hakios.accel_z") / 9806.65 };
}

static CMRotationRate
read_rotation(void)
{
	return (CMRotationRate){ value("kern.hakios.gyro_x") / 1000.0, value("kern.hakios.gyro_y") / 1000.0,
		value("kern.hakios.gyro_z") / 1000.0 };
}

static CMMagneticField
read_field(void)
{
	return (CMMagneticField){ value("kern.hakios.mag_x") / 1000.0, value("kern.hakios.mag_y") / 1000.0,
		value("kern.hakios.mag_z") / 1000.0 };
}

/* ---- the samples: CoreMotion's own classes, their values ours ---- */

@interface HKAccelerometerData : CMAccelerometerData
@property (nonatomic) CMAcceleration hk;
@property (nonatomic) NSTimeInterval hkTime;
@end

@implementation HKAccelerometerData
- (CMAcceleration)acceleration { return self.hk; }
- (NSTimeInterval)timestamp { return self.hkTime; }
@end

@interface HKGyroData : CMGyroData
@property (nonatomic) CMRotationRate hk;
@property (nonatomic) NSTimeInterval hkTime;
@end

@implementation HKGyroData
- (CMRotationRate)rotationRate { return self.hk; }
- (NSTimeInterval)timestamp { return self.hkTime; }
@end

@interface HKMagnetometerData : CMMagnetometerData
@property (nonatomic) CMMagneticField hk;
@property (nonatomic) NSTimeInterval hkTime;
@end

@implementation HKMagnetometerData
- (CMMagneticField)magneticField { return self.hk; }
- (NSTimeInterval)timestamp { return self.hkTime; }
@end

@interface HKAttitude : CMAttitude
@property (nonatomic) CMQuaternion hk;
@end

@implementation HKAttitude
- (CMQuaternion)quaternion { return self.hk; }
- (CMRotationMatrix)rotationMatrix
{
	CMQuaternion q = self.hk;

	return (CMRotationMatrix){
		1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y + q.z * q.w), 2 * (q.x * q.z - q.y * q.w),
		2 * (q.x * q.y - q.z * q.w), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z + q.x * q.w),
		2 * (q.x * q.z + q.y * q.w), 2 * (q.y * q.z - q.x * q.w), 1 - 2 * (q.x * q.x + q.y * q.y),
	};
}
- (double)roll
{
	CMQuaternion q = self.hk;

	return atan2(-2 * (q.x * q.z - q.w * q.y), 1 - 2 * (q.x * q.x + q.y * q.y));
}
- (double)pitch
{
	CMQuaternion q = self.hk;
	double s = 2 * (q.w * q.x + q.y * q.z);

	return asin(s > 1 ? 1 : s < -1 ? -1 : s);
}
- (double)yaw
{
	CMQuaternion q = self.hk;

	return atan2(2 * (q.w * q.z - q.x * q.y), 1 - 2 * (q.x * q.x + q.z * q.z));
}
- (void)multiplyByInverseOfAttitude:(CMAttitude *)attitude
{
	CMQuaternion a = self.hk, b = attitude.quaternion;

	/* (a times b's conjugate) */
	b.x = -b.x; b.y = -b.y; b.z = -b.z;
	self.hk = (CMQuaternion){
		a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
		a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
		a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
	};
}
- (id)copyWithZone:(NSZone *)zone
{
	HKAttitude *c = [HKAttitude new];

	c.hk = self.hk;
	return c;
}
@end

@interface HKDeviceMotion : CMDeviceMotion
@property (nonatomic, strong) HKAttitude *hkAttitude;
@property (nonatomic) CMRotationRate hkRotation;
@property (nonatomic) CMAcceleration hkGravity, hkUser;
@property (nonatomic) CMCalibratedMagneticField hkField;
@property (nonatomic) NSTimeInterval hkTime;
@end

@implementation HKDeviceMotion
- (CMAttitude *)attitude { return self.hkAttitude; }
- (CMRotationRate)rotationRate { return self.hkRotation; }
- (CMAcceleration)gravity { return self.hkGravity; }
- (CMAcceleration)userAcceleration { return self.hkUser; }
- (CMCalibratedMagneticField)magneticField { return self.hkField; }
- (double)heading
{
	double yaw = self.hkAttitude.yaw * 180 / M_PI;

	return fmod(360 - yaw + 360, 360);
}
- (CMDeviceMotionSensorLocation)sensorLocation { return CMDeviceMotionSensorLocationDefault; }
- (NSTimeInterval)timestamp { return self.hkTime; }
@end

/* ---- a manager's updates ---- */

enum { ACCEL, GYRO, MAG, MOTION, KINDS };

@interface HKFeed : NSObject {
@public
	dispatch_source_t timer[KINDS];
	BOOL active[KINDS];
	CMAcceleration gravity;
	BOOL gravitySet;
}
@end

@implementation HKFeed
@end

static const void *feed_key = &feed_key;

static HKFeed *
feed_of(CMMotionManager *m)
{
	HKFeed *f = objc_getAssociatedObject(m, feed_key);

	if (f == nil) {
		f = [HKFeed new];
		objc_setAssociatedObject(m, feed_key, f, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	}
	return f;
}

static id
sample(CMMotionManager *m, int kind)
{
	NSTimeInterval now = uptime();

	switch (kind) {
	case ACCEL: {
		HKAccelerometerData *d = [HKAccelerometerData new];

		d.hk = read_acceleration();
		d.hkTime = now;
		return d;
	}
	case GYRO: {
		HKGyroData *d = [HKGyroData new];

		d.hk = read_rotation();
		d.hkTime = now;
		return d;
	}
	case MAG: {
		HKMagnetometerData *d = [HKMagnetometerData new];

		d.hk = read_field();
		d.hkTime = now;
		return d;
	}
	default: {
		HKFeed *f = feed_of(m);
		HKDeviceMotion *d = [HKDeviceMotion new];
		HKAttitude *a = [HKAttitude new];
		CMAcceleration total = read_acceleration(), g = f->gravity;
		int status = value("kern.hakios.rv_status");

		/* gravity: the acceleration through a low-pass filter (some 0.2 s at 50 Hz), the rest the user's */
		if (!f->gravitySet) {
			g = total;
			f->gravitySet = YES;
		} else {
			g.x += (total.x - g.x) * 0.1;
			g.y += (total.y - g.y) * 0.1;
			g.z += (total.z - g.z) * 0.1;
		}
		f->gravity = g;
		a.hk = (CMQuaternion){ value("kern.hakios.rv_x") / 1e6, value("kern.hakios.rv_y") / 1e6,
			value("kern.hakios.rv_z") / 1e6, value("kern.hakios.rv_w") / 1e6 };
		if (a.hk.x == 0 && a.hk.y == 0 && a.hk.z == 0 && a.hk.w == 0) {
			a.hk = (CMQuaternion){ 0, 0, 0, 1 };
		}
		d.hkAttitude = a;
		d.hkRotation = read_rotation();
		d.hkGravity = g;
		d.hkUser = (CMAcceleration){ total.x - g.x, total.y - g.y, total.z - g.z };
		d.hkField = (CMCalibratedMagneticField){ read_field(),
			status >= 3 ? CMMagneticFieldCalibrationAccuracyHigh : status == 2 ? CMMagneticFieldCalibrationAccuracyMedium :
			status == 1 ? CMMagneticFieldCalibrationAccuracyLow : CMMagneticFieldCalibrationAccuracyUncalibrated };
		d.hkTime = now;
		return d;
	}
	}
}

static NSTimeInterval
interval_of(CMMotionManager *m, int kind)
{
	NSTimeInterval i = kind == ACCEL ? m.accelerometerUpdateInterval : kind == GYRO ? m.gyroUpdateInterval :
	    kind == MAG ? m.magnetometerUpdateInterval : m.deviceMotionUpdateInterval;

	return i < 0.01 ? 0.01 : i > 1 ? 1 : i;
}

static void
start(CMMotionManager *m, int kind, NSOperationQueue *queue, id handler)
{
	HKFeed *f = feed_of(m);
	dispatch_source_t t;
	__weak CMMotionManager *weak = m;
	id block = [handler copy];
	static const char *const names[] = { "accelerometer", "gyroscope", "magnetometer", "device motion" };

	if (f->timer[kind] != nil) {
		dispatch_source_cancel(f->timer[kind]);
	}
	f->active[kind] = YES;
	if (kind == MOTION) {
		f->gravitySet = NO;
	}
	NSLog(@"hakios.motion: %s updates every %.0f ms%s", names[kind], interval_of(m, kind) * 1000,
	    block != nil ? ", to a queue" : ", pulled");
	if (block == nil) {
		/* (pulled: device motion still needs its filter fed) */
		if (kind != MOTION) {
			return;
		}
	}
	t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
	dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, 0), (uint64_t)(interval_of(m, kind) * NSEC_PER_SEC),
	    NSEC_PER_MSEC);
	dispatch_source_set_event_handler(t, ^{
		CMMotionManager *strong = weak;
		id data;

		if (strong == nil) {
			return;
		}
		data = sample(strong, kind);
		if (block != nil) {
			NSOperationQueue *q = queue ?: NSOperationQueue.mainQueue;

			[q addOperationWithBlock:^{
				((void (^)(id, NSError *))block)(data, nil);
			}];
		} else {
			objc_setAssociatedObject(strong, (const void *)(uintptr_t)(kind + 1), data,
			    OBJC_ASSOCIATION_RETAIN_NONATOMIC);
		}
	});
	f->timer[kind] = t;
	dispatch_resume(t);
}

static void
stop(CMMotionManager *m, int kind)
{
	HKFeed *f = feed_of(m);

	if (f->timer[kind] != nil) {
		dispatch_source_cancel(f->timer[kind]);
		f->timer[kind] = nil;
	}
	f->active[kind] = NO;
}

static id
pulled(CMMotionManager *m, int kind)
{
	if (!feed_of(m)->active[kind]) {
		return nil;
	}
	if (kind == MOTION) {
		return objc_getAssociatedObject(m, (const void *)(uintptr_t)(kind + 1)) ?: sample(m, kind);
	}
	return sample(m, kind);
}

/* ---- CMMotionManager's methods, replaced ---- */

static void
replace(Class c, SEL sel, IMP imp)
{
	Method m = class_getInstanceMethod(c, sel);

	if (m != NULL) {
		method_setImplementation(m, imp);
	} else {
		NSLog(@"hakios.motion: CMMotionManager has no %s", sel_getName(sel));
	}
}

static BOOL yes(id self, SEL _cmd) { return YES; }

static BOOL accelActive(CMMotionManager *self, SEL _cmd) { return feed_of(self)->active[ACCEL]; }
static BOOL gyroActive(CMMotionManager *self, SEL _cmd) { return feed_of(self)->active[GYRO]; }
static BOOL magActive(CMMotionManager *self, SEL _cmd) { return feed_of(self)->active[MAG]; }
static BOOL motionActive(CMMotionManager *self, SEL _cmd) { return feed_of(self)->active[MOTION]; }

static void accelStart(CMMotionManager *self, SEL _cmd) { start(self, ACCEL, nil, nil); }
static void accelStartQ(CMMotionManager *self, SEL _cmd, NSOperationQueue *q, id h) { start(self, ACCEL, q, h); }
static void accelStop(CMMotionManager *self, SEL _cmd) { stop(self, ACCEL); }
static id accelData(CMMotionManager *self, SEL _cmd) { return pulled(self, ACCEL); }

static void gyroStart(CMMotionManager *self, SEL _cmd) { start(self, GYRO, nil, nil); }
static void gyroStartQ(CMMotionManager *self, SEL _cmd, NSOperationQueue *q, id h) { start(self, GYRO, q, h); }
static void gyroStop(CMMotionManager *self, SEL _cmd) { stop(self, GYRO); }
static id gyroData(CMMotionManager *self, SEL _cmd) { return pulled(self, GYRO); }

static void magStart(CMMotionManager *self, SEL _cmd) { start(self, MAG, nil, nil); }
static void magStartQ(CMMotionManager *self, SEL _cmd, NSOperationQueue *q, id h) { start(self, MAG, q, h); }
static void magStop(CMMotionManager *self, SEL _cmd) { stop(self, MAG); }
static id magData(CMMotionManager *self, SEL _cmd) { return pulled(self, MAG); }

static void motionStart(CMMotionManager *self, SEL _cmd) { start(self, MOTION, nil, nil); }
static void motionStartFrame(CMMotionManager *self, SEL _cmd, CMAttitudeReferenceFrame frame) { start(self, MOTION, nil, nil); }
static void motionStartQ(CMMotionManager *self, SEL _cmd, NSOperationQueue *q, id h) { start(self, MOTION, q, h); }
static void motionStartFrameQ(CMMotionManager *self, SEL _cmd, CMAttitudeReferenceFrame frame, NSOperationQueue *q, id h)
{
	start(self, MOTION, q, h);
}
static void motionStop(CMMotionManager *self, SEL _cmd) { stop(self, MOTION); }
static id motionData(CMMotionManager *self, SEL _cmd) { return pulled(self, MOTION); }

static NSUInteger frames(Class self, SEL _cmd)
{
	return CMAttitudeReferenceFrameXArbitraryZVertical | CMAttitudeReferenceFrameXArbitraryCorrectedZVertical |
	    CMAttitudeReferenceFrameXMagneticNorthZVertical | CMAttitudeReferenceFrameXTrueNorthZVertical;
}

__attribute__((constructor)) static void
motion_init(void)
{
	Class c = objc_getClass("CMMotionManager");

	if (c == nil || !sensors_there()) {
		return;
	}
	@autoreleasepool {
		replace(c, @selector(isAccelerometerAvailable), (IMP)yes);
		replace(c, @selector(isGyroAvailable), (IMP)yes);
		replace(c, @selector(isMagnetometerAvailable), (IMP)yes);
		replace(c, @selector(isDeviceMotionAvailable), (IMP)yes);
		replace(c, @selector(isAccelerometerActive), (IMP)accelActive);
		replace(c, @selector(isGyroActive), (IMP)gyroActive);
		replace(c, @selector(isMagnetometerActive), (IMP)magActive);
		replace(c, @selector(isDeviceMotionActive), (IMP)motionActive);
		replace(c, @selector(startAccelerometerUpdates), (IMP)accelStart);
		replace(c, @selector(startAccelerometerUpdatesToQueue:withHandler:), (IMP)accelStartQ);
		replace(c, @selector(stopAccelerometerUpdates), (IMP)accelStop);
		replace(c, @selector(accelerometerData), (IMP)accelData);
		replace(c, @selector(startGyroUpdates), (IMP)gyroStart);
		replace(c, @selector(startGyroUpdatesToQueue:withHandler:), (IMP)gyroStartQ);
		replace(c, @selector(stopGyroUpdates), (IMP)gyroStop);
		replace(c, @selector(gyroData), (IMP)gyroData);
		replace(c, @selector(startMagnetometerUpdates), (IMP)magStart);
		replace(c, @selector(startMagnetometerUpdatesToQueue:withHandler:), (IMP)magStartQ);
		replace(c, @selector(stopMagnetometerUpdates), (IMP)magStop);
		replace(c, @selector(magnetometerData), (IMP)magData);
		replace(c, @selector(startDeviceMotionUpdates), (IMP)motionStart);
		replace(c, @selector(startDeviceMotionUpdatesUsingReferenceFrame:), (IMP)motionStartFrame);
		replace(c, @selector(startDeviceMotionUpdatesToQueue:withHandler:), (IMP)motionStartQ);
		replace(c, @selector(startDeviceMotionUpdatesUsingReferenceFrame:toQueue:withHandler:), (IMP)motionStartFrameQ);
		replace(c, @selector(stopDeviceMotionUpdates), (IMP)motionStop);
		replace(c, @selector(deviceMotion), (IMP)motionData);
		{
			Method m = class_getClassMethod(c, @selector(availableAttitudeReferenceFrames));

			if (m != NULL) {
				method_setImplementation(m, (IMP)frames);
			}
		}
	}
}

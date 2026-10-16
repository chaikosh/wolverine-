/* CoreHaptics rumble on iPhone; gamepad rumble goes through SDL in platform.c. */
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#include "haptics.h"
#include "platform.h"
#if TARGET_OS_IPHONE
#import <CoreHaptics/CoreHaptics.h>
static CHHapticEngine *g_engine;
static id<CHHapticAdvancedPatternPlayer> g_player;
static int g_failed;

static int ensure_engine(void) {
    if (g_engine) return 1;
    if (g_failed || !CHHapticEngine.capabilitiesForHardware.supportsHaptics) { g_failed = 1; return 0; }
    NSError *err = nil;
    g_engine = [[CHHapticEngine alloc] initAndReturnError:&err];
    if (!g_engine) { g_failed = 1; return 0; }
    g_engine.playsHapticsOnly = YES;
    g_engine.autoShutdownEnabled = YES;
    g_engine.resetHandler = ^{ [g_engine startAndReturnError:nil]; };
    [g_engine startAndReturnError:&err];
    return 1;
}
#endif

void haptics_play(float intensity, float seconds) {
    plat_rumble(intensity, seconds);
#if TARGET_OS_IPHONE
    @autoreleasepool {
        if (!ensure_engine()) return;
        if (intensity <= 0) return;
        if (seconds <= 0 || seconds > 2) seconds = 2;
        CHHapticEventParameter *in = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:intensity];
        CHHapticEventParameter *sh = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.35f];
        CHHapticEvent *ev = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous parameters:@[in, sh]
                                                       relativeTime:0 duration:seconds];
        NSError *err = nil;
        CHHapticPattern *pat = [[CHHapticPattern alloc] initWithEvents:@[ev] parameters:@[] error:&err];
        if (!pat) return;
        [g_player stopAtTime:CHHapticTimeImmediate error:nil];
        g_player = [g_engine createAdvancedPlayerWithPattern:pat error:&err];
        [g_player startAtTime:CHHapticTimeImmediate error:&err];
    }
#endif
}

void haptics_stop(void) {
    plat_rumble(0, 0);
#if TARGET_OS_IPHONE
    [g_player stopAtTime:CHHapticTimeImmediate error:nil];
    g_player = nil;
#endif
}

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
static volatile int g_stopped = 1;   /* iOS stops the engine on its own (background, audio interruptions) */

static int ensure_engine(void) {
    if (g_engine) return 1;
    if (g_failed || !CHHapticEngine.capabilitiesForHardware.supportsHaptics) { g_failed = 1; return 0; }
    NSError *err = nil;
    g_engine = [[CHHapticEngine alloc] initAndReturnError:&err];
    if (!g_engine) { g_failed = 1; return 0; }
    g_engine.playsHapticsOnly = YES;
    g_engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) { (void)reason; g_stopped = 1; g_player = nil; };
    g_engine.resetHandler = ^{ g_stopped = 1; g_player = nil; };
    return 1;
}
static int engine_running(void) {
    if (!ensure_engine()) return 0;
    if (g_stopped) {
        NSError *err = nil;
        if (![g_engine startAndReturnError:&err]) return 0;
        g_stopped = 0;
    }
    return 1;
}
#endif

void haptics_play(float intensity, float seconds) {
    plat_rumble(intensity, seconds);
#if TARGET_OS_IPHONE
    @autoreleasepool {
        if (intensity <= 0 || !engine_running()) return;
        if (seconds <= 0 || seconds > 2) seconds = 2;
        CHHapticEventParameter *in = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:intensity];
        CHHapticEventParameter *sh = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.35f];
        CHHapticEvent *ev = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous parameters:@[in, sh]
                                                       relativeTime:0 duration:seconds];
        NSError *err = nil;
        CHHapticPattern *pat = [[CHHapticPattern alloc] initWithEvents:@[ev] parameters:@[] error:&err];
        if (!pat) return;
        @try {   /* CoreHaptics throws if the engine stopped in between: a missed buzz must never end the game */
            [g_player stopAtTime:CHHapticTimeImmediate error:nil];
            g_player = [g_engine createAdvancedPlayerWithPattern:pat error:&err];
            [g_player startAtTime:CHHapticTimeImmediate error:&err];
        } @catch (NSException *e) {
            g_stopped = 1; g_player = nil;
        }
    }
#endif
}

void haptics_stop(void) {
    plat_rumble(0, 0);
#if TARGET_OS_IPHONE
    @try { [g_player stopAtTime:CHHapticTimeImmediate error:nil]; } @catch (NSException *e) { g_stopped = 1; }
    g_player = nil;
#endif
}

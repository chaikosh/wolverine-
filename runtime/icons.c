/* Touch button pictograms drawn with CoreGraphics. Each icon is authored in a 100x100 box, y down. */
#include "icons.h"
#include <CoreGraphics/CoreGraphics.h>
#include <math.h>

static CGContextRef C;

static void ink(CGFloat g) { CGContextSetGrayFillColor(C, g, 1); CGContextSetGrayStrokeColor(C, g, 1); }
static void circle(CGFloat x, CGFloat y, CGFloat r) { CGContextFillEllipseInRect(C, CGRectMake(x - r, y - r, 2 * r, 2 * r)); }
static void rrect(CGFloat x0, CGFloat y0, CGFloat x1, CGFloat y1, CGFloat r) {
    CGPathRef p = CGPathCreateWithRoundedRect(CGRectMake(x0, y0, x1 - x0, y1 - y0), r, r, NULL);
    CGContextAddPath(C, p); CGContextFillPath(C); CGPathRelease(p);
}
static void seg(CGFloat x0, CGFloat y0, CGFloat x1, CGFloat y1, CGFloat w) {
    CGContextSetLineWidth(C, w); CGContextSetLineCap(C, kCGLineCapRound);
    CGContextMoveToPoint(C, x0, y0); CGContextAddLineToPoint(C, x1, y1); CGContextStrokePath(C);
}
static void tri(CGFloat ax, CGFloat ay, CGFloat bx, CGFloat by, CGFloat cx, CGFloat cy) {
    CGContextMoveToPoint(C, ax, ay); CGContextAddLineToPoint(C, bx, by); CGContextAddLineToPoint(C, cx, cy);
    CGContextClosePath(C); CGContextFillPath(C);
}

static void punch(void) {   /* fist seen from the front: knuckles, thumb across, wrist */
    rrect(20, 38, 82, 82, 15);
    for (int i = 0; i < 4; i++) rrect(21 + i * 15.3, 22, 35 + i * 15.3, 54, 7);
    ink(0);
    for (int i = 1; i < 4; i++) seg(20.2 + i * 15.3, 34, 20.2 + i * 15.3, 52, 2.4);
    seg(24, 64, 66, 64, 18);
    ink(1);
    seg(26, 64, 64, 64, 12);
    rrect(32, 85, 70, 94, 3);
}

static void kick(void) {    /* boot driving forward, speed lines behind */
    seg(43, 16, 49, 46, 19);
    CGContextMoveToPoint(C, 37, 42);
    CGContextAddLineToPoint(C, 60, 42);
    CGContextAddLineToPoint(C, 67, 53);
    CGContextAddQuadCurveToPoint(C, 84, 55, 91, 64);
    CGContextAddQuadCurveToPoint(C, 96, 74, 86, 80);
    CGContextAddLineToPoint(C, 40, 80);
    CGContextAddQuadCurveToPoint(C, 33, 80, 33, 72);
    CGContextClosePath(C); CGContextFillPath(C);
    ink(0); seg(36, 86, 90, 86, 4); ink(1);
    rrect(36, 84, 88, 90, 3);
    seg(6, 30, 22, 30, 5);
    seg(4, 46, 24, 46, 5);
    seg(8, 62, 24, 62, 5);
}

static void jump(void) {    /* arrow up off the ground */
    tri(50, 8, 82, 42, 18, 42);
    rrect(40, 36, 60, 70, 3);
    rrect(16, 80, 84, 90, 5);
}

static void action(void) {  /* open hand: grab / use / strike */
    rrect(28, 46, 75, 90, 15);
    const CGFloat fx[4] = {33.5, 45, 56.5, 68}, top[4] = {24, 12, 14, 26};
    for (int i = 0; i < 4; i++) seg(fx[i], 54, fx[i], top[i], 9.6);
    seg(31, 72, 14, 50, 11);
}

static void blade(CGFloat xb, CGFloat tx, CGFloat ty) {
    CGFloat lean = (tx - xb) * 0.35;
    CGContextMoveToPoint(C, xb - 5.5, 80);
    CGContextAddQuadCurveToPoint(C, xb - 5 + lean, 44, tx, ty);
    CGContextAddQuadCurveToPoint(C, xb + 5 + lean, 44, xb + 5.5, 80);
    CGContextClosePath(C); CGContextFillPath(C);
}
static void claws(void) {   /* three adamantium claws out of the knuckles */
    blade(31, 19, 12);
    blade(50, 50, 7);
    blade(69, 81, 12);
    rrect(17, 76, 83, 95, 9);
    ink(0);
    seg(40.5, 77, 40.5, 85, 2.6);
    seg(59.5, 77, 59.5, 85, 2.6);
}

static void crouch(void) {  /* arrow down to the ground */
    rrect(40, 8, 60, 42, 3);
    tri(50, 72, 82, 38, 18, 38);
    rrect(16, 80, 84, 90, 5);
}

static void senses(void) {  /* eye: feral senses */
    CGContextMoveToPoint(C, 4, 50);
    CGContextAddQuadCurveToPoint(C, 50, 4, 96, 50);
    CGContextAddQuadCurveToPoint(C, 50, 96, 4, 50);
    CGContextFillPath(C);
    ink(0);
    CGContextMoveToPoint(C, 15, 50);
    CGContextAddQuadCurveToPoint(C, 50, 17, 85, 50);
    CGContextAddQuadCurveToPoint(C, 50, 83, 15, 50);
    CGContextFillPath(C);
    ink(1); circle(50, 50, 17);
    ink(0); circle(50, 50, 7);
    ink(1); circle(56, 44, 3);
}

static void foot(CGFloat x, CGFloat y, CGFloat deg, CGFloat side) {
    CGContextSaveGState(C);
    CGContextTranslateCTM(C, x, y);
    CGContextRotateCTM(C, deg * (CGFloat)M_PI / 180);
    CGContextScaleCTM(C, 1.3, 1.3);
    CGContextFillEllipseInRect(C, CGRectMake(-9.5, -12, 19, 26));   /* ball of the foot */
    CGContextFillEllipseInRect(C, CGRectMake(-7, 10, 14, 13));      /* heel */
    circle(-6 * side, -18, 4.6);
    circle(1 * side, -20.5, 3.6);
    circle(6.5 * side, -18.5, 3.2);
    circle(10.5 * side, -14.5, 2.8);
    circle(12.5 * side, -9.5, 2.4);
    CGContextRestoreGState(C);
}
static void stealth(void) { /* quiet footsteps */
    foot(31, 61, -12, -1);
    foot(66, 39, 12, 1);
}

static void arc_arrow(CGFloat a0, CGFloat a1) {
    const CGFloat cx = 50, cy = 50, r = 29;
    CGContextSetLineWidth(C, 9); CGContextSetLineCap(C, kCGLineCapButt);
    CGContextAddArc(C, cx, cy, r, a0, a1 - 0.25, 0);
    CGContextStrokePath(C);
    CGFloat px = cx + r * cos(a1 - 0.25), py = cy + r * sin(a1 - 0.25);
    CGFloat tx = -sin(a1), ty = cos(a1), nx = cos(a1), ny = sin(a1);
    tri(px + tx * 14, py + ty * 14, px + nx * 11 - tx * 1, py + ny * 11 - ty * 1, px - nx * 11 - tx * 1, py - ny * 11 - ty * 1);
}
static void camera(void) {  /* re-centre the camera: circular arrows */
    const CGFloat d = (CGFloat)M_PI / 180;
    arc_arrow(195 * d, 345 * d);
    arc_arrow(15 * d, 165 * d);
    circle(50, 50, 7);
}

static void pause_icon(void) {
    rrect(28, 20, 44, 80, 4);
    rrect(56, 20, 72, 80, 4);
}

static void (*const DRAW[ICON_COUNT])(void) = {
    [ICON_PUNCH] = punch, [ICON_KICK] = kick, [ICON_JUMP] = jump, [ICON_ACTION] = action, [ICON_CLAWS] = claws,
    [ICON_CROUCH] = crouch, [ICON_SENSES] = senses, [ICON_STEALTH] = stealth, [ICON_CAMERA] = camera,
    [ICON_PAUSE] = pause_icon,
};

void icons_render(uint8_t *px, int cell) {
    int w = cell * ICON_COLS, h = cell * ICON_ROWS;
    CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
    C = CGBitmapContextCreate(px, (size_t)w, (size_t)h, 8, (size_t)w, gray, (CGBitmapInfo)kCGImageAlphaNone);
    CGColorSpaceRelease(gray);
    if (!C) return;
    ink(0); CGContextFillRect(C, CGRectMake(0, 0, w, h));
    CGContextTranslateCTM(C, 0, h); CGContextScaleCTM(C, 1, -1);   /* top-left origin, first buffer row = top */
    for (int i = 0; i < ICON_COUNT; i++) {
        CGContextSaveGState(C);
        CGContextTranslateCTM(C, (i % ICON_COLS) * cell, (i / ICON_COLS) * cell);
        CGContextScaleCTM(C, cell / 100.0, cell / 100.0);
        ink(1);
        DRAW[i]();
        CGContextRestoreGState(C);
    }
    CGContextRelease(C); C = NULL;
}

/*
 * DisplayView: paints the emulator's latest frame with CoreGraphics,
 * letterboxed to the chosen aspect. A CVDisplayLink provides the vsync
 * ticks that feed the session's phase-lock (the macOS analog of the GTK
 * frame clock / Qt frameSwapped loop) and schedules main-thread repaints.
 * Keyboard events go through KeyForward: the session's controller
 * bindings first (Ctrl+digit presses the game-controller keypad, as in the
 * other frontends), the ADAM keyboard for the rest.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DisplayView.h"

#import <CoreVideo/CoreVideo.h>

#import "KeyForward.h"

#include <time.h>

@implementation DisplayView {
    adamsession *_session;
    CVDisplayLinkRef _link;
    uint16_t *_fb;
    uint8_t *_rgba;
    uint64_t _serial;
    CGImageRef _image;
    int _aspectMode;
    BOOL _smooth;
}

static int64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static CVReturn link_cb(CVDisplayLinkRef link, const CVTimeStamp *now,
                        const CVTimeStamp *output, CVOptionFlags flagsIn,
                        CVOptionFlags *flagsOut, void *ctx)
{
    DisplayView *view = (__bridge DisplayView *)ctx;
    (void)link; (void)now; (void)output; (void)flagsIn; (void)flagsOut;
    [view onVsync];
    return kCVReturnSuccess;
}

- (instancetype)initWithSession:(adamsession *)session
{
    self = [super initWithFrame:NSMakeRect(0, 0, ADAMSESSION_FB_WIDTH * 2,
                                           ADAMSESSION_FB_HEIGHT * 2)];
    if (!self)
        return nil;
    _session = session;
    _fb = calloc((size_t)ADAMSESSION_FB_WIDTH * ADAMSESSION_FB_HEIGHT,
                 sizeof(uint16_t));
    _rgba = calloc((size_t)ADAMSESSION_FB_WIDTH * ADAMSESSION_FB_HEIGHT, 4);
    return self;
}

- (void)dealloc
{
    [self stop];
    if (_image)
        CGImageRelease(_image);
    free(_fb);
    free(_rgba);
}

/* CVDisplayLink is deprecated in macOS 15 in favor of NSView.displayLink
 * (macOS 14+), but the retro community runs plenty of older Intel Macs
 * that never see macOS 14 -- so the older API stays, deliberately. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
- (void)start
{
    if (_link)
        return;
    CVDisplayLinkCreateWithActiveCGDisplays(&_link);
    CVDisplayLinkSetOutputCallback(_link, link_cb, (__bridge void *)self);
    CVDisplayLinkStart(_link);
}

- (void)stop
{
    if (_link) {
        CVDisplayLinkStop(_link);
        CVDisplayLinkRelease(_link);
        _link = NULL;
    }
}
#pragma clang diagnostic pop

- (void)setAspectMode:(int)mode
{
    _aspectMode = mode;
    [self setNeedsDisplay:YES];
}

- (void)setSmooth:(BOOL)smooth
{
    _smooth = smooth;
    [self setNeedsDisplay:YES];
}

/* Display-link thread: feed the phase-lock, then hop to the main thread to
 * pull and paint whatever frame is latest. */
- (void)onVsync
{
    adamsession_notify_vsync(_session, monotonic_ns());
    dispatch_async(dispatch_get_main_queue(), ^{
        [self pullFrame];
    });
}

- (void)pullFrame
{
    if (!adamsession_copy_frame(_session, _fb, &_serial))
        return;

    const int n = ADAMSESSION_FB_WIDTH * ADAMSESSION_FB_HEIGHT;
    for (int i = 0; i < n; i++) {
        uint16_t p = _fb[i];
        uint8_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
        _rgba[i * 4 + 0] = (uint8_t)(r << 3 | r >> 2);
        _rgba[i * 4 + 1] = (uint8_t)(g << 2 | g >> 4);
        _rgba[i * 4 + 2] = (uint8_t)(b << 3 | b >> 2);
        _rgba[i * 4 + 3] = 0xFF;
    }

    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        NULL, _rgba, (size_t)n * 4, NULL);
    CGImageRef image = CGImageCreate(
        ADAMSESSION_FB_WIDTH, ADAMSESSION_FB_HEIGHT, 8, 32,
        (size_t)ADAMSESSION_FB_WIDTH * 4, space,
        kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault, provider,
        NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(space);

    if (_image)
        CGImageRelease(_image);
    _image = image;
    [self setNeedsDisplay:YES];
}

- (NSRect)destRect
{
    const CGFloat w = self.bounds.size.width;
    const CGFloat h = self.bounds.size.height;
    CGFloat dw, dh;

    if (_aspectMode == 2) { /* integer scale */
        int scale = (int)MIN(w / ADAMSESSION_FB_WIDTH,
                             h / ADAMSESSION_FB_HEIGHT);
        if (scale < 1)
            scale = 1;
        dw = scale * ADAMSESSION_FB_WIDTH;
        dh = scale * ADAMSESSION_FB_HEIGHT;
    } else {
        const CGFloat aspect =
            _aspectMode == 1 ? 4.0 / 3.0
                             : (CGFloat)ADAMSESSION_FB_WIDTH /
                                   ADAMSESSION_FB_HEIGHT;
        if (w / h > aspect) {
            dh = h;
            dw = h * aspect;
        } else {
            dw = w;
            dh = w / aspect;
        }
    }
    return NSMakeRect((w - dw) / 2.0, (h - dh) / 2.0, dw, dh);
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    [[NSColor blackColor] setFill];
    NSRectFill(self.bounds);
    if (!_image)
        return;

    CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
    CGContextSetInterpolationQuality(
        ctx, _smooth ? kCGInterpolationLow : kCGInterpolationNone);
    CGContextDrawImage(ctx, NSRectToCGRect([self destRect]), _image);
}

/* ---- input ---------------------------------------------------------------- */

- (BOOL)acceptsFirstResponder
{
    return YES;
}

/* Losing the keyboard with keys held would leave the machine believing they
 * are still down. (Losing key status altogether is the window delegate's.) */
- (BOOL)resignFirstResponder
{
    ADAMKeyFocusLost(_session);
    return YES;
}

- (void)keyDown:(NSEvent *)event
{
    if (!ADAMKeyDown(_session, event))
        [super keyDown:event];
}

- (void)keyUp:(NSEvent *)event
{
    ADAMKeyUp(_session, event);
}

/* Modifier keys arrive as flagsChanged, not keyDown/keyUp; a binding moved
 * onto Shift, Control or Option would otherwise never press. */
- (void)flagsChanged:(NSEvent *)event
{
    ADAMFlagsChanged(_session, event);
}

@end

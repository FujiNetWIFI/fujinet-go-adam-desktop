/*
 * ADAMKeypadWindow -- the AppKit controllers panel.
 *
 * Buttons are an ADAMPadButton subclass that reports mouseDown and mouseUp
 * rather than an action on click, for the reason every frontend in this family
 * shares: the emulator samples the controller register once per frame, so a
 * value present only for the instant of a click falls between frames and a
 * polling game never sees it. A button here is HELD.
 *
 * mouseUp arrives even when the pointer has left the button (AppKit tracks
 * the drag for the view that got mouseDown), so dragging off cannot strand
 * the machine with a control held.
 *
 * Whatever holds a control -- the keyboard, a gamepad or a click here -- it
 * lights up in the accent colour, from adamsession_buttons_held.
 *
 * A utility panel of fixed size (no resize mask). Singleton, ordered out
 * rather than closed, so its position survives.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "KeypadWindow.h"

#import "../AccentColor.h"
#import "../KeyForward.h"

/* -2 idle, -1 armed and waiting for a target, >= 0 waiting for a key or
 * pad button for that target. */
static int g_mapState = -2;
static ADAMKeypadWindow *g_singleton;
static adamsession *g_session;

@interface ADAMKeypadWindow ()
- (void)pickTarget:(int)target;
@end

/* The panel forwards keys to the controller itself: it is the first
 * responder (no control takes focus), so its own keyDown: is what runs. */
@interface ADAMKeypadPanel : NSPanel
@end

@implementation ADAMKeypadPanel
- (void)keyDown:(NSEvent *)e { [g_singleton keyDown:e]; }
- (void)keyUp:(NSEvent *)e { [g_singleton keyUp:e]; }
- (void)flagsChanged:(NSEvent *)e { [g_singleton flagsChanged:e]; }
@end

/* NOT named `target`: NSControl already has a `target` property -- the object
 * an action is sent to -- and shadowing it with an int silently breaks the
 * control's own machinery. `padTarget` is the binding-table index. */
@interface ADAMPadButton : NSButton
@property (nonatomic) int padTarget;
@property (nonatomic, copy) NSString *face;
@property (nonatomic) BOOL down;    /* held by the mouse */
@property (nonatomic) BOOL lit;     /* accent look on */
- (void)setLitState:(BOOL)lit;
- (void)setFaceTitle:(NSString *)text;
@end

@implementation ADAMPadButton
- (BOOL)acceptsFirstMouse:(NSEvent *)e
{
    (void)e;
    return YES;
}

- (void)setLitState:(BOOL)lit
{
    if (lit == self.lit)
        return;
    self.lit = lit;
    ADAMSetButtonAccent(self, lit);
}

/* A plain title drops the accent's attributed one, so put it back. */
- (void)setFaceTitle:(NSString *)text
{
    self.title = text;
    if (self.lit)
        ADAMSetButtonAccent(self, YES);
}

- (void)mouseDown:(NSEvent *)e
{
    (void)e;
    if (g_mapState != -2) {
        [g_singleton pickTarget:self.padTarget];
        return;
    }
    self.down = YES;
    [self setLitState:YES];
    if (self.padTarget >= ADAM_TARGET_SYSACT(0))
        return; /* fires on release */
    adamsession_press(g_session, self.padTarget, 1);
}

- (void)mouseUp:(NSEvent *)e
{
    if (!self.down)
        return;
    self.down = NO;
    [self setLitState:NO];
    if (self.padTarget >= ADAM_TARGET_SYSACT(0)) {
        /* System actions fire on release, like a real button: pressing and
         * dragging off must not reset the machine. */
        const NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
        if (NSPointInRect(p, self.bounds))
            adamsession_sysaction(g_session,
                                  self.padTarget - ADAM_TARGET_SYSACT(0));
        return;
    }
    adamsession_press(g_session, self.padTarget, 0);
}
@end

#define KP_BTN_W   80.0
#define KP_BTN_H   34.0
#define KP_GAP      6.0
#define KP_MARGIN  12.0
#define KP_COLW    (KP_BTN_W + KP_GAP)
#define KP_PANEL_W (3 * KP_COLW - KP_GAP)
#define KP_HEAD_H  24.0
/* heading, keypad (4 rows), stick (3 rows), fire row, each with its gap */
#define KP_CTRL_H  (KP_HEAD_H + 4 * (KP_BTN_H + KP_GAP) + KP_GAP \
                    + 3 * (KP_BTN_H + KP_GAP) + KP_GAP + (KP_BTN_H + KP_GAP))

@implementation ADAMKeypadWindow {
    NSMutableArray<ADAMPadButton *> *_buttons;
    NSButton *_mapButton;
    NSTextField *_hint;
    NSTimer *_liveTimer;
}

static NSString *const kFace[ADAM_KEYPAD_KEYS] = {
    @"0", @"1", @"2", @"3", @"4", @"5", @"6", @"7", @"8", @"9", @"*", @"#"
};
/* The physical 3x4 layout: 1-9 in reading order, then * 0 #. */
static const int kOrder[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 11 };

- (ADAMPadButton *)buttonWithFace:(NSString *)face
                           target:(int)target
                            frame:(NSRect)frame
{
    ADAMPadButton *b = [[ADAMPadButton alloc] initWithFrame:frame];
    [b setTitle:face];
    [b setBezelStyle:NSBezelStyleRounded];
    b.padTarget = target;
    b.face = face;
    /* Not focusable: clicking a pad button must not steal the key window's
     * first responder, and tabbing through forty buttons is nobody's idea
     * of input. */
    [b setRefusesFirstResponder:YES];
    [_buttons addObject:b];
    return b;
}

- (CGFloat)buildController:(int)port
                  intoView:(NSView *)parent
                         x:(CGFloat)x0
                      topY:(CGFloat)topY
{
    CGFloat y = topY;

    NSTextField *title = [NSTextField
        labelWithString:[NSString stringWithFormat:@"Controller %d", port + 1]];
    [title setAlignment:NSTextAlignmentCenter];
    [title setFont:[NSFont boldSystemFontOfSize:12]];
    [title setFrame:NSMakeRect(x0, y - 18, KP_PANEL_W, 16)];
    [parent addSubview:title];
    y -= KP_HEAD_H;

    for (int i = 0; i < 12; i++) {
        const int key = kOrder[i];
        NSRect r = NSMakeRect(x0 + (i % 3) * KP_COLW,
                              y - (i / 3 + 1) * (KP_BTN_H + KP_GAP),
                              KP_BTN_W, KP_BTN_H);
        [parent addSubview:[self buttonWithFace:kFace[key]
                                         target:ADAM_TARGET_PORT(port, ADAM_ACT_KEYPAD + key)
                                          frame:r]];
    }
    y -= 4 * (KP_BTN_H + KP_GAP) + KP_GAP;

    [parent addSubview:[self buttonWithFace:@"▲"
        target:ADAM_TARGET_PORT(port, ADAM_ACT_UP)
         frame:NSMakeRect(x0 + KP_COLW, y - KP_BTN_H, KP_BTN_W, KP_BTN_H)]];
    [parent addSubview:[self buttonWithFace:@"◀"
        target:ADAM_TARGET_PORT(port, ADAM_ACT_LEFT)
         frame:NSMakeRect(x0, y - 2 * KP_BTN_H - KP_GAP, KP_BTN_W, KP_BTN_H)]];
    [parent addSubview:[self buttonWithFace:@"▶"
        target:ADAM_TARGET_PORT(port, ADAM_ACT_RIGHT)
         frame:NSMakeRect(x0 + 2 * KP_COLW, y - 2 * KP_BTN_H - KP_GAP,
                          KP_BTN_W, KP_BTN_H)]];
    [parent addSubview:[self buttonWithFace:@"▼"
        target:ADAM_TARGET_PORT(port, ADAM_ACT_DOWN)
         frame:NSMakeRect(x0 + KP_COLW, y - 3 * KP_BTN_H - 2 * KP_GAP,
                          KP_BTN_W, KP_BTN_H)]];
    y -= 3 * (KP_BTN_H + KP_GAP) + KP_GAP;

    [parent addSubview:[self buttonWithFace:@"Fire L"
        target:ADAM_TARGET_PORT(port, ADAM_ACT_FIRE_L)
         frame:NSMakeRect(x0, y - KP_BTN_H, KP_BTN_W + KP_COLW / 2, KP_BTN_H)]];
    [parent addSubview:[self buttonWithFace:@"Fire R"
        target:ADAM_TARGET_PORT(port, ADAM_ACT_FIRE_R)
         frame:NSMakeRect(x0 + KP_COLW + KP_COLW / 2, y - KP_BTN_H,
                          KP_BTN_W + KP_COLW / 2, KP_BTN_H)]];
    return y - KP_BTN_H - KP_GAP;
}

- (instancetype)init
{
    const CGFloat width = 4 * KP_MARGIN + 2 * KP_PANEL_W;
    const CGFloat height =
        2 * KP_MARGIN + KP_CTRL_H + 2 * KP_GAP + 2 * KP_BTN_H;

    NSPanel *win = [[ADAMKeypadPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, width, height)
                  styleMask:(NSWindowStyleMaskTitled |
                             NSWindowStyleMaskClosable |
                             NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [win setTitle:@"Controllers"];
    /* Floats above the machine's window and stays out of the way, which is
     * what a utility panel is for. */
    [win setLevel:NSFloatingWindowLevel];
    [win setReleasedWhenClosed:NO];
    [win setBecomesKeyOnlyIfNeeded:NO];

    self = [super initWithWindow:win];
    if (!self)
        return nil;
    [win setDelegate:self];

    _buttons = [NSMutableArray array];

    NSView *content = [win contentView];
    const CGFloat x1 = KP_MARGIN;
    const CGFloat x2 = KP_MARGIN + KP_PANEL_W + 2 * KP_MARGIN;
    const CGFloat top = height - KP_MARGIN;
    [self buildController:0 intoView:content x:x1 topY:top];
    CGFloat bottom = [self buildController:1 intoView:content x:x2 topY:top];

    /* The system actions: they fire on release. */
    bottom -= KP_GAP;
    [content addSubview:[self buttonWithFace:@"Reset Computer"
        target:ADAM_TARGET_SYSACT(ADAM_SYSACT_RESET_COMPUTER)
         frame:NSMakeRect(x1, bottom - KP_BTN_H, KP_PANEL_W, KP_BTN_H)]];
    [content addSubview:[self buttonWithFace:@"Reset Game"
        target:ADAM_TARGET_SYSACT(ADAM_SYSACT_RESET_GAME)
         frame:NSMakeRect(x2, bottom - KP_BTN_H, KP_PANEL_W, KP_BTN_H)]];
    bottom -= KP_BTN_H + KP_GAP;

    /* The Map row. */
    const CGFloat rowY = bottom - KP_BTN_H;
    _mapButton = [NSButton buttonWithTitle:@"Map"
                                    target:self
                                    action:@selector(toggleMap:)];
    [_mapButton setFrame:NSMakeRect(x1, rowY, 70, KP_BTN_H)];
    [_mapButton setRefusesFirstResponder:YES];
    [content addSubview:_mapButton];

    NSButton *clearButton = [NSButton buttonWithTitle:@"Clear"
                                         target:self
                                         action:@selector(clearBinding:)];
    [clearButton setFrame:NSMakeRect(x1 + 76, rowY, 70, KP_BTN_H)];
    [clearButton setRefusesFirstResponder:YES];
    [content addSubview:clearButton];

    NSButton *defaults = [NSButton buttonWithTitle:@"Defaults"
                                            target:self
                                            action:@selector(restoreDefaults:)];
    [defaults setFrame:NSMakeRect(x1 + 152, rowY, 90, KP_BTN_H)];
    [defaults setRefusesFirstResponder:YES];
    [content addSubview:defaults];

    _hint = [NSTextField labelWithString:@""];
    [_hint setFrame:NSMakeRect(x1 + 252, rowY + (KP_BTN_H - 18) / 2,
                               width - x1 - 252 - KP_MARGIN, 18)];
    [_hint setTextColor:[NSColor secondaryLabelColor]];
    [_hint setLineBreakMode:NSLineBreakByTruncatingTail];
    [content addSubview:_hint];

    [self refresh];
    return self;
}

/* ---- Map mode ---------------------------------------------------------------- */

- (void)setMapState:(int)state
{
    g_mapState = state;
    if (state >= 0)
        adamsession_gamepad_capture_begin(g_session);
    else
        adamsession_gamepad_capture_cancel(g_session);
    [self refresh];
}

- (void)pickTarget:(int)target
{
    _hint.stringValue = @"";
    [self setMapState:target];
}

- (void)toggleMap:(id)sender
{
    (void)sender;
    _hint.stringValue = @"";
    [self setMapState:(g_mapState == -2) ? -1 : -2];
}

/* Clears the picked target's key AND gamepad button. */
- (void)clearBinding:(id)sender
{
    (void)sender;
    if (g_mapState < 0) {
        [self setMapState:-1];
        _hint.stringValue = @"Click the control to clear";
        return;
    }
    const int target = g_mapState;
    adamsession_binding_set_key(g_session, target, 0, NULL, 0);
    adamsession_binding_set_button(g_session, target, ADAM_PAD_BTN_NONE, NULL, 0);
    [self setMapState:-1];
    _hint.stringValue =
        [NSString stringWithFormat:@"%s cleared", adam_target_name(target)];
}

- (void)restoreDefaults:(id)sender
{
    (void)sender;
    adamsession_bindings_reset(g_session);
    if (g_mapState >= 0)
        [self setMapState:-1];
    else
        [self refresh];
    _hint.stringValue = @"Default bindings restored";
}

/* A key or pad button was bound: stay armed, since remapping several in a
 * row is the normal case, and say what happened. */
- (void)finishBind:(int)target label:(const char *)label stolen:(const char *)stolen
{
    [self setMapState:-1];
    NSString *text = [NSString
        stringWithFormat:@"%s = %s", adam_target_name(target), label];
    if (stolen[0])
        text = [text stringByAppendingFormat:@" (taken from %s)", stolen];
    _hint.stringValue = text;
}

- (void)bindKey:(uint32_t)keysym
{
    char stolen[128], name[32];
    const int target = g_mapState;
    stolen[0] = '\0';
    name[0] = '\0';
    adamsession_binding_set_key(g_session, target, keysym, stolen,
                                (int)sizeof stolen);
    adamsession_keysym_name(keysym, name, (int)sizeof name);
    [self finishBind:target label:name stolen:stolen];
}

- (void)pollCapture
{
    int button = ADAM_PAD_BTN_NONE;
    if (g_mapState < 0)
        return;
    if (adamsession_gamepad_capture_poll(g_session, &button)) {
        char stolen[128];
        const int target = g_mapState;
        stolen[0] = '\0';
        adamsession_binding_set_button(g_session, target, button, stolen,
                                       (int)sizeof stolen);
        [self finishBind:target label:adam_pad_button_name(button) stolen:stolen];
    }
}

/* In Map mode every control shows its binding; otherwise its face. */
- (void)refresh
{
    ADAMSetButtonAccent(_mapButton, g_mapState != -2);
    if (g_mapState == -1 && _hint.stringValue.length == 0)
        _hint.stringValue = @"Click a control to remap";
    else if (g_mapState >= 0)
        _hint.stringValue = [NSString
            stringWithFormat:@"Press a key or gamepad button for %s",
                             adam_target_name(g_mapState)];

    for (ADAMPadButton *b in _buttons) {
        if (g_mapState != -2) {
            const adam_binding bind =
                adamsession_binding_get(g_session, b.padTarget);
            char key[32];
            key[0] = '\0';
            if (bind.keysym)
                adamsession_keysym_name(bind.keysym, key, (int)sizeof key);
            b.font = [NSFont systemFontOfSize:10];
            [b setFaceTitle:(key[0] ? [NSString stringWithUTF8String:key] : @"—")];
            /* The button shows the key; the tooltip has the pad button too. */
            [b setToolTip:[NSString
                              stringWithFormat:@"%s: %s, %s", adam_target_name(b.padTarget),
                                               key[0] ? key : "no key",
                                               bind.button != ADAM_PAD_BTN_NONE
                                                   ? adam_pad_button_name(bind.button)
                                                   : "no pad button"]];
            [b setLitState:(b.padTarget == g_mapState)];
        } else {
            b.font = [NSFont systemFontOfSize:NSFont.systemFontSize];
            [b setFaceTitle:b.face];
            [b setToolTip:nil];
            if (!b.down)
                [b setLitState:NO];
        }
    }
}

/* ---- the live part ----------------------------------------------------------- */

/* Twenty times a second while the panel is up: the gamepad capture in Map
 * mode, and what is held (keyboard, gamepad or click) otherwise. */
- (void)tick
{
    if (g_mapState >= 0) {
        [self pollCapture];
        return;
    }
    if (g_mapState != -2)
        return;
    const uint32_t held[2] = {
        adamsession_buttons_held(g_session, 0),
        adamsession_buttons_held(g_session, 1)
    };
    for (ADAMPadButton *b in _buttons) {
        if (b.down || b.padTarget >= ADAM_TARGET_SYSACT(0))
            continue;
        const int port = b.padTarget / ADAM_ACT_PER_PORT;
        const int act = b.padTarget % ADAM_ACT_PER_PORT;
        [b setLitState:(held[port] & (1u << act)) != 0];
    }
}

/* ---- the keyboard ------------------------------------------------------------- */

/* Keyboard here behaves exactly as in the main window, so typing drives the
 * machine whichever window is key -- except in Map mode, where the next key
 * pressed is the binding. */
- (void)keyDown:(NSEvent *)e
{
    if (g_mapState >= 0) {
        /* Cmd chords are the menus'. */
        if ([e isARepeat] || ([e modifierFlags] & NSEventModifierFlagCommand))
            return;
        const uint32_t ks = ADAMKeysymFromEvent(e);
        if (ks)
            [self bindKey:ks];
        return;
    }
    if (g_mapState == -1)
        return;
    ADAMKeyDown(g_session, e);
}

/* Releases always go through: releasing a key that is not held is a no-op,
 * and one pressed before Map was armed must not stay held. */
- (void)keyUp:(NSEvent *)e
{
    ADAMKeyUp(g_session, e);
}

/* Modifiers bind too (Shift, Control and Option are keys like any other). */
- (void)flagsChanged:(NSEvent *)e
{
    int down = 0;
    const uint32_t ks = ADAMKeysymFromFlagsChange(e, &down);
    if (!ks)
        return;
    if (g_mapState >= 0) {
        if (down)
            [self bindKey:ks];
        return;
    }
    if (g_mapState == -1 && down)
        return;
    adamsession_input_key(g_session, ks, down, 0);
}

/* ---- lifetime ------------------------------------------------------------------ */

/* Leaving the panel (closing it, or toggling it away) cancels Map and lets
 * go of everything held. */
- (void)hidePanel
{
    _hint.stringValue = @"";
    [self setMapState:-2];
    [_liveTimer invalidate];
    _liveTimer = nil;
    for (ADAMPadButton *b in _buttons) {
        if (!b.down)
            continue;
        b.down = NO;
        [b setLitState:NO];
        if (b.padTarget < ADAM_TARGET_SYSACT(0))
            adamsession_press(g_session, b.padTarget, 0);
    }
    ADAMKeyFocusLost(g_session);
}

- (void)showPanel
{
    [self refresh];
    if (!_liveTimer)
        _liveTimer = [NSTimer scheduledTimerWithTimeInterval:0.05
                                                     repeats:YES
                                                       block:^(NSTimer *t) {
                                                         (void)t;
                                                         [self tick];
                                                       }];
    [[self window] makeKeyAndOrderFront:nil];
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self hidePanel];
}

/* Losing key status with keys held would leave the machine believing they
 * are still down. */
- (void)windowDidResignKey:(NSNotification *)note
{
    (void)note;
    ADAMKeyFocusLost(g_session);
}

+ (void)toggleWithSession:(adamsession *)session
{
    g_session = session;
    if (!g_singleton)
        g_singleton = [[ADAMKeypadWindow alloc] init];

    if ([[g_singleton window] isVisible]) {
        /* Order out, do not close: the window's position survives. */
        [g_singleton hidePanel];
        [[g_singleton window] orderOut:nil];
    } else {
        [g_singleton showPanel];
    }
}

+ (BOOL)isVisible
{
    return g_singleton && [[g_singleton window] isVisible];
}

@end

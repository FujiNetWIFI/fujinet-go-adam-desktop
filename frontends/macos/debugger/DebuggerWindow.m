/*
 * Debugger window (AppKit) over this app's Z80 debugger engine, via
 * core/include/adamdebug.h. Mirrors the SMS and MSX siblings' macOS debuggers
 * (and the GTK, Qt and Win32 debuggers), tab for tab: Console, CPU & Memory,
 * Disassembly, VDP, Breakpoints, Memory Map & I/O, FujiNet.
 *
 * Opening the window attaches the engine, which stops the machine; closing
 * it (F12, the close button, the menu) detaches, and the machine runs on.
 * F12 in the machine's window and the View menu toggle it.
 *
 * A 100 ms NSTimer polls adamdebug_generation() and refreshes only the tab on
 * show, and only when the generation changed (a stop, a resume, a step, a
 * command, an edit). While the machine runs, that tab's live values refresh
 * twice a second (every fifth tick). After a step the window waits a moment
 * (~40 ms) for the stop to settle before it reads anything back; a longer run
 * (Frame, Step Out, Run to Cursor) is picked up by the poll when it stops.
 * adamdebug_set_stop_callback is not used: it fires on the emulator thread.
 *
 * Keys work anywhere in the window (a local key monitor, so the console's
 * field takes them too): F5 stop/run, F7 step, Shift+F7 frame (to the next
 * frame boundary), F8 step over, Shift+F8 step out, F9 toggle the breakpoint
 * on the selected disassembly line, F12 close.
 *
 * ADAM_DEBUGGER_TAB picks the tab shown first: an index (0 Console,
 * 1 CPU & Memory, 2 Disassembly, 3 VDP, 4 Breakpoints, 5 Memory Map & I/O,
 * 6 FujiNet) or the start of a tab's name ("vram" is the VDP tab).
 *
 * Highlights: the PC's line is the accent fill with black text (the accent
 * is light), an enabled breakpoint's marker red and a disabled one's grey,
 * the selected line (Run to Cursor) the system selection colour underlined,
 * labels and comments grey; the Run button is the accent while stopped.
 *
 * The VDP tab (nametable/pattern/sprite/palette pictures, decoded register
 * and status text, a VRAM hex editor) and the FujiNet tab (every AdamNet
 * packet between the ADAM and FujiNet, both ways, with hex) come over from
 * the older window, over adamvdp_* and adamdebug_fujinet_*.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"

#import "../AccentColor.h"

#include <float.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "adamdebug.h"

#define DISASM_WINDOW 48
#define MAX_BPS 64
#define NUM_REGS 17
#define NUM_FLAGS 6
#define MEM_ROWS 32            /* the hex dump: 16 bytes a row */
#define MEM_BYTES (MEM_ROWS * 16)
#define VRAM_ROWS 16
#define VRAM_PAGE (VRAM_ROWS * 16)
#define POKE_MAX 64
#define TRACE_MAX 512          /* FujiNet packets shown */
#define TAB_CONSOLE 0          /* the tabs, in order */
#define TAB_CPU 1
#define TAB_DISASM 2
#define TAB_VDP 3
#define TAB_BREAKS 4
#define TAB_IO 5
#define TAB_FUJINET 6
#define LIVE_TICKS 5           /* 5 x 100 ms: the live refresh while running */

static DebuggerWindow *g_debugger;

/* An enabled breakpoint's marker: the same red in every toolkit's debugger. */
static NSColor *breakpointRed(void)
{
    return [NSColor colorWithSRGBRed:0xE0 / 255.0 green:0x1B / 255.0 blue:0x24 / 255.0 alpha:1.0];
}

/* Disassembly text view: a click selects the clicked line (Run to Cursor);
 * a click on its gutter, or a double-click, toggles its breakpoint; the
 * wheel browses by instruction. */
@interface DasmTextView : NSTextView
@property (nonatomic, copy) void (^onClick)(int line, int column, NSInteger clicks);
@property (nonatomic, copy) void (^onScroll)(int rows);
@end

@implementation DasmTextView {
    CGFloat _wheel;     /* the wheel's fraction not yet scrolled */
}

- (void)mouseDown:(NSEvent *)event
{
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger idx = [self characterIndexForInsertionAtPoint:p];
    NSString *text = self.string;
    if (idx > text.length) idx = text.length;
    int line = 0;
    NSUInteger lineStart = 0;
    for (NSUInteger i = 0; i < idx; i++)
        if ([text characterAtIndex:i] == '\n') { line++; lineStart = i + 1; }
    if (self.onClick) self.onClick(line, (int)(idx - lineStart), event.clickCount);
}

- (void)scrollWheel:(NSEvent *)event
{
    /* a notch is a few rows; a trackpad's pixels are many */
    _wheel += event.hasPreciseScrollingDeltas ? -event.scrollingDeltaY / 8.0 : -event.scrollingDeltaY * 3.0;
    const int rows = (int)_wheel;
    if (rows == 0) return;
    _wheel -= rows;
    if (self.onScroll) self.onScroll(rows);
}
@end

static NSImage *imageFromRGBA(const uint8_t *rgba, int w, int h)
{
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, rgba, (size_t)w * (size_t)h * 4, NULL);
    CGImageRef cg = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, space,
                                  kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault, provider, NULL, false,
                                  kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(space);
    NSImage *img = [[NSImage alloc] initWithCGImage:cg size:NSMakeSize(w, h)];
    CGImageRelease(cg);
    return img;
}

@interface DebuggerWindow () <NSTabViewDelegate>
- (instancetype)initWithSession:(adamsession *)session;
- (void)refreshNow;
@end

@implementation DebuggerWindow {
    adamsession *_session;
    adamdebug *_dbg;
    NSWindow *_window;
    NSTimer *_tick;
    id _keyMonitor;
    unsigned _seenGen;
    BOOL _wasStopped;
    int _liveTicks;
    NSTabView *_tabs;

    NSButton *_runBtn;
    NSTextField *_status;

    NSTextView *_conOut;
    NSTextField *_conIn;
    NSMutableArray<NSString *> *_history;
    NSInteger _historyPos;

    NSTextField *_reg[NUM_REGS];
    NSButton *_flag[NUM_FLAGS];
    NSTextField *_beam;
    NSTextView *_mem;
    NSTextField *_memGoto, *_memWrite;
    int _memBase;               /* the dump's first address */
    int _memMark;               /* the address Go to marked, or -1 */

    NSButton *_followPc;
    NSTextField *_jump;
    DasmTextView *_disasm;
    uint16_t _disasmAddr;
    uint16_t _lineAddr[DISASM_WINDOW];
    int _lineCount;
    int _selected;              /* the selected line's address, or -1 */

    NSImageView *_ntView, *_patView, *_sprView, *_palView;
    NSPopUpButton *_patBank;
    NSTextView *_sprInfo;
    NSTextView *_vdpState;
    NSTextField *_vramEntry;
    NSTextField *_vramStatus;
    NSTextView *_vramView;
    uint16_t _vramBase;

    NSPopUpButton *_bpType;
    NSTextField *_bpStart, *_bpEnd, *_bpCond, *_bpErr;
    NSStackView *_bpList;
    NSString *_bpSig;

    NSTextView *_io;

    NSTextField *_fujiNote;
    NSTextView *_fujiText, *_fujiState;
    uint64_t _fujiLast;
    int _fujiCount;             /* what the log shows: -1 forces a rebuild */
}

#define NUM_FILE_KINDS 4
static const char *const kFileKinds[NUM_FILE_KINDS] = { "dis", "mem", "vram", "regs" };
static NSString *const kFileTitles[NUM_FILE_KINDS] = {
    @"Disassembly…", @"Memory (64K)…", @"VRAM…", @"Registers…"
};
static NSString *const kFileNames[NUM_FILE_KINDS] = {
    @"disassembly.asm", @"memory.bin", @"vram.bin", @"registers.txt"
};

/* ADAM_DEBUGGER_TAB takes an index or the start of one of these. */
#define NUM_TABS 7
static const char *const kTabNames[NUM_TABS] = {
    "console", "cpu & memory", "disassembly", "vdp", "breakpoints", "memory map & i/o", "fujinet"
};

/* ---- helpers --------------------------------------------------------------- */

/* The engine's text as a string: control bytes dropped (a console's CRs and
 * escapes), UTF-8 if it is, otherwise Latin-1. */
static NSString *cstr(const char *s)
{
    if (!s) return @"";
    const size_t n = strlen(s);
    char *b = malloc(n + 1);
    if (!b) return @"";
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 || c == '\n' || c == '\t') b[o++] = (char)c;
    }
    b[o] = '\0';
    NSString *r = [NSString stringWithUTF8String:b];
    if (!r) r = [[NSString alloc] initWithBytes:b length:o encoding:NSISOLatin1StringEncoding];
    free(b);
    return r ?: @"";
}

/* Pads a column to width (never cuts a longer one). */
static NSString *padTo(NSString *s, NSUInteger width)
{
    return s.length >= width ? s : [s stringByPaddingToLength:width withString:@" " startingAtIndex:0];
}

/* $hex, 0xhex, hexh, #dec, or bare hex. */
static BOOL parseNum(NSString *text, long *out)
{
    NSString *t = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    int base = 16;
    if ([t hasPrefix:@"$"]) t = [t substringFromIndex:1];
    else if ([t.lowercaseString hasPrefix:@"0x"]) t = [t substringFromIndex:2];
    else if ([t hasPrefix:@"#"]) { t = [t substringFromIndex:1]; base = 10; }
    else if (t.length > 1 && [t.lowercaseString hasSuffix:@"h"]) t = [t substringToIndex:t.length - 1];
    if (t.length == 0) return NO;
    char *end;
    long v = strtol(t.UTF8String, &end, base);
    if (*end) return NO;
    *out = v;
    return YES;
}

/* A value as the fields take it: a label; $hex, 0xhex, hexh, #dec or bare hex
 * digits (hex here, though the console's bare digits are decimal); anything
 * else (an expression, a register) through the engine. On failure the reason
 * is in *why. */
- (BOOL)evaluate:(NSString *)text into:(long *)out why:(NSString **)why
{
    NSString *t = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    long v;
    if (t.length == 0) {
        if (why) *why = @"nothing entered";
        return NO;
    }
    const int label = adamdebug_label_address(_dbg, t.UTF8String);
    if (label >= 0) {
        *out = label;
        return YES;
    }
    if (parseNum(t, &v)) {
        *out = v;
        return YES;
    }
    char err[160];
    err[0] = '\0';
    if (adamdebug_eval(_dbg, t.UTF8String, &v, err, (int)sizeof err) == 0) {
        *out = v;
        return YES;
    }
    NSString *e = cstr(err);
    if (why) *why = e.length ? e : @"not a number, label or expression";
    return NO;
}

/* An address: the above, within $0000-$FFFF. -1 when it is not one. */
- (int)resolveAddr:(NSString *)text why:(NSString **)why
{
    long v;
    if (![self evaluate:text into:&v why:why]) return -1;
    if (v < 0 || v > 0xFFFF) {
        if (why) *why = @"out of range ($0000-$FFFF)";
        return -1;
    }
    return (int)v;
}

static NSFont *monoFont(void)
{
    return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
}

static NSDictionary *monoAttrs(void)
{
    return @{ NSFontAttributeName: monoFont(), NSForegroundColorAttributeName: NSColor.textColor };
}

/* The accent fill with black on it (the accent is light): the PC's line, a
 * marked row. */
static NSDictionary *accentAttrs(void)
{
    return @{ NSBackgroundColorAttributeName: ADAMAccentColor(), NSForegroundColorAttributeName: NSColor.blackColor };
}

static NSTextView *monoView(NSScrollView **scrollOut, BOOL wrap)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = !wrap;
    NSTextView *view = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    view.editable = NO;
    view.richText = NO;
    view.font = monoFont();
    if (wrap) {
        view.autoresizingMask = NSViewWidthSizable;
    } else {
        view.horizontallyResizable = YES;
        view.textContainer.widthTracksTextView = NO;
        view.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    }
    scroll.documentView = view;
    *scrollOut = scroll;
    return view;
}

/* Whether a view's reader is at its end (a log then follows new lines). */
static BOOL atEnd(NSTextView *view)
{
    NSScrollView *scroll = view.enclosingScrollView;
    if (!scroll) return YES;
    return NSMaxY(scroll.contentView.documentVisibleRect) >= NSHeight(view.frame) - 4;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
static void setText(NSTextView *view, NSString *text, BOOL follow)
{
    if (![view.string isEqualToString:text]) {
        NSScrollView *scroll = view.enclosingScrollView;
        const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
        view.string = text;
        if (scroll && !follow) {
            [scroll.contentView scrollToPoint:at];
            [scroll reflectScrolledClipView:scroll.contentView];
        }
    }
    if (follow) [view scrollRangeToVisible:NSMakeRange(view.string.length, 0)];
}

/* The same for styled text, always where the reader left it. */
static void setStyledText(NSTextView *view, NSAttributedString *text)
{
    NSScrollView *scroll = view.enclosingScrollView;
    const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
    [view.textStorage setAttributedString:text];
    if (scroll) {
        [scroll.contentView scrollToPoint:at];
        [scroll reflectScrolledClipView:scroll.contentView];
    }
}

- (NSButton *)button:(NSString *)title action:(SEL)sel
{
    NSButton *b = [NSButton buttonWithTitle:title target:self action:sel];
    [b setRefusesFirstResponder:YES];
    return b;
}

- (NSTextField *)label:(NSString *)text
{
    return [NSTextField labelWithString:text];
}

- (NSTextField *)hint:(NSString *)text
{
    NSTextField *l = [self label:text];
    l.textColor = NSColor.secondaryLabelColor;
    /* a long hint gives way to a narrow window instead of widening it */
    l.lineBreakMode = NSLineBreakByTruncatingTail;
    [l setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                forOrientation:NSLayoutConstraintOrientationHorizontal];
    return l;
}

- (NSTextField *)field:(NSString *)placeholder width:(CGFloat)width action:(SEL)sel
{
    NSTextField *f = [[NSTextField alloc] init];
    f.placeholderString = placeholder;
    f.font = monoFont();
    f.target = self;
    f.action = sel;
    if (width > 0) [f.widthAnchor constraintEqualToConstant:width].active = YES;
    return f;
}

/* A pull-down button: `title` on its face, `items` underneath (each one's
 * tag is its index). */
- (NSPopUpButton *)pullDown:(NSString *)title items:(NSArray<NSString *> *)items action:(SEL)sel
{
    NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:YES];
    [p addItemWithTitle:title];
    p.lastItem.tag = -1;
    for (NSUInteger i = 0; i < items.count; i++) {
        [p addItemWithTitle:items[i]];
        p.lastItem.tag = (NSInteger)i;
    }
    p.target = self;
    p.action = sel;
    [p setRefusesFirstResponder:YES];
    return p;
}

/* A picture, scaled up crisp: nearest-neighbour, so the pixels stay square. */
- (NSImageView *)pixelView:(int)w height:(int)h
{
    NSImageView *v = [[NSImageView alloc] init];
    v.imageScaling = NSImageScaleAxesIndependently;
    v.wantsLayer = YES;
    v.layer.magnificationFilter = kCAFilterNearest;
    [v.widthAnchor constraintEqualToConstant:w].active = YES;
    [v.heightAnchor constraintEqualToConstant:h].active = YES;
    return v;
}

static NSStackView *hstack(NSArray<NSView *> *views)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    s.spacing = 6;
    return s;
}

static NSStackView *vstack(NSArray<NSView *> *views)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationVertical;
    s.alignment = NSLayoutAttributeLeading;
    s.spacing = 6;
    return s;
}

static void fill(NSStackView *stack, NSView *view)
{
    [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
}

/* ---- lifetime -------------------------------------------------------------- */

+ (void)showForSession:(adamsession *)session
{
    if (!g_debugger) g_debugger = [[DebuggerWindow alloc] initWithSession:session];
    [g_debugger show];
}

+ (void)toggleForSession:(adamsession *)session
{
    if (g_debugger && [g_debugger->_window isVisible]) {
        /* windowWillClose: detaches */
        [g_debugger->_window close];
        return;
    }
    [self showForSession:session];
}

- (void)show
{
    if (_window.isMiniaturized) [_window deminiaturize:nil];
    [_window makeKeyAndOrderFront:nil];
    const unsigned gen = adamdebug_generation(_dbg);
    adamdebug_attach(_dbg);
    [self settleFrom:gen];
    [self refreshNow];
    if ([self currentTab] == TAB_CONSOLE) [_window makeFirstResponder:_conIn];
}

- (instancetype)initWithSession:(adamsession *)session
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _dbg = adamsession_debugger(session);
    _fujiCount = -1;
    _memBase = 0xFC30;
    _memMark = -1;
    _selected = -1;
    _history = [NSMutableArray array];
    [self buildWindow];

    __weak DebuggerWindow *weakSelf = self;
    _tick = [NSTimer timerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *t) {
        (void)t;
        [weakSelf onTick];
    }];
    [[NSRunLoop mainRunLoop] addTimer:_tick forMode:NSRunLoopCommonModes];

    /* F5/F7/F8/F9/F12 wherever the focus is in this window, the console's
     * field included */
    _keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                        handler:^NSEvent *(NSEvent *event) {
        DebuggerWindow *s = weakSelf;
        if (!s || event.window != s->_window || !s->_window.isVisible) return event;
        return [s handleKey:event] ? nil : event;
    }];
    return self;
}

- (void)dealloc
{
    [_tick invalidate];
    if (_keyMonitor) [NSEvent removeMonitor:_keyMonitor];
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object != _window) return;
    adamdebug_detach(_dbg);
}

- (NSInteger)currentTab
{
    return [_tabs indexOfTabViewItem:_tabs.selectedTabViewItem];
}

/* The poll: refresh on a stop, a resume or a command (the generation), and
 * the visible tab's live values twice a second while the machine runs. The
 * FujiNet tab also follows the trace twice a second while stopped, as the
 * FujiNet's packets arrive on their own. */
- (void)onTick
{
    if (![_window isVisible]) return;
    const unsigned gen = adamdebug_generation(_dbg);
    const BOOL stopped = adamdebug_is_stopped(_dbg) != 0;
    if (gen != _seenGen || stopped != _wasStopped) {
        [self refreshNow];
    } else if (++_liveTicks >= LIVE_TICKS) {
        _liveTicks = 0;
        if (!stopped) {
            [self refreshStatus];
            [self refreshTab];
        } else if ([self currentTab] == TAB_FUJINET) {
            [self refreshFujinet:NO];
        }
    }
}

/* Waits up to ~40 ms for a stop to land after the engine was asked to move:
 * the generation moves on and the machine reads as stopped. */
- (void)settleFrom:(unsigned)gen
{
    for (int i = 0; i < 20; i++) {
        if (adamdebug_generation(_dbg) != gen && adamdebug_is_stopped(_dbg)) break;
        usleep(2000);
    }
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    (void)item;
    [self refreshTab];
    if ([self currentTab] == TAB_CONSOLE) [_window makeFirstResponder:_conIn];
}

/* ---- keys -------------------------------------------------------------------- */

- (BOOL)handleKey:(NSEvent *)event
{
    NSString *chars = event.charactersIgnoringModifiers;
    if (chars.length != 1) return NO;
    const NSEventModifierFlags mods = event.modifierFlags;
    if (mods & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption)) return NO;
    const BOOL shift = (mods & NSEventModifierFlagShift) != 0;
    switch ([chars characterAtIndex:0]) {
    case NSF5FunctionKey: [self toggleRun:nil]; return YES;
    case NSF7FunctionKey: if (shift) [self frame:nil]; else [self step:nil]; return YES;
    case NSF8FunctionKey: if (shift) [self stepOut:nil]; else [self stepOver:nil]; return YES;
    case NSF9FunctionKey: [self toggleSelected:nil]; return YES;
    case NSF12FunctionKey: [_window performClose:nil]; return YES;
    default: return NO;
    }
}

/* ---- construction ------------------------------------------------------------ */

- (NSView *)buildConsole
{
    NSScrollView *outScroll;
    _conOut = monoView(&outScroll, YES);
    _conOut.string = @"Debugger console. Type 'help' for every command; Tab completes, Up/Down walk the history.\n";
    _conIn = [self field:@"command (help, step, break $8000, bpw $6000, regs, mem $6000 32, ...) — Tab completes, ↑↓ history"
                   width:0 action:@selector(runCommand:)];
    _conIn.delegate = self;

    NSStackView *v = vstack(@[outScroll, _conIn]);
    fill(v, outScroll);
    fill(v, _conIn);
    return v;
}

- (NSView *)buildCpu
{
    static NSString *const names[NUM_REGS] = {
        @"PC", @"SP", @"AF", @"BC", @"DE", @"HL", @"IX", @"IY",
        @"AF'", @"BC'", @"DE'", @"HL'", @"I", @"R", @"IM", @"IFF1", @"IFF2"
    };
    NSMutableArray *row1 = [NSMutableArray array], *row2 = [NSMutableArray array];
    for (int i = 0; i < NUM_REGS; i++) {
        /* the 16-bit pairs, then I and R, then IM and the two flip-flops */
        const CGFloat width = i < 12 ? 52 : (i < 14 ? 36 : 28);
        NSMutableArray *row = i < 8 ? row1 : row2;
        [row addObject:[self label:names[i]]];
        _reg[i] = [self field:@"" width:width action:@selector(applyRegister:)];
        _reg[i].tag = i;
        [row addObject:_reg[i]];
    }
    static NSString *const fnames[NUM_FLAGS] = { @"S", @"Z", @"H", @"PV", @"N", @"C" };
    NSMutableArray *flags = [NSMutableArray arrayWithObject:[self label:@"Flags"]];
    for (int i = 0; i < NUM_FLAGS; i++) {
        _flag[i] = [NSButton checkboxWithTitle:fnames[i] target:self action:@selector(flagClicked:)];
        _flag[i].tag = i;
        [flags addObject:_flag[i]];
    }
    NSTextField *editHint = [self hint:@"Registers and flags can be edited while the machine is stopped (hex, $hex, a label or an expression)"];
    [flags addObject:editHint];
    _beam = [self hint:@""];

    NSScrollView *memScroll;
    _mem = monoView(&memScroll, NO);
    _memGoto = [self field:@"address or label" width:150 action:@selector(gotoMem:)];
    NSButton *prev = [self button:@"◀" action:@selector(memPrev:)];
    NSButton *next = [self button:@"▶" action:@selector(memNext:)];
    NSStackView *memHead = hstack(@[[self label:@"Memory (as the CPU sees it)"], [self label:@"Go to"], _memGoto,
                                    prev, next]);
    _memWrite = [self field:@"address value" width:170 action:@selector(writeMem:)];
    NSStackView *edit = hstack(@[[self label:@"Poke"], _memWrite,
                                 [self hint:@"RAM takes it; ROM ignores it. Several values go to consecutive addresses"]]);

    NSStackView *v = vstack(@[hstack(row1), hstack(row2), hstack(flags), _beam, memHead, memScroll, edit]);
    fill(v, memScroll);
    return v;
}

- (NSView *)buildDisasm
{
    _followPc = [NSButton checkboxWithTitle:@"Follow PC" target:self action:@selector(followToggled:)];
    _followPc.state = NSControlStateValueOn;
    _jump = [self field:@"address or label" width:150 action:@selector(jumpTo:)];
    NSStackView *row = hstack(@[_followPc, [self label:@"Jump to"], _jump,
                                [self hint:@"Click a line to select it (Run to Cursor); click its left edge, double-click it "
                                           @"or press F9 for a breakpoint (* on, o off); scroll to browse"]]);

    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = NO;
    scroll.hasHorizontalScroller = YES;
    _disasm = [[DasmTextView alloc] initWithFrame:NSMakeRect(0, 0, 600, 600)];
    _disasm.editable = NO;
    _disasm.richText = NO;
    _disasm.selectable = NO;
    _disasm.font = monoFont();
    _disasm.horizontallyResizable = YES;
    _disasm.textContainer.widthTracksTextView = NO;
    _disasm.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
    _disasm.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    __weak DebuggerWindow *weakSelf = self;
    _disasm.onClick = ^(int line, int column, NSInteger clicks) {
        DebuggerWindow *s = weakSelf;
        if (!s || line < 0 || line >= s->_lineCount) return;
        const BOOL gutter = column < 2;
        s->_selected = s->_lineAddr[line];
        if (gutter ? clicks == 1 : clicks == 2) [s toggleBreakpointAt:s->_lineAddr[line]];
        else [s refreshDisasm];
    };
    _disasm.onScroll = ^(int rows) {
        DebuggerWindow *s = weakSelf;
        if (!s || rows == 0) return;
        s->_followPc.state = NSControlStateValueOff;
        s->_disasmAddr = (uint16_t)adamdebug_row_address(s->_dbg, s->_disasmAddr, rows);
        [s refreshDisasm];
    };
    scroll.documentView = _disasm;

    NSStackView *v = vstack(@[row, scroll]);
    fill(v, row);
    fill(v, scroll);
    return v;
}

/* The pictures, the decoded registers and the VRAM editor: one page, which
 * scrolls when the window is smaller than it. */
- (NSView *)buildVdp
{
    _ntView = [self pixelView:512 height:384];
    /* 1:1 with the rendered swatch grid so the index labels stay crisp. */
    _palView = [self pixelView:ADAMVDP_PAL_W height:ADAMVDP_PAL_H];
    NSStackView *left = vstack(@[ [self label:@"Nametable"], _ntView, [self label:@"Palette"], _palView ]);

    _patView = [self pixelView:512 height:128];
    _patBank = [[NSPopUpButton alloc] init];
    [_patBank addItemsWithTitles:@[ @"Bank 0", @"Bank 1", @"Bank 2" ]];
    _patBank.target = self;
    _patBank.action = @selector(vdpViewChanged:);
    NSStackView *patBox = vstack(@[ hstack(@[ [self label:@"Patterns"], _patBank ]), _patView ]);

    _sprView = [self pixelView:256 height:128];
    NSScrollView *sprScroll;
    _sprInfo = monoView(&sprScroll, YES);
    [sprScroll.widthAnchor constraintEqualToConstant:300].active = YES;
    [sprScroll.heightAnchor constraintEqualToConstant:128].active = YES;
    NSStackView *sprBox = vstack(@[ [self label:@"Sprites"], hstack(@[ _sprView, sprScroll ]) ]);

    NSScrollView *stateScroll;
    _vdpState = monoView(&stateScroll, YES);
    [stateScroll.widthAnchor constraintEqualToConstant:560].active = YES;
    [stateScroll.heightAnchor constraintEqualToConstant:230].active = YES;
    NSStackView *stateBox = vstack(@[ [self label:@"Registers & status"], stateScroll ]);

    NSStackView *right = vstack(@[ patBox, sprBox, stateBox ]);
    right.spacing = 10;
    NSStackView *top = hstack(@[ left, right ]);
    top.alignment = NSLayoutAttributeTop;
    top.spacing = 12;

    _vramEntry = [self field:@"addr [bytes...] e.g. 1800: 41 42 43" width:300 action:@selector(vramEntryActivated:)];
    NSButton *vramPrev = [self button:@"◀" action:@selector(vramPagePrev:)];
    NSButton *vramNext = [self button:@"▶" action:@selector(vramPageNext:)];
    _vramStatus = [self label:@""];
    NSStackView *vramBar = hstack(@[ _vramEntry, vramPrev, vramNext, _vramStatus ]);
    NSScrollView *vramScroll;
    _vramView = monoView(&vramScroll, NO);
    [vramScroll.widthAnchor constraintEqualToConstant:640].active = YES;
    [vramScroll.heightAnchor constraintEqualToConstant:260].active = YES;
    NSStackView *vramBox = vstack(@[ [self label:@"VRAM (Enter writes; addresses wrap at 16K)"], vramBar, vramScroll ]);

    /* the page is the scroll view's document, pinned to its top left; its
     * own size is what its views ask for */
    NSStackView *page = vstack(@[ top, vramBox ]);
    page.spacing = 12;
    page.translatesAutoresizingMaskIntoConstraints = NO;
    NSScrollView *outer = [[NSScrollView alloc] init];
    outer.hasVerticalScroller = YES;
    outer.hasHorizontalScroller = YES;
    outer.drawsBackground = NO;
    outer.documentView = page;
    NSClipView *clip = outer.contentView;
    [page.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
    [page.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
    return outer;
}

/* One breakpoint-table cell: a fixed-width monospace label, or (width 0) the
 * flexible one that takes what is left. */
static NSTextField *bpCell(NSString *text, CGFloat width, BOOL bold)
{
    NSTextField *l = [NSTextField labelWithString:text];
    l.font = bold ? [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightSemibold] : monoFont();
    l.lineBreakMode = NSLineBreakByTruncatingTail;
    if (width > 0) {
        [l.widthAnchor constraintEqualToConstant:width].active = YES;
    } else {
        [l setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
        [l setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    }
    return l;
}

- (NSView *)buildBreaks
{
    /* the table's rows scroll: a session can collect more breakpoints than
     * the tab is tall. The stack is the scroll view's document, pinned to
     * the top and to the clip view's width so rows keep the tab's width. */
    NSStackView *header = hstack(@[bpCell(@"On  #", 70, YES), bpCell(@"Type", 60, YES), bpCell(@"Range", 190, YES),
                                   bpCell(@"Condition", 0, YES), bpCell(@"Hits", 60, YES), bpCell(@"", 70, YES)]);
    header.distribution = NSStackViewDistributionFill;
    _bpList = vstack(@[]);
    _bpList.translatesAutoresizingMaskIntoConstraints = NO;
    NSScrollView *bpScroll = [[NSScrollView alloc] init];
    bpScroll.hasVerticalScroller = YES;
    bpScroll.drawsBackground = NO;
    bpScroll.documentView = _bpList;
    NSClipView *clip = bpScroll.contentView;
    [_bpList.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
    [_bpList.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
    [_bpList.widthAnchor constraintEqualToAnchor:clip.widthAnchor].active = YES;

    _bpType = [[NSPopUpButton alloc] init];
    [_bpType addItemsWithTitles:@[@"exec", @"read", @"write", @"in", @"out"]];
    _bpStart = [self field:@"$8000, a label, or port $FF" width:190 action:@selector(addBreakpoint:)];
    _bpEnd = [self field:@"end (optional)" width:120 action:@selector(addBreakpoint:)];
    _bpCond = [self field:@"condition (optional): a == 5" width:0 action:@selector(addBreakpoint:)];
    NSButton *add = [self button:@"Add" action:@selector(addBreakpoint:)];
    NSButton *clear = [self button:@"Clear All" action:@selector(clearBreaks:)];
    NSStackView *row = hstack(@[_bpType, [self label:@"Start"], _bpStart, [self label:@"End"], _bpEnd,
                                [self label:@"Condition"], _bpCond, add, clear]);
    row.distribution = NSStackViewDistributionFill;
    _bpErr = [self label:@""];
    _bpErr.textColor = NSColor.systemRedColor;
    NSTextField *hint = [self hint:@"Untick to disable. A condition is an expression, e.g. a == 5 or, on read/write/in/out, "
                                   @"addr == $6000 && value > 3. A port is 0-255 for in/out; the end defaults to the start."];

    NSStackView *v = vstack(@[header, bpScroll, hint, row, _bpErr]);
    fill(v, header);
    fill(v, bpScroll);
    fill(v, row);
    [bpScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    return v;
}

- (NSView *)buildIo
{
    NSScrollView *scroll;
    _io = monoView(&scroll, NO);
    return scroll;
}

/* The packets left (a note and Clear over them), the link right. */
- (NSView *)buildFujinet
{
    _fujiNote = [self label:@""];
    _fujiNote.textColor = NSColor.secondaryLabelColor;
    _fujiNote.lineBreakMode = NSLineBreakByTruncatingTail;
    [_fujiNote setContentHuggingPriority:NSLayoutPriorityDefaultLow
                          forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_fujiNote setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *row = hstack(@[_fujiNote, [self button:@"Clear" action:@selector(fujiClear:)]]);

    NSScrollView *fujiScroll, *stateScroll;
    _fujiText = monoView(&fujiScroll, NO);
    _fujiState = monoView(&stateScroll, NO);
    NSTextField *title = [self label:@"AdamNet packets over BoIP"];
    title.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    NSStackView *left = vstack(@[title, row, fujiScroll]);
    left.distribution = NSStackViewDistributionFill;   /* the log takes the column's height */
    fill(left, row);
    fill(left, fujiScroll);
    NSTextField *stateTitle = [self label:@"BoIP link"];
    stateTitle.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    NSStackView *right = vstack(@[stateTitle, stateScroll]);
    right.distribution = NSStackViewDistributionFill;
    fill(right, stateScroll);

    NSStackView *h = hstack(@[left, right]);
    h.alignment = NSLayoutAttributeTop;
    h.spacing = 12;
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:520].active = YES;
    [right.widthAnchor constraintEqualToConstant:320].active = YES;
    [left.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    [right.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    return h;
}

- (void)buildWindow
{
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1180, 820)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"ADAM Debugger";
    _window.releasedWhenClosed = NO;
    _window.delegate = self;

    /* Toolbar row. The keys are handled by the key monitor, not as the
     * buttons' key equivalents, so they work wherever the focus is. */
    _runBtn = [self button:@"Stop (F5)" action:@selector(toggleRun:)];
    NSButton *step = [self button:@"Step (F7)" action:@selector(step:)];
    NSButton *over = [self button:@"Step Over (F8)" action:@selector(stepOver:)];
    NSButton *outBtn = [self button:@"Step Out (⇧F8)" action:@selector(stepOut:)];
    NSButton *toCursor = [self button:@"Run to Cursor" action:@selector(runToCursor:)];
    NSButton *frame = [self button:@"Frame (⇧F7)" action:@selector(frame:)];
    NSButton *symbols = [self button:@"Load Symbols…" action:@selector(loadSymbols:)];
    NSMutableArray *saveTitles = [NSMutableArray array];
    for (int i = 0; i < NUM_FILE_KINDS; i++) [saveTitles addObject:kFileTitles[i]];
    NSPopUpButton *save = [self pullDown:@"Save…" items:saveTitles action:@selector(saveFile:)];
    _status = [self label:@"Running"];
    _status.alignment = NSTextAlignmentRight;
    _status.textColor = NSColor.secondaryLabelColor;
    _status.lineBreakMode = NSLineBreakByTruncatingTail;
    NSStackView *toolbar = hstack(@[_runBtn, step, over, outBtn, toCursor, frame, symbols, save, _status]);
    [_status setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_status setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                      forOrientation:NSLayoutConstraintOrientationHorizontal];

    _tabs = [[NSTabView alloc] init];
    NSArray *pages = @[ @[@"Console", [self buildConsole]], @[@"CPU & Memory", [self buildCpu]],
                        @[@"Disassembly", [self buildDisasm]], @[@"VDP", [self buildVdp]],
                        @[@"Breakpoints", [self buildBreaks]], @[@"Memory Map & I/O", [self buildIo]],
                        @[@"FujiNet", [self buildFujinet]] ];
    for (NSArray *page in pages) {
        NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:page[0]];
        item.label = page[0];
        NSView *content = page[1];
        NSView *holder = [[NSView alloc] init];
        [holder addSubview:content];
        content.translatesAutoresizingMaskIntoConstraints = NO;
        [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
        [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
        [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
        item.view = holder;
        [_tabs addTabViewItem:item];
    }
    const char *tab = getenv("ADAM_DEBUGGER_TAB");
    if (tab && *tab) {
        int t = -1;
        if (tab[0] >= '0' && tab[0] <= '9') {
            t = atoi(tab);
        } else if (!strncasecmp(tab, "vram", 4)) {
            t = TAB_VDP;
        } else {
            for (int i = 0; i < NUM_TABS && t < 0; i++)
                if (!strncasecmp(tab, kTabNames[i], strlen(tab))) t = i;
        }
        if (t >= 0 && t < (int)_tabs.numberOfTabViewItems) [_tabs selectTabViewItemAtIndex:t];
    }
    _tabs.delegate = self;

    NSStackView *root = vstack(@[toolbar, _tabs]);
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [toolbar.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [_tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    _window.contentView = root;
    [_window center];
}

/* ---- actions ------------------------------------------------------------------ */

/* A line for the status label (and the console's log, when asked). */
- (void)message:(NSString *)text console:(BOOL)console
{
    _status.stringValue = text;
    if (console) [self appendConsole:[text stringByAppendingString:@"\n"]];
}

- (void)toggleRun:(id)sender
{
    (void)sender;
    const unsigned gen = adamdebug_generation(_dbg);
    if (adamdebug_is_stopped(_dbg)) adamdebug_resume(_dbg); else adamdebug_stop(_dbg);
    [self settleFrom:gen];
    [self refreshNow];
}

/* An action that moves the machine: the stop settles (or the poll catches
 * it, for a long run), then everything shown is read back. */
- (void)moved:(unsigned)gen
{
    [self settleFrom:gen];
    [self refreshNow];
}

- (void)step:(id)sender { (void)sender; const unsigned g = adamdebug_generation(_dbg); adamdebug_step(_dbg); [self moved:g]; }
- (void)stepOver:(id)sender { (void)sender; const unsigned g = adamdebug_generation(_dbg); adamdebug_step_over(_dbg); [self moved:g]; }
- (void)stepOut:(id)sender { (void)sender; const unsigned g = adamdebug_generation(_dbg); adamdebug_step_out(_dbg); [self moved:g]; }
- (void)frame:(id)sender { (void)sender; const unsigned g = adamdebug_generation(_dbg); adamdebug_frame(_dbg, 1); [self moved:g]; }

- (void)runToCursor:(id)sender
{
    (void)sender;
    if (_selected < 0) {
        [self message:@"Select a line in the Disassembly to run to" console:NO];
        return;
    }
    const unsigned g = adamdebug_generation(_dbg);
    adamdebug_run_to(_dbg, (uint16_t)_selected);
    [self moved:g];
}

- (void)toggleBreakpointAt:(uint16_t)addr
{
    adamdebug_breakpoint_toggle(_dbg, addr);
    [self refreshDisasm];
}

/* F9 */
- (void)toggleSelected:(id)sender
{
    (void)sender;
    if (_selected < 0) {
        [self message:@"Select a line in the Disassembly to toggle its breakpoint" console:NO];
        return;
    }
    [self toggleBreakpointAt:(uint16_t)_selected];
    if ([self currentTab] == TAB_BREAKS) [self refreshBps];
}

- (void)fujiClear:(id)sender
{
    (void)sender;
    adamdebug_fujinet_trace_clear(_dbg);
    [self refreshFujinet:YES];
}

- (void)appendConsole:(NSString *)text
{
    const BOOL follow = atEnd(_conOut);     /* a reader scrolled back keeps their place */
    [_conOut.textStorage appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:monoAttrs()]];
    if (follow) [_conOut scrollRangeToVisible:NSMakeRange(_conOut.string.length, 0)];
}

- (void)runCommand:(id)sender
{
    (void)sender;
    static char out[65536];
    NSString *cmd = [_conIn.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (cmd.length == 0) return;
    if (![_history.lastObject isEqualToString:cmd]) [_history addObject:cmd];
    _historyPos = (NSInteger)_history.count;
    [self appendConsole:[NSString stringWithFormat:@"> %@\n", cmd]];
    out[0] = '\0';
    const unsigned gen = adamdebug_generation(_dbg);
    adamdebug_command(_dbg, cmd.UTF8String, out, (int)sizeof out);
    NSString *text = cstr(out);
    if (text.length) [self appendConsole:[text hasSuffix:@"\n"] ? text : [text stringByAppendingString:@"\n"]];
    _conIn.stringValue = @"";
    [self settleFrom:gen];
    [self refreshNow];
}

/* In the console, Tab completes instead of moving the focus, and Up/Down
 * walk the history. */
- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)sel
{
    (void)textView;
    if (control != _conIn) return NO;
    if (sel == @selector(moveUp:) || sel == @selector(moveDown:)) {
        const NSInteger n = (NSInteger)_history.count;
        if (n == 0) return YES;
        _historyPos += sel == @selector(moveUp:) ? -1 : 1;
        if (_historyPos < 0) _historyPos = 0;
        if (_historyPos > n) _historyPos = n;
        _conIn.stringValue = _historyPos < n ? _history[(NSUInteger)_historyPos] : @"";
        [_conIn.currentEditor setSelectedRange:NSMakeRange(_conIn.stringValue.length, 0)];
        return YES;
    }
    if (sel != @selector(insertTab:)) return NO;
    /* the engine takes the whole line and completes its last word */
    NSString *text = _conIn.stringValue;
    const NSRange sp = [text rangeOfString:@" " options:NSBackwardsSearch];
    char comps[4096];
    comps[0] = '\0';
    const int n = adamdebug_completions(_dbg, text.UTF8String, comps, (int)sizeof comps);
    NSString *list = cstr(comps);
    if (n == 1) {
        NSString *c = [list componentsSeparatedByString:@"\n"][0];
        NSString *head = sp.location == NSNotFound ? @"" : [text substringToIndex:sp.location + 1];
        _conIn.stringValue = [NSString stringWithFormat:@"%@%@ ", head, c];
        [_conIn.currentEditor setSelectedRange:NSMakeRange(_conIn.stringValue.length, 0)];
    } else if (n > 1) {
        if (![list hasSuffix:@"\n"]) list = [list stringByAppendingString:@"\n"];
        [self appendConsole:list];
    }
    return YES;
}

- (void)loadSymbols:(id)sender
{
    (void)sender;
    char msg[512];
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.title = @"Load Symbols (\"HHHH NAME\" per line, or a WLA, z88dk or SDCC map)";
    if ([p runModal] != NSModalResponseOK) return;
    msg[0] = '\0';
    adamdebug_load_symbols(_dbg, [[p URL] fileSystemRepresentation], msg, (int)sizeof msg);
    [self message:cstr(msg) console:YES];
    [self refreshNow];
}

- (void)saveFile:(NSPopUpButton *)sender
{
    const NSInteger kind = sender.selectedItem.tag;
    if (kind < 0 || kind >= NUM_FILE_KINDS) return;
    NSSavePanel *p = [NSSavePanel savePanel];
    p.title = [NSString stringWithFormat:@"Save %@", kFileTitles[kind]];
    [p setNameFieldStringValue:kFileNames[kind]];
    if ([p runModal] != NSModalResponseOK) return;
    char msg[512];
    msg[0] = '\0';
    adamdebug_save(_dbg, kFileKinds[kind], [[p URL] fileSystemRepresentation], msg, (int)sizeof msg);
    [self message:cstr(msg) console:YES];
}

- (void)applyRegister:(NSTextField *)sender
{
    static const int regIds[NUM_REGS] = {
        ADAM_REG_PC, ADAM_REG_SP, ADAM_REG_AF, ADAM_REG_BC, ADAM_REG_DE, ADAM_REG_HL, ADAM_REG_IX, ADAM_REG_IY,
        ADAM_REG_AF2, ADAM_REG_BC2, ADAM_REG_DE2, ADAM_REG_HL2, ADAM_REG_I, ADAM_REG_R, ADAM_REG_IM,
        ADAM_REG_IFF1, ADAM_REG_IFF2
    };
    long v;
    NSString *why = nil;
    [_window makeFirstResponder:nil];       /* leave the field, so the refresh may rewrite it */
    if (!adamdebug_is_stopped(_dbg)) {
        [self message:@"Stop the machine (F5) to edit registers" console:NO];
    } else if (sender.tag < 0 || sender.tag >= NUM_REGS) {
        /* not one of ours */
    } else if ([self evaluate:sender.stringValue into:&v why:&why]) {
        adamdebug_cpu_set(_dbg, regIds[sender.tag], (int)v);
    } else {
        [self message:[NSString stringWithFormat:@"Register: %@", why] console:NO];
    }
    [self refreshCpu];
    if ([self currentTab] == TAB_DISASM) [self refreshDisasm];
}

- (void)flagClicked:(NSButton *)sender
{
    static const int flagIds[NUM_FLAGS] = { ADAM_FLAG_S, ADAM_FLAG_Z, ADAM_FLAG_H, ADAM_FLAG_PV, ADAM_FLAG_N, ADAM_FLAG_C };
    if (adamdebug_is_stopped(_dbg))
        adamdebug_cpu_set(_dbg, flagIds[sender.tag], sender.state == NSControlStateValueOn);
    else
        [self message:@"Stop the machine (F5) to edit flags" console:NO];
    [self refreshCpu];
}

/* Brings an address's row into the dump (a couple of rows down from the
 * top) and marks it. */
- (void)showMemAddr:(int)addr
{
    _memBase = (addr & ~0xF) - 2 * 16;
    _memMark = addr;
    [self refreshMem];
}

- (void)gotoMem:(id)sender
{
    (void)sender;
    NSString *why = nil;
    const int a = [self resolveAddr:_memGoto.stringValue why:&why];
    if (a < 0) {
        [self message:[NSString stringWithFormat:@"Go to %@: %@", _memGoto.stringValue, why] console:NO];
        return;
    }
    [self showMemAddr:a];
}

- (void)memPrev:(id)sender { (void)sender; _memBase -= MEM_BYTES; [self refreshMem]; }
- (void)memNext:(id)sender { (void)sender; _memBase += MEM_BYTES; [self refreshMem]; }

/* "address value [value...]": the values land at consecutive addresses. */
- (void)writeMem:(id)sender
{
    (void)sender;
    NSArray<NSString *> *words = [_memWrite.stringValue componentsSeparatedByCharactersInSet:
        [NSCharacterSet whitespaceCharacterSet]];
    NSMutableArray<NSString *> *tok = [NSMutableArray array];
    for (NSString *w in words) if (w.length) [tok addObject:w];
    NSString *why = nil;
    const int a = tok.count > 0 ? [self resolveAddr:tok[0] why:&why] : -1;
    if (tok.count < 2 || a < 0) {
        [self message:@"Poke: address value [value...]" console:NO];
        return;
    }
    for (NSUInteger i = 1; i < tok.count; i++) {
        long v;
        if (![self evaluate:tok[i] into:&v why:&why] || v < 0 || v > 0xFF || a + (int)i - 1 > 0xFFFF) {
            [self message:[NSString stringWithFormat:@"Not a byte: %@", tok[i]] console:NO];
            return;
        }
        adamdebug_write(_dbg, (uint16_t)(a + (int)i - 1), (uint8_t)v);
    }
    _memWrite.stringValue = @"";
    if (a < _memBase || a >= _memBase + MEM_BYTES) [self showMemAddr:a];
    else [self refreshMem];
}

- (void)followToggled:(id)sender
{
    (void)sender;
    [self refreshDisasm];
}

- (void)jumpTo:(id)sender
{
    (void)sender;
    NSString *why = nil;
    const int addr = [self resolveAddr:_jump.stringValue why:&why];
    if (addr < 0) {
        [self message:[NSString stringWithFormat:@"Jump to %@: %@", _jump.stringValue, why] console:NO];
        return;
    }
    _followPc.state = NSControlStateValueOff;
    _disasmAddr = (uint16_t)addr;
    _selected = addr;
    [self refreshDisasm];
}

- (void)addBreakpoint:(id)sender
{
    (void)sender;
    static const int types[5] = { ADAMDEBUG_BP_EXEC, ADAMDEBUG_BP_READ, ADAMDEBUG_BP_WRITE, ADAMDEBUG_BP_IN,
                                  ADAMDEBUG_BP_OUT };
    const NSInteger sel = _bpType.indexOfSelectedItem;
    const int type = types[sel >= 0 && sel < 5 ? sel : 0];
    const BOOL port = (type & (ADAMDEBUG_BP_IN | ADAMDEBUG_BP_OUT)) != 0;
    NSString *what = port ? @"port" : @"address";
    NSString *startText = [_bpStart.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    NSString *endText = [_bpEnd.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (startText.length == 0) {
        _bpErr.stringValue = [NSString stringWithFormat:@"Give a %@", what];
        return;
    }
    NSString *why = nil;
    const int start = [self resolveAddr:startText why:&why];
    const int end = endText.length ? [self resolveAddr:endText why:&why] : start;
    if (start < 0 || end < 0 || end < start || (port && end > 0xFF)) {
        _bpErr.stringValue = [NSString stringWithFormat:@"Bad %@ range: %@%@%@%@%@", what, startText,
                              endText.length ? @" - " : @"", endText, why.length ? @" (" : @"",
                              why.length ? [why stringByAppendingString:@")"] : @""];
        return;
    }
    char err[256];
    err[0] = '\0';
    const int bpId = adamdebug_breakpoint_add(_dbg, type, (uint16_t)start, (uint16_t)end,
                                              _bpCond.stringValue.UTF8String, err, (int)sizeof err);
    if (bpId < 0) {
        NSString *e = cstr(err);
        _bpErr.stringValue = e.length ? e : @"The breakpoint was refused";
        return;
    }
    _bpErr.stringValue = @"";
    _bpStart.stringValue = @"";
    _bpEnd.stringValue = @"";
    _bpCond.stringValue = @"";
    [self refreshNow];
}

- (void)bpEnabled:(NSButton *)sender
{
    adamdebug_breakpoint_enable(_dbg, (int)sender.tag, sender.state == NSControlStateValueOn);
    [self refreshNow];
}

- (void)bpRemove:(NSButton *)sender
{
    adamdebug_breakpoint_remove(_dbg, (int)sender.tag);
    [self refreshNow];
}

- (void)clearBreaks:(id)sender
{
    (void)sender;
    adamdebug_breakpoint_clear(_dbg);
    [self refreshNow];
}

- (void)vdpViewChanged:(id)sender
{
    (void)sender;
    [self refreshVdp];
}

/* One field does both jobs: an address alone scrolls the dump there, an
 * address followed by hex bytes writes them first ("1800: 41 42 43"). */
- (void)vramEntryActivated:(id)sender
{
    (void)sender;
    uint8_t bytes[POKE_MAX];
    uint16_t addr = 0;
    int n = adamvdp_parse_poke(_vramEntry.stringValue.UTF8String, &addr, bytes, POKE_MAX);
    if (n < 0) {
        _vramStatus.stringValue = @"expected: addr [bytes...], e.g. 1800: 41 42";
        return;
    }
    if (n > 0) {
        adamdebug_vdp_write(_dbg, addr, bytes, n);
        _vramStatus.stringValue = [NSString stringWithFormat:@"wrote %d byte%s at $%04X", n, n == 1 ? "" : "s", addr];
        _vramEntry.stringValue = @"";
    } else {
        _vramStatus.stringValue = [NSString stringWithFormat:@"$%04X", addr];
    }
    _vramBase = (uint16_t)(addr & 0x3FF0);
    [self refreshVdp];
}

- (void)vramPagePrev:(id)sender
{
    (void)sender;
    _vramBase = (uint16_t)((_vramBase - VRAM_PAGE) & 0x3FFF);
    [self refreshVdp];
}

- (void)vramPageNext:(id)sender
{
    (void)sender;
    _vramBase = (uint16_t)((_vramBase + VRAM_PAGE) & 0x3FFF);
    [self refreshVdp];
}

/* ---- refreshers ------------------------------------------------------------------ */

/* Everything the window shows, read back now: the status and the visible
 * tab. The poll compares against what was seen here. */
- (void)refreshNow
{
    _seenGen = adamdebug_generation(_dbg);
    _wasStopped = adamdebug_is_stopped(_dbg) != 0;
    _liveTicks = 0;
    [self refreshStatus];
    [self refreshTab];
}

/* Only the tab on show: the others refresh when they are selected. */
- (void)refreshTab
{
    switch ([self currentTab]) {
    case TAB_CPU: [self refreshCpu]; [self refreshMem]; break;
    case TAB_DISASM: [self refreshDisasm]; break;
    case TAB_VDP: [self refreshVdp]; break;
    case TAB_BREAKS: [self refreshBps]; break;
    case TAB_IO: [self refreshIo]; break;
    case TAB_FUJINET: [self refreshFujinet:NO]; break;
    default: break;
    }
}

- (void)refreshStatus
{
    char reason[160];
    int addr = -1;
    const BOOL stopped = adamdebug_is_stopped(_dbg) != 0;
    reason[0] = '\0';
    adamdebug_stop_reason(_dbg, reason, (int)sizeof reason, &addr);
    NSString *why = cstr(reason);
    NSMutableString *text = [NSMutableString stringWithString:stopped ? @"Stopped" : @"Running"];
    if (stopped) {
        /* "stopped" says nothing the word does not; a reason does */
        if (why.length && ![why isEqualToString:@"stopped"]) [text appendFormat:@": %@", why];
        if (addr >= 0) {
            uint16_t off = 0;
            const char *sym = adamdebug_symbol_at(_dbg, (uint16_t)addr, &off);
            [text appendFormat:@"   PC $%04X", addr & 0xFFFF];
            if (sym && off < 0x100) [text appendFormat:@"  %@%@", cstr(sym), off ? [NSString stringWithFormat:@"+%X", off] : @""];
        }
    }
    _status.stringValue = text;
    _runBtn.title = stopped ? @"Run (F5)" : @"Stop (F5)";
    ADAMSetButtonAccent(_runBtn, stopped);    /* the accent while stopped */
}

- (void)refreshCpu
{
    adamdebug_cpu c;
    adamdebug_cpu_get(_dbg, &c);
    const int vals[NUM_REGS] = {
        c.pc, c.sp, c.af, c.bc, c.de, c.hl, c.ix, c.iy, c.af2, c.bc2, c.de2, c.hl2, c.i, c.r, c.im, c.iff1, c.iff2
    };
    for (int i = 0; i < NUM_REGS; i++)
        if (!_reg[i].currentEditor)         /* never over a field being edited */
            _reg[i].stringValue = [NSString stringWithFormat:(i < 12 ? @"%04X" : (i < 14 ? @"%02X" : @"%d")), vals[i]];
    const int flags[NUM_FLAGS] = { c.sf, c.zf, c.hf, c.pf, c.nf, c.cf };
    for (int i = 0; i < NUM_FLAGS; i++) _flag[i].state = flags[i] ? NSControlStateValueOn : NSControlStateValueOff;
    _beam.stringValue = [NSString stringWithFormat:@"line %d   frame %u   cycles %llu   WZ $%04X%@", c.line, c.frame,
                         (unsigned long long)c.cycles, c.wz & 0xFFFF, c.halted ? @"   HALT" : @""];
}

- (void)refreshMem
{
    uint8_t data[MEM_BYTES];
    if (_memBase > 0x10000 - MEM_BYTES) _memBase = 0x10000 - MEM_BYTES;
    if (_memBase < 0) _memBase = 0;
    memset(data, 0, sizeof data);
    adamdebug_read(_dbg, (uint16_t)_memBase, data, MEM_BYTES);
    NSMutableString *text = [NSMutableString stringWithString:@"      0  1  2  3  4  5  6  7   8  9  A  B  C  D  E  F\n"];
    NSRange mark = NSMakeRange(NSNotFound, 0);
    for (int row = 0; row < MEM_ROWS; row++) {
        const int base = _memBase + row * 16;
        const NSUInteger start = text.length;
        [text appendFormat:@"%04X  ", base];
        for (int col = 0; col < 16; col++)
            [text appendFormat:(col == 8 ? @" %02X " : @"%02X "), data[row * 16 + col]];
        [text appendString:@" "];
        for (int col = 0; col < 16; col++) {
            const uint8_t c = data[row * 16 + col];
            [text appendFormat:@"%c", (c >= 0x20 && c < 0x7F) ? (char)c : '.'];
        }
        if (_memMark >= base && _memMark < base + 16) mark = NSMakeRange(start, text.length - start);
        [text appendString:@"\n"];
    }
    NSMutableAttributedString *as = [[NSMutableAttributedString alloc] initWithString:text attributes:monoAttrs()];
    if (mark.location != NSNotFound) [as addAttributes:accentAttrs() range:mark];
    setStyledText(_mem, as);
}

- (void)refreshDisasm
{
    static adamdebug_line lines[DISASM_WINDOW];
    int pcLine = -1;
    if (_followPc.state == NSControlStateValueOn) {
        adamdebug_cpu c;
        adamdebug_cpu_get(_dbg, &c);
        /* the PC a third of the way down */
        _disasmAddr = (uint16_t)adamdebug_row_address(_dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 3));
    }
    int n = adamdebug_disassemble(_dbg, _disasmAddr, lines, DISASM_WINDOW, &pcLine);
    if (n < 0) n = 0;
    if (n > DISASM_WINDOW) n = DISASM_WINDOW;
    _lineCount = n;

    /* "*> 8033  CD 50 80     LABEL            CALL $8050   ; note": the
     * ranges are the line, its label and its comment, for the colours */
    NSMutableString *text = [NSMutableString string];
    NSRange ranges[DISASM_WINDOW], labels[DISASM_WINDOW], notes[DISASM_WINDOW];
    for (int i = 0; i < n; i++) {
        _lineAddr[i] = lines[i].address;
        const char gutter = !lines[i].has_breakpoint ? ' ' : (lines[i].breakpoint_enabled ? '*' : 'o');
        NSString *label = cstr(lines[i].label), *comment = cstr(lines[i].comment);
        NSMutableString *line = [NSMutableString stringWithFormat:@"%c%c %04X  ", gutter,
                                 lines[i].is_pc ? '>' : ' ', lines[i].address];
        [line appendString:padTo(cstr(lines[i].bytes), 11)];
        [line appendString:@"  "];
        labels[i] = NSMakeRange(text.length + line.length, label.length);
        [line appendString:padTo(label, 16)];
        [line appendString:@" "];
        [line appendString:padTo(cstr(lines[i].disasm), 24)];
        notes[i] = NSMakeRange(NSNotFound, 0);
        if (comment.length) {
            notes[i] = NSMakeRange(text.length + line.length, comment.length + 3);
            [line appendString:@" ; "];
            [line appendString:comment];
        }
        ranges[i] = NSMakeRange(text.length, line.length);
        [text appendString:line];
        [text appendString:@"\n"];
    }
    if (n == 0) [text appendString:@"(no disassembly: the debugger is not attached)\n"];

    NSMutableAttributedString *as = [[NSMutableAttributedString alloc] initWithString:text attributes:monoAttrs()];
    for (int i = 0; i < n; i++) {
        /* labels and comments grey */
        if (labels[i].length)
            [as addAttribute:NSForegroundColorAttributeName value:NSColor.systemGrayColor range:labels[i]];
        if (notes[i].location != NSNotFound)
            [as addAttribute:NSForegroundColorAttributeName value:NSColor.systemGrayColor range:notes[i]];
        /* the gutter: a breakpoint red, a disabled one grey */
        if (lines[i].has_breakpoint)
            [as addAttribute:NSForegroundColorAttributeName
                       value:lines[i].breakpoint_enabled ? breakpointRed() : NSColor.systemGrayColor
                       range:NSMakeRange(ranges[i].location, 1)];
        /* the selected line underlined on a selection fill */
        if ((int)lines[i].address == _selected)
            [as addAttributes:@{ NSUnderlineStyleAttributeName: @(NSUnderlineStyleSingle),
                                 NSBackgroundColorAttributeName: NSColor.selectedTextBackgroundColor }
                        range:ranges[i]];
    }
    /* the PC's line in the accent colour with black text, last so its
     * colours win */
    for (int i = 0; i < n; i++) {
        if (!lines[i].is_pc) continue;
        [as addAttributes:accentAttrs() range:ranges[i]];
        if ((int)lines[i].address == _selected)
            [as addAttribute:NSUnderlineStyleAttributeName value:@(NSUnderlineStyleSingle) range:ranges[i]];
    }
    [_disasm.textStorage setAttributedString:as];
    NSScrollView *scroll = _disasm.enclosingScrollView;
    [scroll.contentView scrollToPoint:NSMakePoint(scroll.contentView.bounds.origin.x, 0)];
    [scroll reflectScrolledClipView:scroll.contentView];
}

- (void)refreshVdp
{
    static adamvdp_snapshot snap;
    static uint8_t nt[256 * 192 * 4], pat[256 * 64 * 4], spr[128 * 64 * 4], pal[ADAMVDP_PAL_W * ADAMVDP_PAL_H * 4];
    static char text[4096];
    adamvdp_sprite info[32];

    adamdebug_vdp_snapshot(_dbg, &snap);

    adamvdp_render_nametable(&snap, nt);
    _ntView.image = imageFromRGBA(nt, 256, 192);

    adamvdp_render_patterns(&snap, (int)_patBank.indexOfSelectedItem, pat);
    _patView.image = imageFromRGBA(pat, 256, 64);

    adamvdp_render_sprites(&snap, spr, info);
    _sprView.image = imageFromRGBA(spr, 128, 64);

    adamvdp_render_palette(&snap, pal);
    _palView.image = imageFromRGBA(pal, ADAMVDP_PAL_W, ADAMVDP_PAL_H);

    NSMutableString *sprText = [NSMutableString stringWithString:@"##  Y    X  PAT CLR EC\n"];
    for (int i = 0; i < 32; i++)
        [sprText appendFormat:@"%02d %3d  %3d  %02X  %2d  %d\n", i, info[i].y, info[i].x, info[i].pattern,
                              info[i].color, info[i].early_clock];
    setText(_sprInfo, sprText, NO);

    text[0] = '\0';
    adamvdp_format_state(&snap, text, (int)sizeof(text));
    setText(_vdpState, cstr(text), NO);

    text[0] = '\0';
    adamvdp_format_hex(&snap, _vramBase, VRAM_ROWS, text, (int)sizeof(text));
    setText(_vramView, cstr(text), NO);
}

- (void)refreshBps
{
    static adamdebug_breakpoint bps[MAX_BPS];
    int n = adamdebug_breakpoint_list(_dbg, bps, MAX_BPS);
    if (n < 0) n = 0;
    if (n > MAX_BPS) n = MAX_BPS;

    /* the rows are rebuilt only when something in them changed (a refresh
     * must not eat a click) */
    NSMutableString *sig = [NSMutableString string];
    for (int i = 0; i < n; i++)
        [sig appendFormat:@"%d %d %d %d %d %u %@\n", bps[i].id, bps[i].type, bps[i].start, bps[i].end,
                          bps[i].enabled, bps[i].hits, cstr(bps[i].condition)];
    if ([sig isEqualToString:_bpSig]) return;
    _bpSig = sig;

    for (NSView *v in [_bpList.arrangedSubviews copy]) [_bpList removeView:v];
    if (n == 0) [_bpList addArrangedSubview:[self label:@"No breakpoints"]];
    for (int i = 0; i < n; i++) {
        static const struct { int bit; const char *name; } kinds[5] = {
            { ADAMDEBUG_BP_EXEC, "exec" }, { ADAMDEBUG_BP_READ, "read" }, { ADAMDEBUG_BP_WRITE, "write" },
            { ADAMDEBUG_BP_IN, "in" }, { ADAMDEBUG_BP_OUT, "out" },
        };
        NSMutableString *type = [NSMutableString string];
        for (int k = 0; k < 5; k++)
            if (bps[i].type & kinds[k].bit) {
                if (type.length) [type appendString:@"/"];
                [type appendFormat:@"%s", kinds[k].name];
            }
        const BOOL port = (bps[i].type & (ADAMDEBUG_BP_IN | ADAMDEBUG_BP_OUT)) != 0;
        NSMutableString *range = [NSMutableString stringWithFormat:(port ? @"port $%02X" : @"$%04X"), bps[i].start];
        if (bps[i].end != bps[i].start) [range appendFormat:(port ? @"-$%02X" : @"-$%04X"), bps[i].end];
        char label[64];
        label[0] = '\0';
        if (!port && adamdebug_address_label(_dbg, bps[i].start, label, (int)sizeof label) > 0 && label[0])
            [range appendFormat:@" %@", cstr(label)];

        NSButton *box = [NSButton checkboxWithTitle:[NSString stringWithFormat:@"#%d", bps[i].id]
                                             target:self action:@selector(bpEnabled:)];
        box.state = bps[i].enabled ? NSControlStateValueOn : NSControlStateValueOff;
        box.tag = bps[i].id;
        box.font = monoFont();
        [box.widthAnchor constraintEqualToConstant:70].active = YES;
        NSButton *remove = [self button:@"Remove" action:@selector(bpRemove:)];
        remove.tag = bps[i].id;
        [remove.widthAnchor constraintEqualToConstant:70].active = YES;
        NSStackView *row = hstack(@[box, bpCell(type, 60, NO), bpCell(range, 190, NO),
                                    bpCell(cstr(bps[i].condition), 0, NO),
                                    bpCell([NSString stringWithFormat:@"%u", bps[i].hits], 60, NO), remove]);
        row.distribution = NSStackViewDistributionFill;
        [_bpList addArrangedSubview:row];
        [row.widthAnchor constraintEqualToAnchor:_bpList.widthAnchor].active = YES;
    }
}

- (void)refreshIo
{
    static char text[8192];
    text[0] = '\0';
    adamdebug_io_state(_dbg, text, (int)sizeof(text));
    setText(_io, cstr(text), NO);
}

/* The log is rebuilt only when it changed: a new packet, or a repeat folded
 * into the newest. */
- (void)refreshFujinet:(BOOL)force
{
    static adam_fuji_packet p[TRACE_MAX];
    static char state[4096];
    int n = adamdebug_fujinet_trace(_dbg, p, TRACE_MAX);
    if (n < 0) n = 0;

    if (!adamsession_fujinet_running(_session) && !adamsession_fujinet_external(_session))
        _fujiNote.stringValue = @"FujiNet is not running (FujiNet ▸ Console Log).";
    else
        _fujiNote.stringValue = [NSString stringWithFormat:@"%@ FujiNet on BoIP %s, %@.",
                                 adamsession_fujinet_external(_session) ? @"External" : @"Built-in",
                                 adamsession_boip_endpoint(_session),
                                 adamsession_fujinet_link(_session) ? @"connected" : @"not connected"];
    state[0] = '\0';
    adamdebug_fujinet_state(_dbg, state, (int)sizeof(state));
    setText(_fujiState, cstr(state), NO);

    if (!force && n == _fujiCount && (n == 0 || p[n - 1].serial == _fujiLast))
        return;
    const BOOL follow = force || _fujiCount < 0 || atEnd(_fujiText);
    _fujiCount = n;
    _fujiLast = n > 0 ? p[n - 1].serial : 0;
    NSMutableString *text = [NSMutableString string];
    for (int i = 0; i < n; i++) {
        char hex[sizeof(p[i].data) * 3 + 8];
        int len = 0;
        hex[0] = '\0';
        for (int k = 0; k < p[i].nbytes && k < (int)sizeof(p[i].data); k++)
            len += snprintf(hex + len, sizeof(hex) - (size_t)len, "%02X ", p[i].data[k]);
        if (p[i].nbytes > (int)sizeof(p[i].data))
            snprintf(hex + len, sizeof(hex) - (size_t)len, "\xe2\x80\xa6");
        if (i) [text appendString:@"\n"];
        [text appendString:cstr(p[i].text)];
        [text appendString:@"\n     "];
        [text appendString:cstr(hex)];
    }
    if (n == 0) [text appendString:@"(no AdamNet packets since the trace was cleared)"];
    setText(_fujiText, text, follow);
}

@end

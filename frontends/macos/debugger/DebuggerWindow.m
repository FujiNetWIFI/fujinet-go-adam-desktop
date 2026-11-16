/*
 * Debugger window (AppKit) over this app's Z80 debugger engine, via
 * core/include/adamdebug.h. Twelve tabs, as on the GNOME, KDE and Windows
 * debuggers: Console, Disassembly, Memory, Breakpoints, Watches, Cart & I/O,
 * Trace, History, Video, Sound, Symbols, FujiNet, over an editable register
 * strip. The Video tab is VideoTab.m, a view over core/include/adamvidtab.h;
 * the old VDP panes are its Decoded sources and its VRAM section.
 *
 * Opening the window attaches the engine, which stops the machine at its next
 * instruction (the engine only engages while a window holds it); closing it
 * (F12, the close button, the menu) turns the Trace and History records off
 * and detaches, and the machine runs on. F12 in the machine's window toggles
 * it.
 *
 * The engine's stop callback fires on the emulator thread, so the window does
 * not use it: a 100 ms NSTimer polls adamdebug_stop_snapshot() (the stop's
 * serial) and adamdebug_generation() and refreshes the tabs on show, and
 * only when something changed. While the machine runs, the visible tabs' live values
 * refresh twice a second (every fifth tick), the Video tab every 0.3 s and
 * the Trace tab every 0.4 s. After a step the window waits a moment (~40 ms)
 * for the stop to settle before it reads anything back; a longer run (Frame,
 * Step Out, Run to Cursor) is picked up by the poll when it stops.
 *
 * Keys work anywhere in the window (a local key monitor, so the console's
 * field takes them too): F5 stop/run, F7 step, Shift+F7 frame (to the end of the
 * frame in progress), F8 step over, Shift+F8 step out, F9 toggle the breakpoint
 * on the selected disassembly line, F12 close.
 *
 * Environment hooks, for tests and captures: ADAM_DEBUGGER_TAB (an index, a
 * tab's name or id, or an older name: cpu, vdp, vram, io), ADAM_DEBUGGER_WATCH,
 * ADAM_DEBUGGER_WATCH_BREAK, ADAM_DEBUGGER_MEM_GOTO, ADAM_DEBUGGER_HISTORY,
 * ADAM_DEBUGGER_POPOUT (a tab), and ADAM_DEBUGGER_VIDEO_* (read by the
 * Video tab's controller). ADAM_OPEN_DEBUGGER opens the window at launch.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"
#import "VideoTab.h"
#import "../help/HelpWindow.h"

#include <float.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "adamdebug.h"
#include "adammemtools.h"
#include "adamsession.h"

#define DISASM_WINDOW 48
#define MAX_BPS 64
#define NUM_REGS 17
#define NUM_FLAGS 6
#define MEM_ROWS 32            /* the hex dump: 16 bytes a row */
#define MEM_BYTES (MEM_ROWS * 16)
#define POKE_MAX 64
#define TRACE_MAX 512          /* FujiNet packets shown */
#define LIVE_TICKS 5           /* 5 x 100 ms: the live refresh while running */
#define VIDEO_TICKS 3          /* the Video tab follows a running machine, every 0.3 s */
#define TRACE_TICKS 4          /* the Trace tab, every 0.4 s */
#define HIST_BUF 4000          /* records read from the ring */
#define HIST_SHOWN 400         /* lines shown */
#define SEARCH_MAX 32

static DebuggerWindow *g_debugger;

/* The CPU as the strip shows it: the pairs, the flags split out of F. */
typedef struct {
    int programCounter, stackPointer, pairAF, pairBC, pairDE, pairHL, indexX, indexY;
    int pairAF2, pairBC2, pairDE2, pairHL2;
    int vectorBase, refresh, intMode, iff1, iff2;
    int sign, zero, half, parity, subtract, carry;
    int halted;
} CpuView;

static void cpuView(adamdebug *dbg, CpuView *out)
{
    adamdebug_cpu cpu;
    adamdebug_cpu_get(dbg, &cpu);
    out->programCounter = cpu.pc;
    out->stackPointer = cpu.sp;
    out->pairAF = cpu.af;
    out->pairBC = cpu.bc;
    out->pairDE = cpu.de;
    out->pairHL = cpu.hl;
    out->indexX = cpu.ix;
    out->indexY = cpu.iy;
    out->pairAF2 = cpu.af2;
    out->pairBC2 = cpu.bc2;
    out->pairDE2 = cpu.de2;
    out->pairHL2 = cpu.hl2;
    out->vectorBase = cpu.i;
    out->refresh = cpu.r;
    out->intMode = cpu.im;
    out->iff1 = cpu.iff1 != 0;
    out->iff2 = cpu.iff2 != 0;
    out->sign = cpu.sf != 0;
    out->zero = cpu.zf != 0;
    out->half = cpu.hf != 0;
    out->parity = cpu.pf != 0;
    out->subtract = cpu.nf != 0;
    out->carry = cpu.cf != 0;
    out->halted = cpu.halted != 0;
}

/* The tabs, in order. The identifiers are the help contexts' ends
 * (debugger/<id>, adamhelp.h). ADAM_DEBUGGER_TAB picks one by number, by the
 * start of its title or id (any case), or by an older name ("cpu", "vdp",
 * "vram", "io"), for tests and captures. */
static const char *const kTabNames[] = {"Console", "Disassembly", "Memory", "Breakpoints", "Watches", "Cart & I/O",
                                        "Trace", "History", "Video", "Sound", "Symbols", "FujiNet"};
static const char *const kTabIds[] = {"console", "disassembly", "memory", "breakpoints", "watches", "cart",
                                      "trace", "history", "video", "sound", "symbols", "fujinet"};
#define TAB_COUNT ((int)(sizeof(kTabNames) / sizeof(kTabNames[0])))
enum { MT_CONSOLE, MT_DISASM, MT_MEMORY, MT_BREAKS, MT_WATCHES, MT_CART, MT_TRACE, MT_HISTORY, MT_VIDEO, MT_SOUND,
       MT_SYMBOLS, MT_FUJINET };

static int tabFromText(const char *tab)
{
    if (!tab || !*tab) return -1;
    char *end;
    const long count = strtol(tab, &end, 10);
    if (end != tab && *end == '\0') return count >= 0 && count < TAB_COUNT ? (int)count : -1;
    if (strcasecmp(tab, "cpu") == 0) return MT_DISASM;
    if (strcasecmp(tab, "vdp") == 0 || strcasecmp(tab, "vram") == 0) return MT_VIDEO;
    if (strcasecmp(tab, "io") == 0) return MT_CART;
    for (int idx = 0; idx < TAB_COUNT; idx++)
        if (strncasecmp(tab, kTabNames[idx], strlen(tab)) == 0 || strncasecmp(tab, kTabIds[idx], strlen(tab)) == 0)
            return idx;
    return -1;
}

/* The stopped Run button, the PC's line and a marked row wear the accent (the
 * icon's colour) with its ink on top; changed values wear the light variant,
 * which reads on a dark theme. */
static NSColor *rgbColor(unsigned rgb)
{
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xff) / 255.0
                               green:((rgb >> 8) & 0xff) / 255.0
                                blue:(rgb & 0xff) / 255.0
                               alpha:1.0];
}

static NSColor *accentColor(void)
{
    return rgbColor(ADAMSESSION_ACCENT_RGB);
}

static NSColor *accentInk(void)
{
    return rgbColor(ADAMSESSION_ACCENT_INK_RGB);
}

static NSColor *accentLight(void)
{
    return rgbColor(ADAMSESSION_ACCENT_LIGHT_RGB);
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
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger idx = [self characterIndexForInsertionAtPoint:point];
    NSString *text = self.string;
    if (idx > text.length) idx = text.length;
    int line = 0;
    NSUInteger lineStart = 0;
    for (NSUInteger charPos = 0; charPos < idx; charPos++)
        if ([text characterAtIndex:charPos] == '\n') { line++; lineStart = charPos + 1; }
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

/* A list of lines the user picks one of by clicking (the Watches tab, and the
 * Symbols tab, where the pick is the address to go to). The picked line is
 * kept in `picked`, -1 for none, and drawn with a background by whoever owns
 * the text. */
@interface PickTextView : NSTextView
@property (nonatomic) int picked;
@property (nonatomic, copy) void (^onPick)(int line, NSInteger clicks);
@end

@implementation PickTextView
- (void)mouseDown:(NSEvent *)event
{
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger idx = [self characterIndexForInsertionAtPoint:point];
    NSString *text = self.string;
    if (idx > text.length) idx = text.length;
    int line = 0;
    for (NSUInteger charPos = 0; charPos < idx; charPos++)
        if ([text characterAtIndex:charPos] == '\n') line++;
    self.picked = line;
    if (self.onPick) self.onPick(line, event.clickCount);
}
@end

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
    uint64_t _seenSerial;
    unsigned _seenGen;
    BOOL _wasStopped;
    BOOL _envApplied;
    int _liveTicks, _videoTicks, _traceTicks;
    NSTabView *_tabs;

    /* Tabs peeled off into windows of their own */
    NSTabViewItem *_items[16];
    NSWindow *_popped[16];

    NSButton *_runBtn;
    NSTextField *_status;

    /* Console */
    NSTextView *_conOut;
    NSTextField *_conIn;
    NSMutableArray<NSString *> *_history;
    NSInteger _historyPos;

    /* The register strip under every tab */
    NSTextField *_reg[NUM_REGS];
    NSButton *_flag[NUM_FLAGS];
    NSTextField *_regInfo;

    /* Disassembly */
    NSButton *_followPc;
    NSTextField *_jump;
    DasmTextView *_disasm;
    uint16_t _disasmAddr;
    uint16_t _lineAddr[DISASM_WINDOW];
    int _lineCount;
    int _selected;              /* the selected line's address, or -1 */

    /* Memory: the dump, the highlighted range, the Poke bar and the search */
    NSTextView *_mem;
    NSTextField *_memGoto, *_memWrite, *_memNote;
    int _memBase;               /* the dump's first address */
    BOOL _memHl;
    uint16_t _memHlLo, _memHlHi;
    NSPopUpButton *_srchKind;
    NSTextField *_srchEntry, *_srchNote;

    /* Breakpoints */
    NSPopUpButton *_bpType;
    NSTextField *_bpStart, *_bpEnd, *_bpCond, *_bpErr, *_bpStop;
    NSStackView *_bpList;
    NSString *_bpSig;

    /* Watches */
    NSTextField *_wtEntry;
    PickTextView *_wtList;

    /* Cart & I/O, Sound */
    NSTextView *_cartText, *_soundText;

    /* Trace (0, live) and History (1, when stopped) share one ring */
    NSButton *_hRec[2];
    NSPopUpButton *_hFilter[2];
    NSTextField *_hRange[2], *_hNote[2];
    NSTextView *_hText[2];
    uint64_t _hSeen[2];

    /* Video */
    AdamVideoTab *_video;

    /* Symbols */
    PickTextView *_symList;
    NSTextField *_symFilter, *_symCount;
    NSString *_symSig;
    NSMutableArray<NSNumber *> *_symAddrs;

    /* FujiNet */
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

/* ---- helpers --------------------------------------------------------------- */

/* The engine's text as a string: control bytes dropped (a console's CRs and
 * escapes), UTF-8 if it is, otherwise Latin-1. */
static NSString *cstr(const char *source)
{
    if (!source) return @"";
    const size_t length = strlen(source);
    char *copy = malloc(length + 1);
    if (!copy) return @"";
    size_t outPos = 0;
    for (size_t index = 0; index < length; index++) {
        const unsigned char chr = (unsigned char)source[index];
        if (chr >= 0x20 || chr == '\n' || chr == '\t') copy[outPos++] = (char)chr;
    }
    copy[outPos] = '\0';
    NSString *result = [NSString stringWithUTF8String:copy];
    if (!result) result = [[NSString alloc] initWithBytes:copy length:outPos encoding:NSISOLatin1StringEncoding];
    free(copy);
    return result ?: @"";
}

/* $hex, 0xhex, hexh, #dec, or bare hex. */
static BOOL parseNum(NSString *text, long *out)
{
    NSString *token = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    int base = 16;
    if ([token hasPrefix:@"$"]) token = [token substringFromIndex:1];
    else if ([token.lowercaseString hasPrefix:@"0x"]) token = [token substringFromIndex:2];
    else if ([token hasPrefix:@"#"]) { token = [token substringFromIndex:1]; base = 10; }
    else if (token.length > 1 && [token.lowercaseString hasSuffix:@"h"]) token = [token substringToIndex:token.length - 1];
    if (token.length == 0) return NO;
    char *end;
    long number = strtol(token.UTF8String, &end, base);
    if (*end) return NO;
    *out = number;
    return YES;
}

/* A label, a number, or an expression. -1 when none. */
- (int)resolveAddr:(NSString *)text
{
    long value;
    NSString *slot = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (slot.length == 0) return -1;
    int addr = adamdebug_label_address(_dbg, slot.UTF8String);
    if (addr < 0 && parseNum(slot, &value) && value >= 0 && value <= 0xffff) addr = (int)value;
    if (addr < 0) {
        char err[160];
        err[0] = '\0';
        if (adamdebug_eval(_dbg, slot.UTF8String, &value, err, (int)sizeof err) == 0 && value >= 0 && value <= 0xffff) addr = (int)value;
    }
    return addr;
}

/* A bare run of hex digits is hex (the fields show hex); anything else is an
 * expression for the core to evaluate ($1234, 0x1234, a label, hl + 1). */
- (BOOL)evaluate:(NSString *)text into:(long *)out why:(NSString **)why
{
    NSString *trim = [text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    const char *chars = trim.UTF8String;
    char err[160];
    if (!*chars) {
        *why = @"nothing to apply";
        return NO;
    }
    char *end;
    const unsigned long hex = strtoul(chars, &end, 16);
    if (*end == '\0') {
        *out = (long)hex;
        return YES;
    }
    err[0] = '\0';
    if (adamdebug_eval(_dbg, chars, out, err, (int)sizeof err) == 0) return YES;
    *why = err[0] ? cstr(err) : @"does not parse";
    return NO;
}

static NSFont *monoFont(void)
{
    return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
}

static NSDictionary *monoAttrs(void)
{
    return @{ NSFontAttributeName: monoFont(), NSForegroundColorAttributeName: NSColor.textColor };
}

/* The accent fill with its ink on it: the PC's line, a marked row. */
static NSDictionary *accentAttrs(void)
{
    return @{ NSBackgroundColorAttributeName: accentColor(), NSForegroundColorAttributeName: accentInk() };
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
        const NSPoint origin = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
        view.string = text;
        if (scroll && !follow) {
            [scroll.contentView scrollToPoint:origin];
            [scroll reflectScrolledClipView:scroll.contentView];
        }
    }
    if (follow) [view scrollRangeToVisible:NSMakeRange(view.string.length, 0)];
}

/* The same for styled text, always where the reader left it. */
static void setStyledText(NSTextView *view, NSAttributedString *text)
{
    NSScrollView *scroll = view.enclosingScrollView;
    const NSPoint origin = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
    [view.textStorage setAttributedString:text];
    if (scroll) {
        [scroll.contentView scrollToPoint:origin];
        [scroll reflectScrolledClipView:scroll.contentView];
    }
}

/* Text for a pick list, the picked line drawn on the accent. */
static void setPickText(PickTextView *view, NSArray<NSString *> *lines)
{
    NSMutableAttributedString *text = [[NSMutableAttributedString alloc] init];
    int line = 0;
    for (NSString *source in lines) {
        NSMutableAttributedString *entry = [[NSMutableAttributedString alloc]
            initWithString:[source stringByAppendingString:@"\n"] attributes:monoAttrs()];
        if (line == view.picked) [entry addAttributes:accentAttrs() range:NSMakeRange(0, entry.length)];
        [text appendAttributedString:entry];
        line++;
    }
    setStyledText(view, text);
}

static PickTextView *makePickView(NSScrollView **scrollOut)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = YES;
    PickTextView *view = [[PickTextView alloc] initWithFrame:NSMakeRect(0, 0, 900, 400)];
    view.editable = NO;
    view.richText = YES;
    view.selectable = NO;
    view.font = monoFont();
    view.horizontallyResizable = YES;
    view.textContainer.widthTracksTextView = NO;
    view.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    view.picked = -1;
    scroll.documentView = view;
    *scrollOut = scroll;
    return view;
}

- (NSButton *)button:(NSString *)title action:(SEL)sel
{
    NSButton *button = [NSButton buttonWithTitle:title target:self action:sel];
    [button setRefusesFirstResponder:YES];
    return button;
}

- (NSTextField *)label:(NSString *)text
{
    return [NSTextField labelWithString:text];
}

- (NSTextField *)hint:(NSString *)text
{
    NSTextField *label = [self label:text];
    label.textColor = NSColor.secondaryLabelColor;
    return label;
}

- (NSTextField *)field:(NSString *)placeholder width:(CGFloat)width action:(SEL)sel
{
    NSTextField *field = [[NSTextField alloc] init];
    field.placeholderString = placeholder;
    field.font = monoFont();
    field.target = self;
    field.action = sel;
    if (width > 0) [field.widthAnchor constraintEqualToConstant:width].active = YES;
    return field;
}

/* A pull-down button: `title` on its face, `items` underneath (each one's
 * tag is its index). */
- (NSPopUpButton *)pullDown:(NSString *)title items:(NSArray<NSString *> *)items action:(SEL)sel
{
    NSPopUpButton *popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:YES];
    [popup addItemWithTitle:title];
    popup.lastItem.tag = -1;
    for (NSUInteger index = 0; index < items.count; index++) {
        [popup addItemWithTitle:items[index]];
        popup.lastItem.tag = (NSInteger)index;
    }
    popup.target = self;
    popup.action = sel;
    [popup setRefusesFirstResponder:YES];
    return popup;
}

static NSStackView *hstack(NSArray<NSView *> *views)
{
    NSStackView *stack = [NSStackView stackViewWithViews:views];
    stack.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    stack.spacing = 6;
    return stack;
}

static NSStackView *vstack(NSArray<NSView *> *views)
{
    NSStackView *stack = [NSStackView stackViewWithViews:views];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 6;
    return stack;
}

static void fill(NSStackView *stack, NSView *view)
{
    [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
}

/* Why the machine stopped, worded by the core. Empty while running. */
static NSString *stopText(const adamdebug_snapshot *snap)
{
    return snap->paused ? cstr(snap->text) : @"";
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
        /* windowWillClose: records off, the machine runs on */
        [g_debugger->_window close];
        return;
    }
    [self showForSession:session];
}

- (void)show
{
    if (_window.isMiniaturized) [_window deminiaturize:nil];
    [_window makeKeyAndOrderFront:nil];
    for (int slot = 0; slot < TAB_COUNT; slot++) [_popped[slot] orderFront:nil];
    adamdebug_attach(_dbg);     /* idempotent; stops the machine */
    [self applyEnvironment];
    [self refreshNow];
    if ([self tabVisible:MT_CONSOLE]) [_window makeFirstResponder:_conIn];
}

/* The captures' hooks, once the window is up (closing it turns the Trace and
 * History records off, so they could not be set before):
 * ADAM_DEBUGGER_WATCH="x" adds a watch, ADAM_DEBUGGER_WATCH_BREAK="x" a breakpoint on a write
 * that changes it, ADAM_DEBUGGER_MEM_GOTO="x" shows it in Memory, ADAM_DEBUGGER_HISTORY records
 * the History ring, ADAM_DEBUGGER_POPOUT=<tab> pops that tab out into a window. */
- (void)applyEnvironment
{
    if (_envApplied) return;
    _envApplied = YES;
    const char *watchText = getenv("ADAM_DEBUGGER_WATCH"), *breakText = getenv("ADAM_DEBUGGER_WATCH_BREAK"),
               *gotoText = getenv("ADAM_DEBUGGER_MEM_GOTO");
    uint16_t first, last;
    if (watchText && *watchText) adamwatch_add(watchText);
    if (breakText && *breakText && adamsym_parse_range(_dbg, breakText, &first, &last))
        [self breakOnChangeFrom:first to:last];
    if (gotoText && *gotoText && adamsym_parse_range(_dbg, gotoText, &first, &last))
        [self showInMemoryFrom:first last:last];
    if (getenv("ADAM_DEBUGGER_HISTORY")) adamdebug_history_enable(_dbg, 1);
    const int pop = tabFromText(getenv("ADAM_DEBUGGER_POPOUT"));
    if (pop >= 0) [self popOutTab:pop];
}

- (instancetype)initWithSession:(adamsession *)session
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _dbg = adamsession_debugger(session);
    _fujiCount = -1;
    _selected = -1;
    _hSeen[0] = _hSeen[1] = (uint64_t)-1;
    _history = [NSMutableArray array];
    _symAddrs = [NSMutableArray array];
    [self buildWindow];

    __weak DebuggerWindow *weakSelf = self;
    _tick = [NSTimer timerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *timer) {
        (void)timer;
        [weakSelf onTick];
    }];
    [[NSRunLoop mainRunLoop] addTimer:_tick forMode:NSRunLoopCommonModes];

    /* F5/F7/F8/F9/F12 wherever the focus is in this window (or a tab peeled
     * off into its own), the console's field included */
    _keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                        handler:^NSEvent *(NSEvent *event) {
        DebuggerWindow *owner = weakSelf;
        if (!owner || ![owner ownsWindow:event.window]) return event;
        return [owner handleKey:event] ? nil : event;
    }];
    return self;
}

- (void)dealloc
{
    [_tick invalidate];
    if (_keyMonitor) [NSEvent removeMonitor:_keyMonitor];
}

- (BOOL)ownsWindow:(NSWindow *)window
{
    if (!window) return NO;
    if (window == _window) return _window.isVisible;
    for (int slot = 0; slot < TAB_COUNT; slot++)
        if (window == _popped[slot]) return YES;
    return NO;
}

/* The main window's close control and Window > Close: a peeled-off tab's
 * window docks it instead. */
- (BOOL)windowShouldClose:(NSWindow *)sender
{
    for (int slot = 0; slot < TAB_COUNT; slot++)
        if (sender == _popped[slot]) {
            [self dockTab:slot];
            return NO;
        }
    return YES;
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object != _window) return;
    for (int slot = 0; slot < TAB_COUNT; slot++) [_popped[slot] orderOut:nil];
    adamdebug_trace_enable(_dbg, 0);
    adamdebug_history_enable(_dbg, 0);
    adamdebug_detach(_dbg);     /* the machine runs on; breakpoints are kept */
    _envApplied = YES;      /* the hooks are for the first opening only */
}

/* On screen: the tab view's selected page, or a window of its own. */
- (BOOL)tabVisible:(int)tab
{
    return _popped[tab] != nil || _tabs.selectedTabViewItem == _items[tab];
}

/* The poll: refresh on a stop (its serial), a resume or a command (the
 * generation), and the visible tabs' live values twice a second while the
 * machine runs. The Video and Trace tabs follow a running machine faster, and
 * the FujiNet tab also follows the trace while stopped, as the AdamNet
 * packets arrive on their own. */
- (void)onTick
{
    if (![_window isVisible]) return;
    adamdebug_snapshot snap;
    adamdebug_stop_snapshot(_dbg, &snap);
    const unsigned gen = adamdebug_generation(_dbg);
    const BOOL stopped = snap.paused != 0;
    if (snap.serial != _seenSerial || gen != _seenGen || stopped != _wasStopped) {
        [self refreshNow];
        return;
    }
    if (!stopped && [self tabVisible:MT_VIDEO] && ++_videoTicks >= VIDEO_TICKS) {
        _videoTicks = 0;
        [_video refreshForce:NO];
    }
    if ([self tabVisible:MT_TRACE] && ++_traceTicks >= TRACE_TICKS) {
        _traceTicks = 0;
        [self refreshHist:0 force:NO];
    }
    if (++_liveTicks >= LIVE_TICKS) {
        _liveTicks = 0;
        if (!stopped) {
            [self refreshStatus];
            [self refreshVisibleForce:NO];
        } else if ([self tabVisible:MT_FUJINET]) {
            [self refreshFujinet:NO];
        }
    }
}

/* Waits up to ~40 ms for a stop to land after the engine was asked to move:
 * the generation moves on and the machine reads as stopped. */
- (void)settleFrom:(unsigned)gen
{
    for (int attempt = 0; attempt < 20; attempt++) {
        if (adamdebug_generation(_dbg) != gen && adamdebug_is_stopped(_dbg)) break;
        usleep(2000);
    }
}

/* Help's contexts (adamhelp.h): the tab identifiers are the contexts' ends */
- (void)updateHelpContext
{
    NSString *ident = _tabs.selectedTabViewItem.identifier;
    if (ident) AdamSetHelpContext(_window, [@"debugger/" stringByAppendingString:ident]);
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    (void)item;
    [self updateHelpContext];
    [self refreshVisibleForce:YES];
    if ([self tabVisible:MT_CONSOLE]) [_window makeFirstResponder:_conIn];
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

/* A tab's content, padded and pinned on all four sides of its page. */
- (NSView *)pageHolding:(NSView *)content
{
    NSView *holder = [[NSView alloc] init];
    [holder addSubview:content];
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
    [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
    [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
    [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
    return holder;
}

- (NSView *)buildConsole
{
    NSScrollView *outScroll;
    _conOut = monoView(&outScroll, YES);
    _conOut.string = @"Debugger console. Type 'help' for every command; anything else is an error. "
                     @"Tab completes, Up/Down walk the history.\n";
    _conIn = [self field:@"command (help, step, break $0038, bpa $C000, regs, print hl+1, mem $C000 32, video, ...)"
                   width:0 action:@selector(runCommand:)];
    _conIn.delegate = self;

    NSStackView *column = vstack(@[outScroll, _conIn]);
    fill(column, outScroll);
    fill(column, _conIn);
    return column;
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
        DebuggerWindow *owner = weakSelf;
        if (!owner || line < 0 || line >= owner->_lineCount) return;
        const BOOL gutter = column < 2;
        owner->_selected = owner->_lineAddr[line];
        if (gutter ? clicks == 1 : clicks == 2) [owner toggleBreakpointAt:owner->_lineAddr[line]];
        else [owner refreshDisasm];
    };
    _disasm.onScroll = ^(int rows) {
        DebuggerWindow *owner = weakSelf;
        if (!owner || rows == 0) return;
        owner->_followPc.state = NSControlStateValueOff;
        owner->_disasmAddr = (uint16_t)adamdebug_row_address(owner->_dbg, owner->_disasmAddr, rows);
        [owner refreshDisasm];
    };
    scroll.documentView = _disasm;

    NSStackView *column = vstack(@[row, scroll]);
    fill(column, row);
    fill(column, scroll);
    return column;
}

/* The 64K as the CPU sees it: a go-to bar, a search, the dump and a Poke bar. */
- (NSView *)buildMemory
{
    NSScrollView *memScroll;
    _mem = monoView(&memScroll, NO);
    _memGoto = [self field:@"address or label" width:170 action:@selector(gotoMem:)];
    NSButton *prev = [self button:@"◀" action:@selector(memPrev:)];
    NSButton *next = [self button:@"▶" action:@selector(memNext:)];
    NSStackView *memHead = hstack(@[[self label:@"Address:"], _memGoto, [self button:@"Go" action:@selector(gotoMem:)],
                                    prev, next, [self hint:@"as the CPU sees it, through the bank selects"]]);

    _srchKind = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_srchKind addItemsWithTitles:@[@"Hex bytes", @"Text"]];
    _srchKind.toolTip = @"Hex bytes (3E 00 C9) or text, one character per byte";
    _srchEntry = [self field:@"3E 00 C9, or some text" width:0 action:@selector(searchNext:)];
    _srchNote = [self hint:@""];
    NSStackView *searchRow = hstack(@[[self label:@"Search"], _srchKind, _srchEntry,
                                      [self button:@"Find Next" action:@selector(searchNext:)],
                                      [self button:@"Find Previous" action:@selector(searchPrev:)], _srchNote]);
    searchRow.distribution = NSStackViewDistributionFill;

    _memWrite = [self field:@"address value ... (hex; several values go to consecutive addresses)" width:0
                     action:@selector(writeMem:)];
    _memNote = [self hint:@""];
    NSStackView *pokeRow = hstack(@[[self label:@"Poke"], _memWrite, [self button:@"Write" action:@selector(writeMem:)],
                                    _memNote]);
    pokeRow.distribution = NSStackViewDistributionFill;

    NSStackView *column = vstack(@[memHead, searchRow, memScroll, pokeRow]);
    fill(column, memHead);
    fill(column, searchRow);
    fill(column, memScroll);
    fill(column, pokeRow);
    [memScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    return column;
}

/* One breakpoint-table cell: a fixed-width monospace label, or (width 0) the
 * flexible one that takes what is left. */
static NSTextField *bpCell(NSString *text, CGFloat width, BOOL bold)
{
    NSTextField *label = [NSTextField labelWithString:text];
    label.font = bold ? [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightSemibold] : monoFont();
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    if (width > 0) {
        [label.widthAnchor constraintEqualToConstant:width].active = YES;
    } else {
        [label setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
        [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];
    }
    return label;
}

- (NSView *)buildBreaks
{
    /* the table's rows scroll: a session can collect more breakpoints than
     * the tab is tall. The stack is the scroll view's document, pinned to
     * the top and to the clip view's width so rows keep the tab's width. */
    NSStackView *header = hstack(@[bpCell(@"On  #", 70, YES), bpCell(@"Type", 60, YES), bpCell(@"Range", 190, YES),
                                   bpCell(@"Condition", 0, YES), bpCell(@"Hits", 60, YES), bpCell(@"", 80, YES), bpCell(@"", 70, YES)]);
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
    [_bpType addItemsWithTitles:@[@"Execute", @"Read", @"Write", @"Access", @"In", @"Out"]];
    _bpType.toolTip = @"Execute stops before an instruction; Read, Write and Access stop after the instruction "
                      @"that touched the bytes; In and Out watch I/O ports";
    _bpStart = [self field:@"$4010, a label, or port $A1" width:190 action:@selector(addBreakpoint:)];
    _bpEnd = [self field:@"end (optional)" width:120 action:@selector(addBreakpoint:)];
    _bpCond = [self field:@"condition (optional): value != old, a == 5, [$C000] > 3" width:0 action:@selector(addBreakpoint:)];
    _bpCond.toolTip = @"An expression: the Z80 registers and flags, [addr] and {addr}, labels, frame, cycles, vline "
                      @"and, on an access, value, addr and old (the byte before a write).";
    NSButton *add = [self button:@"Add" action:@selector(addBreakpoint:)];
    NSButton *clear = [self button:@"Clear All" action:@selector(clearBreaks:)];
    NSButton *hits = [self button:@"Clear Hits" action:@selector(clearHits:)];
    NSStackView *row = hstack(@[_bpType, [self label:@"Start"], _bpStart, [self label:@"End"], _bpEnd,
                                [self label:@"Condition"], _bpCond, add, hits, clear]);
    row.distribution = NSStackViewDistributionFill;
    _bpErr = [self label:@""];
    _bpErr.textColor = NSColor.systemRedColor;
    _bpStop = [self hint:@""];
    NSTextField *hint = [self hint:@"Untick to disable. A port is 0-255 for In and Out; the end defaults to the start."];

    NSStackView *column = vstack(@[header, bpScroll, hint, row, _bpErr, _bpStop]);
    fill(column, header);
    fill(column, bpScroll);
    fill(column, row);
    [bpScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    return column;
}

- (NSView *)buildWatches
{
    _wtEntry = [self field:@"address, label, expression, or lo-hi" width:0 action:@selector(addWatch:)];
    NSStackView *form = hstack(@[[self label:@"Watch"], _wtEntry, [self button:@"Add" action:@selector(addWatch:)]]);
    form.distribution = NSStackViewDistributionFill;

    NSScrollView *scroll;
    _wtList = makePickView(&scroll);
    __weak DebuggerWindow *weakSelf = self;
    _wtList.onPick = ^(int line, NSInteger clicks) {
        (void)line;
        (void)clicks;
        [weakSelf refreshWatches];
    };

    NSStackView *foot = hstack(@[[self button:@"Memory" action:@selector(wtMemory:)],
                                 [self button:@"Break on Change" action:@selector(wtBreak:)],
                                 [self button:@"Remove" action:@selector(wtRemove:)],
                                 [self hint:@"Values show while the machine is stopped"]]);

    NSStackView *column = vstack(@[form, scroll, foot]);
    fill(column, form);
    fill(column, scroll);
    [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    return column;
}

- (NSView *)buildTextTab:(NSTextView *__strong *)view
{
    NSScrollView *scroll;
    *view = monoView(&scroll, NO);
    return scroll;
}

- (NSView *)buildHist:(int)idx
{
    _hRec[idx] = [self button:@"Record" action:@selector(histRec:)];
    [_hRec[idx] setButtonType:NSButtonTypePushOnPushOff];
    _hRec[idx].tag = idx;
    _hRec[idx].toolTip = @"Record every instruction the CPU runs (the machine runs a little slower)";
    _hFilter[idx] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_hFilter[idx] addItemsWithTitles:@[@"All instructions", @"Branches only", @"In range"]];
    _hFilter[idx].target = self;
    _hFilter[idx].action = @selector(histChanged:);
    _hRange[idx] = [self field:@"range: lo-hi or a label" width:200 action:@selector(histChanged:)];
    _hNote[idx] = [self hint:@""];
    NSButton *clear = [self button:@"Clear" action:@selector(histClear:)];
    NSStackView *bar = hstack(@[_hRec[idx], _hFilter[idx], _hRange[idx], clear, _hNote[idx]]);
    NSScrollView *scroll;
    _hText[idx] = monoView(&scroll, NO);
    NSStackView *column = vstack(@[bar, scroll]);
    fill(column, bar);
    fill(column, scroll);
    return column;
}

- (NSView *)buildVideo
{
    _video = [[AdamVideoTab alloc] initWithDebugger:_dbg];
    NSView *view = _video.view;
    NSStackView *column = vstack(@[view]);
    [view.widthAnchor constraintEqualToAnchor:column.widthAnchor].active = YES;
    [view.heightAnchor constraintEqualToAnchor:column.heightAnchor].active = YES;
    return column;
}

- (NSView *)buildSymbols
{
    _symFilter = [self field:@"Filter (Enter to apply)" width:0 action:@selector(symFilterChanged:)];
    _symCount = [self hint:@""];
    NSStackView *filterRow = hstack(@[_symFilter, _symCount]);
    filterRow.distribution = NSStackViewDistributionFill;

    NSScrollView *scroll;
    _symList = makePickView(&scroll);
    __weak DebuggerWindow *weakSelf = self;
    _symList.onPick = ^(int line, NSInteger clicks) {
        DebuggerWindow *owner = weakSelf;
        if (!owner || line < 0 || line >= (int)owner->_symAddrs.count) return;
        owner->_symSig = nil;       /* redraw with the pick */
        [owner refreshSymbols];
        if (clicks >= 2) {      /* activating a row goes to that address */
            const int addr = owner->_symAddrs[(NSUInteger)line].intValue;
            [owner selectTab:MT_MEMORY];
            [owner showInMemoryFrom:(uint16_t)addr last:(uint16_t)addr];
        }
    };
    NSTextField *hint = [self hint:@"Double-click a row to show its address in Memory"];

    NSStackView *column = vstack(@[filterRow, scroll, hint]);
    fill(column, filterRow);
    fill(column, scroll);
    [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationVertical];
    return column;
}

/* The packets left (a note and Clear over them), the AdamNet link and
 * counts right. */
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
    NSTextField *title = [self label:@"FujiBus packets (SLIP-decoded)"];
    title.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    NSStackView *left = vstack(@[title, row, fujiScroll]);
    left.distribution = NSStackViewDistributionFill;   /* the log takes the column's height */
    fill(left, row);
    fill(left, fujiScroll);
    NSTextField *stateTitle = [self label:@"FujiNet"];
    stateTitle.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    NSStackView *right = vstack(@[stateTitle, stateScroll]);
    right.distribution = NSStackViewDistributionFill;
    fill(right, stateScroll);

    NSStackView *both = hstack(@[left, right]);
    both.alignment = NSLayoutAttributeTop;
    both.spacing = 12;
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:520].active = YES;
    [right.widthAnchor constraintEqualToConstant:320].active = YES;
    [left.heightAnchor constraintEqualToAnchor:both.heightAnchor].active = YES;
    [right.heightAnchor constraintEqualToAnchor:both.heightAnchor].active = YES;
    return both;
}

/* The Z80's registers under every tab, each a field that takes a value (hex,
 * $hex, a label or an expression) while the machine is stopped; Enter applies
 * it (adamdebug_regs_set). The flags are checkboxes. */
- (NSView *)buildRegStrip
{
    static NSString *const names[NUM_REGS] = {
        @"AF", @"BC", @"DE", @"HL", @"IX", @"IY", @"SP", @"PC",
        @"AF'", @"BC'", @"DE'", @"HL'", @"I", @"R", @"IM", @"IFF1", @"IFF2"
    };
    NSMutableArray *row1 = [NSMutableArray array], *row2 = [NSMutableArray array];
    for (int idx = 0; idx < NUM_REGS; idx++) {
        /* the 16-bit pairs, then I and R, then IM and the two flip-flops */
        const CGFloat width = idx < 12 ? 52 : (idx < 14 ? 36 : 28);
        NSMutableArray *row = idx < 8 ? row1 : row2;
        [row addObject:[self label:names[idx]]];
        _reg[idx] = [self field:@"" width:width action:@selector(applyRegister:)];
        _reg[idx].tag = idx;
        _reg[idx].toolTip = @"Edit while the machine is stopped (hex, $hex, a label or an expression), then Enter. "
                          @"IM is 0-2, IFF1 and IFF2 are 0 or 1";
        [row addObject:_reg[idx]];
    }
    static NSString *const fnames[NUM_FLAGS] = { @"S", @"Z", @"H", @"PV", @"N", @"C" };
    NSMutableArray *flags = [NSMutableArray arrayWithObject:[self label:@"Flags"]];
    for (int idx = 0; idx < NUM_FLAGS; idx++) {
        _flag[idx] = [NSButton checkboxWithTitle:fnames[idx] target:self action:@selector(flagClicked:)];
        _flag[idx].tag = idx;
        [flags addObject:_flag[idx]];
    }
    _regInfo = [self hint:@""];
    _regInfo.font = monoFont();
    [flags addObject:_regInfo];
    return vstack(@[hstack(row1), hstack(row2), hstack(flags)]);
}

- (void)buildWindow
{
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1180, 860)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"Debugger";
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
    NSPopUpButton *symbols = [self pullDown:@"Symbols" items:@[@"Load Symbols…", @"Clear Symbols"]
                                     action:@selector(symbolsMenu:)];
    NSMutableArray *saveTitles = [NSMutableArray array];
    for (int entry = 0; entry < NUM_FILE_KINDS; entry++) [saveTitles addObject:kFileTitles[entry]];
    NSPopUpButton *save = [self pullDown:@"Save…" items:saveTitles action:@selector(saveFile:)];
    NSButton *popOut = [self button:@"Pop Out" action:@selector(popOutCurrent:)];
    popOut.toolTip = @"Move the tab that shows into a window of its own; Dock puts it back";
    _status = [self label:@"Running"];
    _status.alignment = NSTextAlignmentRight;
    _status.textColor = NSColor.secondaryLabelColor;
    _status.lineBreakMode = NSLineBreakByTruncatingTail;
    NSButton *helpBtn = AdamHelpButton(nil);
    [helpBtn setRefusesFirstResponder:YES];
    NSStackView *toolbar = hstack(@[_runBtn, step, over, outBtn, toCursor, frame, symbols, save, popOut, _status, helpBtn]);
    [_status setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_status setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                      forOrientation:NSLayoutConstraintOrientationHorizontal];

    _tabs = [[NSTabView alloc] init];
    NSArray<NSView *> *pages = @[
        [self buildConsole], [self buildDisasm], [self buildMemory], [self buildBreaks], [self buildWatches],
        [self buildTextTab:&_cartText], [self buildHist:0], [self buildHist:1], [self buildVideo],
        [self buildTextTab:&_soundText], [self buildSymbols], [self buildFujinet]
    ];
    for (int idx = 0; idx < TAB_COUNT; idx++) {   /* each keeps its place when moved around */
        NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:@(kTabIds[idx])];
        item.label = @(kTabNames[idx]);
        item.view = [self pageHolding:pages[(NSUInteger)idx]];
        _items[idx] = item;
        [_tabs addTabViewItem:item];
    }
    const int first = tabFromText(getenv("ADAM_DEBUGGER_TAB"));
    if (first >= 0) [_tabs selectTabViewItemAtIndex:first];
    _tabs.delegate = self;
    [self updateHelpContext];

    /* The registers sit under every tab. */
    NSView *strip = [self buildRegStrip];

    NSStackView *root = vstack(@[toolbar, _tabs, strip]);
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [toolbar.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [_tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [strip.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [_tabs.heightAnchor constraintGreaterThanOrEqualToConstant:620].active = YES;
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

- (void)step:(id)sender { (void)sender; const unsigned genNow = adamdebug_generation(_dbg); adamdebug_step(_dbg); [self moved:genNow]; }
- (void)stepOver:(id)sender { (void)sender; const unsigned genNow = adamdebug_generation(_dbg); adamdebug_step_over(_dbg); [self moved:genNow]; }
- (void)stepOut:(id)sender { (void)sender; const unsigned genNow = adamdebug_generation(_dbg); adamdebug_step_out(_dbg); [self moved:genNow]; }
- (void)frame:(id)sender { (void)sender; const unsigned genNow = adamdebug_generation(_dbg); adamdebug_frame(_dbg, 1); [self moved:genNow]; }

- (void)runToCursor:(id)sender
{
    (void)sender;
    if (_selected < 0) {
        [self message:@"Select a line in the Disassembly to run to" console:NO];
        return;
    }
    const unsigned genNow = adamdebug_generation(_dbg);
    adamdebug_run_to(_dbg, (uint16_t)_selected);
    [self moved:genNow];
}

- (void)toggleBreakpointAt:(uint16_t)addr
{
    adamdebug_breakpoint_toggle(_dbg, addr);
    [self refreshDisasm];
    if ([self tabVisible:MT_BREAKS]) [self refreshBps];
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
}

- (void)fujiClear:(id)sender
{
    (void)sender;
    adamdebug_fujinet_trace_clear(_dbg);
    [self refreshFujinet:YES];
}

- (void)selectTab:(int)tab
{
    if (_popped[tab]) [_popped[tab] makeKeyAndOrderFront:nil];
    else [_tabs selectTabViewItem:_items[tab]];
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
    adamdebug_command(_dbg, cmd.UTF8String, out, sizeof out);
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
        const NSInteger total = (NSInteger)_history.count;
        if (total == 0) return YES;
        _historyPos += sel == @selector(moveUp:) ? -1 : 1;
        if (_historyPos < 0) _historyPos = 0;
        if (_historyPos > total) _historyPos = total;
        _conIn.stringValue = _historyPos < total ? _history[(NSUInteger)_historyPos] : @"";
        [_conIn.currentEditor setSelectedRange:NSMakeRange(_conIn.stringValue.length, 0)];
        return YES;
    }
    if (sel != @selector(insertTab:)) return NO;
    /* the engine takes the whole line and completes its last word */
    NSString *text = _conIn.stringValue;
    const NSRange space = [text rangeOfString:@" " options:NSBackwardsSearch];
    char comps[4096];
    comps[0] = '\0';
    const int matches = adamdebug_completions(_dbg, text.UTF8String, comps, sizeof comps);
    NSString *list = cstr(comps);
    if (matches == 1) {
        NSString *first = [list componentsSeparatedByString:@"\n"][0];
        NSString *head = space.location == NSNotFound ? @"" : [text substringToIndex:space.location + 1];
        _conIn.stringValue = [NSString stringWithFormat:@"%@%@ ", head, first];
        [_conIn.currentEditor setSelectedRange:NSMakeRange(_conIn.stringValue.length, 0)];
    } else if (matches > 1) {
        if (![list hasSuffix:@"\n"]) list = [list stringByAppendingString:@"\n"];
        [self appendConsole:list];
    }
    return YES;
}

/* The Symbols pull-down: Load Symbols… or Clear Symbols. */
- (void)symbolsMenu:(NSPopUpButton *)sender
{
    if (sender.selectedItem.tag == 1) {
        adamdebug_clear_symbols(_dbg);
        [self message:@"Symbols cleared (the BIOS names stay)" console:YES];
        [self refreshNow];
        return;
    }
    char msg[512];
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title = @"Load Symbols (\"HHHH NAME\" per line)";
    if ([panel runModal] != NSModalResponseOK) return;
    msg[0] = '\0';
    adamdebug_load_symbols(_dbg, [[panel URL] fileSystemRepresentation], msg, sizeof msg);
    [self message:cstr(msg) console:YES];
    [self refreshNow];
}

- (void)saveFile:(NSPopUpButton *)sender
{
    const NSInteger kind = sender.selectedItem.tag;
    if (kind < 0 || kind >= NUM_FILE_KINDS) return;
    NSSavePanel *panel = [NSSavePanel savePanel];
    panel.title = [NSString stringWithFormat:@"Save %@", kFileTitles[kind]];
    [panel setNameFieldStringValue:kFileNames[kind]];
    if ([panel runModal] != NSModalResponseOK) return;
    char msg[512];
    msg[0] = '\0';
    adamdebug_save(_dbg, kFileKinds[kind], [[panel URL] fileSystemRepresentation], msg, sizeof msg);
    [self message:cstr(msg) console:YES];
}

/* A register field's text applied through adamdebug_regs_set, which writes
 * them all at once: the other fields keep what they have now. */
- (void)applyRegister:(NSTextField *)sender
{
    long value = 0;
    NSString *why = nil;
    [_window makeFirstResponder:nil];       /* leave the field, so the refresh may rewrite it */
    if (!adamdebug_is_stopped(_dbg)) {
        [self message:@"Stop the machine (F5) to edit registers" console:NO];
    } else if (sender.tag < 0 || sender.tag >= NUM_REGS) {
        /* not one of ours */
    } else if (![self evaluate:sender.stringValue into:&value why:&why]) {
        [self message:[NSString stringWithFormat:@"Register: %@", why] console:NO];
    } else {
        /* the engine's own register names, in the strip's order */
        static const char *const regNames[NUM_REGS] = {
            "af", "bc", "de", "hl", "ix", "iy", "sp", "pc", "af'", "bc'", "de'", "hl'", "i", "r", "im", "iff1", "iff2"
        };
        char err[160];
        err[0] = '\0';
        if (!adamdebug_register_set(_dbg, regNames[sender.tag], value, err, (int)sizeof err))
            [self message:[NSString stringWithFormat:@"Register: %@", err[0] ? cstr(err) : @"refused"] console:NO];
        else if (err[0])
            [self message:cstr(err) console:NO];       /* a note: PC set while the CPU is halted */
    }
    [self refreshNow];
}

- (void)flagClicked:(NSButton *)sender
{
    static const char *const flagNames[NUM_FLAGS] = { "sf", "zf", "hf", "pf", "nf", "cf" };
    char err[160];
    err[0] = '\0';
    if (sender.tag < 0 || sender.tag >= NUM_FLAGS)
        return;
    if (adamdebug_is_stopped(_dbg))
        adamdebug_register_set(_dbg, flagNames[sender.tag], sender.state == NSControlStateValueOn, err, (int)sizeof err);
    else
        [self message:@"Stop the machine (F5) to edit flags" console:NO];
    [self refreshRegs];
}

/* Shows a range in the Memory tab, highlighted, a couple of rows down from
 * the top. */
- (void)showInMemoryFrom:(uint16_t)first last:(uint16_t)last
{
    _memBase = (first & ~0xF) - 2 * 16;
    _memHl = YES;
    _memHlLo = first;
    _memHlHi = last;
    [self refreshMem];
}

- (void)gotoMem:(id)sender
{
    (void)sender;
    uint16_t first, last;
    if (!adamsym_parse_range(_dbg, _memGoto.stringValue.UTF8String, &first, &last)) {
        [self message:[NSString stringWithFormat:@"Not an address or label: %@", _memGoto.stringValue] console:NO];
        return;
    }
    [self showInMemoryFrom:first last:last];
}

- (void)memPrev:(id)sender { (void)sender; _memBase -= MEM_BYTES; [self refreshMem]; }
- (void)memNext:(id)sender { (void)sender; _memBase += MEM_BYTES; [self refreshMem]; }

/* "address value [value...]": the values land at consecutive addresses. */
- (void)writeMem:(id)sender
{
    (void)sender;
    NSArray<NSString *> *words = [_memWrite.stringValue componentsSeparatedByCharactersInSet:
        [NSCharacterSet characterSetWithCharactersInString:@" \t,"]];
    NSMutableArray<NSString *> *tok = [NSMutableArray array];
    for (NSString *piece in words) if (piece.length) [tok addObject:piece];
    const int address = tok.count > 0 ? [self resolveAddr:tok[0]] : -1;
    if (tok.count < 2 || address < 0) {
        _memNote.stringValue = @"Poke: address value [value...]";
        return;
    }
    if (!adamdebug_is_stopped(_dbg)) {
        _memNote.stringValue = @"Stop the machine (F5) to poke it";
        return;
    }
    for (NSUInteger idx = 1; idx < tok.count; idx++) {
        long value;
        if (!parseNum(tok[idx], &value) || value < 0 || value > 0xFF || address + (int)idx - 1 > 0xFFFF) {
            _memNote.stringValue = [NSString stringWithFormat:@"Not a byte: %@", tok[idx]];
            return;
        }
        const uint8_t byte = (uint8_t)value;
        adamdebug_write(_dbg, (uint16_t)(address + (int)idx - 1), byte);
    }
    _memWrite.stringValue = @"";
    _memNote.stringValue = [NSString stringWithFormat:@"Wrote %d byte%s at $%04X (RAM takes it; ROM ignores it)",
                            (int)tok.count - 1, tok.count == 2 ? "" : "s", address];
    if (address < _memBase || address >= _memBase + MEM_BYTES) [self showInMemoryFrom:(uint16_t)address last:(uint16_t)address];
    else [self refreshMem];
}

- (void)searchBackward:(BOOL)backward
{
    uint8_t pat[SEARCH_MAX], mask = 0xFF;
    int count = 0, flags = 0;
    uint16_t found;
    if (!adamsearch_parse((int)_srchKind.indexOfSelectedItem, _srchEntry.stringValue.UTF8String, pat, &count, &mask,
                         &flags) || count < 1) {
        _srchNote.stringValue = @"Type hex bytes (3E 00 C9) or some text.";
        return;
    }
    if (!adamdebug_is_stopped(_dbg)) {
        _srchNote.stringValue = @"Stop the machine (F5) to search its memory.";
        return;
    }
    const uint16_t from = _memHl ? (backward ? _memHlLo : (uint16_t)(_memHlLo + 1)) : (uint16_t)_memBase;
    const int hit = adamdebug_search_next(_dbg, pat, count, mask, flags, from, backward ? 1 : 0, &found);
    if (hit > 0) {
        _srchNote.stringValue = [NSString stringWithFormat:@"Found at $%04X", found];
        [self showInMemoryFrom:found last:(uint16_t)(found + count - 1)];
    } else {
        _srchNote.stringValue = hit == 0 ? @"Not found." : @"Stop the machine (F5) to search its memory.";
    }
}

- (void)searchNext:(id)sender { (void)sender; [self searchBackward:NO]; }
- (void)searchPrev:(id)sender { (void)sender; [self searchBackward:YES]; }

- (void)followToggled:(id)sender
{
    (void)sender;
    [self refreshDisasm];
}

- (void)jumpTo:(id)sender
{
    (void)sender;
    const int addr = [self resolveAddr:_jump.stringValue];
    if (addr < 0) {
        [self message:[NSString stringWithFormat:@"Not an address or label: %@", _jump.stringValue] console:NO];
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
    static const int types[6] = { ADAMDEBUG_BP_EXEC, ADAMDEBUG_BP_READ, ADAMDEBUG_BP_WRITE, ADAMDEBUG_BP_ACCESS,
                                  ADAMDEBUG_BP_IN, ADAMDEBUG_BP_OUT };
    const NSInteger sel = _bpType.indexOfSelectedItem;
    const int type = types[sel >= 0 && sel < 6 ? sel : 0];
    const BOOL port = (type & (ADAMDEBUG_BP_IN | ADAMDEBUG_BP_OUT)) != 0;
    NSString *what = port ? @"port" : @"address";
    NSString *startText = [_bpStart.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    NSString *endText = [_bpEnd.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (startText.length == 0) {
        _bpErr.stringValue = [NSString stringWithFormat:@"Give a %@", what];
        return;
    }
    const int start = [self resolveAddr:startText];
    const int end = endText.length ? [self resolveAddr:endText] : start;
    if (start < 0 || end < 0 || end < start || (port && end > 0xFF)) {
        _bpErr.stringValue = [NSString stringWithFormat:@"Bad %@ range: %@%@%@", what, startText,
                              endText.length ? @" - " : @"", endText];
        return;
    }
    char err[256];
    err[0] = '\0';
    const int bpId = adamdebug_breakpoint_add(_dbg, type, (uint16_t)start, (uint16_t)end,
                                             _bpCond.stringValue.UTF8String, err, sizeof err);
    if (bpId < 0) {
        NSString *reason = cstr(err);
        _bpErr.stringValue = reason.length ? reason : @"The breakpoint was refused";
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

/* Replaces the entry's condition with the Condition field's text. */
- (void)bpSetCondition:(NSButton *)sender
{
    char err[256];
    err[0] = '\0';
    if (!adamdebug_breakpoint_set_condition(_dbg, (int)sender.tag, _bpCond.stringValue.UTF8String, err, (int)sizeof err)) {
        NSString *reason = cstr(err);
        _bpErr.stringValue = reason.length ? reason : @"The condition was refused";
        return;
    }
    _bpErr.stringValue = @"";
    _bpCond.stringValue = @"";
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

- (void)clearHits:(id)sender
{
    (void)sender;
    adamdebug_breakpoint_clear_hits(_dbg);
    [self refreshNow];
}

- (void)addWatch:(id)sender
{
    (void)sender;
    if (adamwatch_add(_wtEntry.stringValue.UTF8String) < 0) {
        [self message:@"Type an address, label, expression or lo-hi range to watch" console:NO];
        return;
    }
    _wtEntry.stringValue = @"";
    [self refreshWatches];
}

- (void)wtMemory:(id)sender
{
    (void)sender;
    uint16_t first, last;
    if (_wtList.picked >= 0 && adamwatch_range(_dbg, _wtList.picked, &first, &last)) {
        [self selectTab:MT_MEMORY];
        [self showInMemoryFrom:first last:last];
    }
}

- (void)wtBreak:(id)sender
{
    (void)sender;
    uint16_t first, last;
    if (_wtList.picked < 0 || !adamwatch_range(_dbg, _wtList.picked, &first, &last)) return;
    [self breakOnChangeFrom:first to:last];
}

/* A write breakpoint on first..last that fires when the value written
 * differs from the old one (the core's `value != old`). */
- (void)breakOnChangeFrom:(uint16_t)first to:(uint16_t)last
{
    char err[160];
    err[0] = '\0';
    if (adamdebug_breakpoint_add(_dbg, ADAMDEBUG_BP_WRITE, first, last, "value != old", err, (int)sizeof err) >= 0)
        [self message:@"Breaking when it changes" console:NO];
    else
        [self message:err[0] ? cstr(err) : @"The breakpoint was refused" console:NO];
    [self refreshNow];
}

- (void)wtRemove:(id)sender
{
    (void)sender;
    if (_wtList.picked >= 0) {
        adamwatch_remove(_wtList.picked);
        _wtList.picked = -1;
        [self refreshWatches];
    }
}

- (void)histRec:(NSButton *)sender
{
    const int enabled = sender.state == NSControlStateValueOn;
    if (sender.tag == 0) adamdebug_trace_enable(_dbg, enabled); else adamdebug_history_enable(_dbg, enabled);
    _hSeen[0] = _hSeen[1] = (uint64_t)-1;
    [self refreshNow];
}

- (void)histChanged:(id)sender
{
    (void)sender;
    _hSeen[0] = _hSeen[1] = (uint64_t)-1;
    [self refreshHist:0 force:YES];
    [self refreshHist:1 force:YES];
}

- (void)histClear:(id)sender
{
    (void)sender;
    adamdebug_trace_clear(_dbg);
    adamdebug_history_clear(_dbg);
    _hSeen[0] = _hSeen[1] = (uint64_t)-1;
    [self refreshNow];
}

- (void)symFilterChanged:(id)sender
{
    (void)sender;
    _symList.picked = -1;
    [self refreshSymbols];
}

/* ---- tabs in windows of their own --------------------------------------------------- */

- (void)popOutCurrent:(id)sender
{
    (void)sender;
    for (int slot = 0; slot < TAB_COUNT; slot++)
        if (_tabs.selectedTabViewItem == _items[slot]) {
            [self popOutTab:slot];
            return;
        }
}

- (void)popOutTab:(int)tab
{
    if (_popped[tab] || _tabs.numberOfTabViewItems <= 1) return;     /* one tab always stays */
    NSTabViewItem *item = _items[tab];
    NSView *view = item.view;
    [_tabs removeTabViewItem:item];

    NSWindow *win = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 820, 580)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable |
                            NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    win.title = [NSString stringWithFormat:@"Debugger – %s", kTabNames[tab]];
    win.releasedWhenClosed = NO;
    win.delegate = self;
    NSButton *dockBtn = [self button:@"Dock" action:@selector(dockButton:)];
    dockBtn.tag = tab;
    dockBtn.toolTip = @"Put this tab back in the debugger";
    NSStackView *root = vstack(@[dockBtn, view]);
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [view.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [view.heightAnchor constraintGreaterThanOrEqualToConstant:300].active = YES;
    win.contentView = root;
    _popped[tab] = win;
    [win center];
    [win makeKeyAndOrderFront:nil];
    [self refreshNow];
}

- (void)dockButton:(id)sender
{
    [self dockTab:(int)((NSButton *)sender).tag];
}

- (void)dockTab:(int)tab
{
    NSWindow *win = _popped[tab];
    if (!win) return;
    NSTabViewItem *item = _items[tab];
    NSView *view = item.view;
    int pos = 0;
    _popped[tab] = nil;
    [view removeFromSuperview];
    item.view = view;
    for (int slot = 0; slot < tab; slot++)   /* back where it was in the order */
        if (!_popped[slot]) pos++;
    [_tabs insertTabViewItem:item atIndex:pos];
    [_tabs selectTabViewItem:item];
    win.delegate = nil;
    [win close];
    [self refreshNow];
}

/* ---- refreshers ------------------------------------------------------------------ */

/* Everything the window shows, read back now: the status, the register strip
 * and the visible tabs. The poll compares against what was seen here. */
- (void)refreshNow
{
    adamdebug_snapshot snap;
    adamdebug_stop_snapshot(_dbg, &snap);
    _seenSerial = snap.serial;
    _seenGen = adamdebug_generation(_dbg);
    _wasStopped = snap.paused != 0;
    _liveTicks = 0;
    [self refreshStatus];
    [self refreshVisibleForce:YES];
}

/* Only the tabs on show: the others refresh when they are selected. */
- (void)refreshVisibleForce:(BOOL)force
{
    [self refreshRegs];
    /* the live poll of a running machine skips the tabs that read the memory
     * map or the code: they refresh on a stop, a command or when shown */
    const BOOL liveRead = !force && !adamdebug_is_stopped(_dbg);
    for (int slot = 0; slot < TAB_COUNT; slot++) {
        if (![self tabVisible:slot]) continue;
        if (liveRead && (slot == MT_DISASM || slot == MT_MEMORY || slot == MT_CART || slot == MT_SYMBOLS)) continue;
        switch (slot) {
        case MT_DISASM: [self refreshDisasm]; break;
        case MT_MEMORY: [self refreshMem]; break;
        case MT_BREAKS: [self refreshBps]; break;
        case MT_WATCHES: [self refreshWatches]; break;
        case MT_CART: [self refreshCart]; break;
        case MT_TRACE: [self refreshHist:0 force:force]; break;
        case MT_HISTORY: [self refreshHist:1 force:force]; break;
        case MT_VIDEO: [_video refreshForce:force]; break;
        case MT_SOUND: [self refreshSound]; break;
        case MT_SYMBOLS: [self refreshSymbols]; break;
        case MT_FUJINET: [self refreshFujinet:NO]; break;
        default: break;
        }
    }
}

- (void)refreshStatus
{
    adamdebug_snapshot snap;
    adamdebug_stop_snapshot(_dbg, &snap);
    const BOOL stopped = snap.paused != 0;
    NSString *why = stopText(&snap);
    if (why.length)
        why = [[[why substringToIndex:1] uppercaseString] stringByAppendingString:[why substringFromIndex:1]];
    _status.stringValue = stopped ? (why.length ? why : @"Stopped") : @"Running";
    _runBtn.title = stopped ? @"Run (F5)" : @"Stop (F5)";
    _runBtn.bezelColor = stopped ? accentColor() : nil;
    _runBtn.contentTintColor = stopped ? accentInk() : nil;
}

- (void)refreshRegs
{
    static int previous[NUM_REGS];
    static BOOL havePrevious;
    CpuView cpu;
    cpuView(_dbg, &cpu);
    const int vals[NUM_REGS] = {
        cpu.pairAF, cpu.pairBC, cpu.pairDE, cpu.pairHL, cpu.indexX, cpu.indexY, cpu.stackPointer, cpu.programCounter,
        cpu.pairAF2, cpu.pairBC2, cpu.pairDE2, cpu.pairHL2, cpu.vectorBase, cpu.refresh, cpu.intMode, cpu.iff1, cpu.iff2
    };
    for (int idx = 0; idx < NUM_REGS; idx++) {
        const BOOL changed = havePrevious && previous[idx] != vals[idx];
        previous[idx] = vals[idx];
        _reg[idx].textColor = changed ? accentLight() : NSColor.textColor;     /* moved since the last look */
        if (_reg[idx].currentEditor) continue;        /* never over a field being edited */
        _reg[idx].stringValue = [NSString stringWithFormat:(idx < 12 ? @"%04X" : (idx < 14 ? @"%02X" : @"%d")), vals[idx]];
    }
    havePrevious = YES;
    const int flags[NUM_FLAGS] = { cpu.sign, cpu.zero, cpu.half, cpu.parity, cpu.subtract, cpu.carry };
    for (int idx = 0; idx < NUM_FLAGS; idx++) _flag[idx].state = flags[idx] ? NSControlStateValueOn : NSControlStateValueOff;
    _regInfo.stringValue = adamdebug_is_stopped(_dbg)
        ? [NSString stringWithFormat:@"  %@%@", cpu.halted ? @"HALT  " : @"",
                                      @"edit a register and press Enter; IM is 0-2"]
        : @"  running";
}

- (void)refreshMem
{
    uint8_t data[MEM_BYTES];
    adamdebug_region map[16];
    if (_memBase > 0x10000 - MEM_BYTES) _memBase = 0x10000 - MEM_BYTES;
    if (_memBase < 0) _memBase = 0;
    memset(data, 0, sizeof data);
    adamdebug_read(_dbg, (uint16_t)_memBase, data, MEM_BYTES);
    int nmap = adamdebug_memory_map(_dbg, map, 16);
    if (nmap < 0) nmap = 0;
    NSMutableString *text = [NSMutableString stringWithString:@"      0  1  2  3  4  5  6  7   8  9  A  B  C  D  E  F\n"];
    NSMutableArray<NSValue *> *marks = [NSMutableArray array];
    char region[64], lastRegion[64];
    lastRegion[0] = '\0';
    for (int row = 0; row < MEM_ROWS; row++) {
        const int base = _memBase + row * 16;
        [text appendFormat:@"%04X  ", base];
        for (int col = 0; col < 16; col++) {
            const uint16_t addr = (uint16_t)(base + col);
            if (col == 8) [text appendString:@" "];
            if (_memHl && addr >= _memHlLo && addr <= _memHlHi)
                [marks addObject:[NSValue valueWithRange:NSMakeRange(text.length, 2)]];
            [text appendFormat:@"%02X ", data[row * 16 + col]];
        }
        [text appendString:@" "];
        for (int col = 0; col < 16; col++) {
            const uint8_t chr = data[row * 16 + col];
            [text appendFormat:@"%c", (chr >= 0x20 && chr < 0x7F) ? (char)chr : '.'];
        }
        /* label a row where the region it is in changes */
        region[0] = '\0';
        if (adammem_region_name(map, nmap, (uint16_t)base, region, (int)sizeof region) > 0 &&
            strcmp(region, lastRegion) != 0)
            [text appendFormat:@"  ; %s", region];
        snprintf(lastRegion, sizeof lastRegion, "%s", region);
        [text appendString:@"\n"];
    }
    NSMutableAttributedString *as = [[NSMutableAttributedString alloc] initWithString:text attributes:monoAttrs()];
    for (NSValue *span in marks) [as addAttributes:accentAttrs() range:span.rangeValue];
    setStyledText(_mem, as);
}

- (void)refreshDisasm
{
    static adamdebug_line lines[DISASM_WINDOW];
    int pcLine = -1;
    if (_followPc.state == NSControlStateValueOn) {
        adamdebug_cpu cpu;
        adamdebug_cpu_get(_dbg, &cpu);
        /* the PC a third of the way down */
        _disasmAddr = (uint16_t)adamdebug_row_address(_dbg, (uint16_t)cpu.pc, -(DISASM_WINDOW / 3));
    }
    int lineTotal = adamdebug_disassemble(_dbg, _disasmAddr, lines, DISASM_WINDOW, &pcLine);
    if (lineTotal < 0) lineTotal = 0;
    if (lineTotal > DISASM_WINDOW) lineTotal = DISASM_WINDOW;
    _lineCount = lineTotal;

    NSMutableString *text = [NSMutableString string];
    NSRange ranges[DISASM_WINDOW];
    for (int rowNo = 0; rowNo < lineTotal; rowNo++) {
        _lineAddr[rowNo] = lines[rowNo].address;
        const char gutter = !lines[rowNo].has_breakpoint ? ' ' : (lines[rowNo].breakpoint_enabled ? '*' : 'o');
        NSString *line = [NSString stringWithFormat:@"%c%c %04X  %-11s  %-16s %-24s%s%s", gutter,
                          lines[rowNo].is_pc ? '>' : ' ', lines[rowNo].address, lines[rowNo].bytes, lines[rowNo].label,
                          lines[rowNo].disasm, lines[rowNo].comment[0] ? " ; " : "", lines[rowNo].comment];
        ranges[rowNo] = NSMakeRange(text.length, line.length);
        [text appendString:line];
        [text appendString:@"\n"];
    }
    if (lineTotal == 0) [text appendString:@"(no disassembly: no machine is running)\n"];

    NSMutableAttributedString *as = [[NSMutableAttributedString alloc] initWithString:text attributes:monoAttrs()];
    for (int rowNo = 0; rowNo < lineTotal; rowNo++) {
        /* the gutter: a breakpoint red, a disabled one grey */
        if (lines[rowNo].has_breakpoint)
            [as addAttribute:NSForegroundColorAttributeName
                       value:lines[rowNo].breakpoint_enabled ? NSColor.systemRedColor : NSColor.systemGrayColor
                       range:NSMakeRange(ranges[rowNo].location, 1)];
        /* the selected line underlined on a selection fill */
        if ((int)lines[rowNo].address == _selected)
            [as addAttributes:@{ NSUnderlineStyleAttributeName: @(NSUnderlineStyleSingle),
                                 NSBackgroundColorAttributeName: NSColor.selectedTextBackgroundColor }
                        range:ranges[rowNo]];
    }
    /* the PC's line in the accent colour, last so its colours win */
    for (int rowNo = 0; rowNo < lineTotal; rowNo++) {
        if (!lines[rowNo].is_pc) continue;
        [as addAttributes:accentAttrs() range:ranges[rowNo]];
        if ((int)lines[rowNo].address == _selected)
            [as addAttribute:NSUnderlineStyleAttributeName value:@(NSUnderlineStyleSingle) range:ranges[rowNo]];
    }
    [_disasm.textStorage setAttributedString:as];
    NSScrollView *scroll = _disasm.enclosingScrollView;
    [scroll.contentView scrollToPoint:NSMakePoint(scroll.contentView.bounds.origin.x, 0)];
    [scroll reflectScrolledClipView:scroll.contentView];
}

- (void)refreshBps
{
    static adamdebug_breakpoint bps[MAX_BPS];
    int count = adamdebug_breakpoint_list(_dbg, bps, MAX_BPS);
    if (count < 0) count = 0;
    if (count > MAX_BPS) count = MAX_BPS;

    /* why the machine last stopped, worded by the core */
    {
        adamdebug_snapshot snap;
        adamdebug_stop_snapshot(_dbg, &snap);
        NSString *why = stopText(&snap);
        _bpStop.stringValue = !snap.paused ? @"Running." : why.length ? [@"Stopped: " stringByAppendingString:why] : @"Stopped.";
    }

    /* the rows are rebuilt only when something in them changed (a refresh
     * must not eat a click) */
    NSMutableString *sig = [NSMutableString string];
    for (int idx = 0; idx < count; idx++)
        [sig appendFormat:@"%d %d %d %d %d %u %s\n", bps[idx].id, bps[idx].type, bps[idx].start, bps[idx].end,
                          bps[idx].enabled, bps[idx].hits, bps[idx].condition];
    if ([sig isEqualToString:_bpSig]) return;
    _bpSig = sig;

    for (NSView *rowView in [_bpList.arrangedSubviews copy]) [_bpList removeView:rowView];
    if (count == 0) [_bpList addArrangedSubview:[self label:@"No breakpoints"]];
    for (int idx = 0; idx < count; idx++) {
        static const struct { int bit; const char *name; } kinds[5] = {
            { ADAMDEBUG_BP_EXEC, "exec" }, { ADAMDEBUG_BP_READ, "read" }, { ADAMDEBUG_BP_WRITE, "write" },
            { ADAMDEBUG_BP_IN, "in" }, { ADAMDEBUG_BP_OUT, "out" },
        };
        NSMutableString *type = [NSMutableString string];
        if (bps[idx].type == ADAMDEBUG_BP_ACCESS) {
            [type appendString:@"access"];
        } else {
            for (int item = 0; item < 5; item++)
                if (bps[idx].type & kinds[item].bit) {
                    if (type.length) [type appendString:@"/"];
                    [type appendFormat:@"%s", kinds[item].name];
                }
        }
        const BOOL port = (bps[idx].type & (ADAMDEBUG_BP_IN | ADAMDEBUG_BP_OUT)) != 0;
        NSMutableString *range = [NSMutableString stringWithFormat:(port ? @"port $%02X" : @"$%04X"), bps[idx].start];
        if (bps[idx].end != bps[idx].start) [range appendFormat:(port ? @"-$%02X" : @"-$%04X"), bps[idx].end];
        char label[64];
        label[0] = '\0';
        if (!port && adamdebug_address_label(_dbg, bps[idx].start, label, sizeof label) > 0 && label[0])
            [range appendFormat:@" %s", label];

        NSButton *box = [NSButton checkboxWithTitle:[NSString stringWithFormat:@"#%d", bps[idx].id]
                                             target:self action:@selector(bpEnabled:)];
        box.state = bps[idx].enabled ? NSControlStateValueOn : NSControlStateValueOff;
        box.tag = bps[idx].id;
        box.font = monoFont();
        [box.widthAnchor constraintEqualToConstant:70].active = YES;
        NSButton *remove = [self button:@"Remove" action:@selector(bpRemove:)];
        remove.tag = bps[idx].id;
        [remove.widthAnchor constraintEqualToConstant:70].active = YES;
        NSButton *condition = [self button:@"Set Cond." action:@selector(bpSetCondition:)];
        condition.tag = bps[idx].id;
        condition.toolTip = @"Replace this breakpoint's condition with the text in the Condition field (empty removes it)";
        [condition.widthAnchor constraintEqualToConstant:80].active = YES;
        NSStackView *row = hstack(@[box, bpCell(type, 60, NO), bpCell(range, 190, NO),
                                    bpCell(cstr(bps[idx].condition), 0, NO),
                                    bpCell([NSString stringWithFormat:@"%u", bps[idx].hits], 60, NO), condition, remove]);
        row.distribution = NSStackViewDistributionFill;
        [_bpList addArrangedSubview:row];
        [row.widthAnchor constraintEqualToAnchor:_bpList.widthAnchor].active = YES;
    }
}

- (void)refreshWatches
{
    const int count = adamwatch_count();
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    for (int idx = 0; idx < count; idx++) {
        char line[256];
        line[0] = '\0';
        adamwatch_format(_dbg, idx, line, (int)sizeof line);
        [lines addObject:cstr(line)];
    }
    if (count == 0) [lines addObject:@"No watches. Type an address, label, expression or lo-hi range above."];
    if (_wtList.picked >= count) _wtList.picked = -1;
    setPickText(_wtList, lines);
}

/* The cartridge, the controllers, the VDP's host side, the CPU's interrupt
 * lines, the memory control ports, the SGM and the memory map: what the
 * machine answers where. */
- (void)refreshCart
{
    static adamdebug_io ports;
    static adamdebug_cart cart;
    static char text[8192];
    static char summary[256];
    adamdebug_region map[16];
    NSMutableString *out = [NSMutableString string];

    adamdebug_cart_info(_dbg, &cart);
    summary[0] = '\0';
    adamdebug_cart_summary(_dbg, summary, (int)sizeof summary);
    [out appendString:@"Machine\n"];
    [out appendFormat:@"  %s\n", summary];
    [out appendFormat:@"  Memory configuration: %s\n", cart.config];
    if (cart.cartridge) {
        static const char *const headers[4] = { "nothing readable", "$AA55 (title screen first)",
                                                "$55AA (straight to the game)", "no cartridge header" };
        [out appendFormat:@"  Cartridge: %s, %ld bytes, %s, window %s at $8000\n", cart.cart_path,
                          cart.cart_size, headers[cart.cart_header & 3], cart.cart_visible ? "mapped" : "not mapped"];
    } else {
        [out appendString:@"  Cartridge: none\n"];
    }
    [out appendFormat:@"  FujiNet %s%s%s\n", cart.fujinet_runtime ? "on" : "off",
                      cart.fujinet_runtime ? (cart.fujinet_link ? ", AdamNet link up  " : ", AdamNet link down  ") : "",
                      cart.fujinet_runtime ? cart.boip : ""];
    [out appendFormat:@"  Super Game Module %s\n", cart.sgm ? "fitted" : "absent"];

    if (adamdebug_io_get(_dbg, &ports) && ports.valid) {
        [out appendFormat:@"\nControllers (strobe: %s half)\n", ports.joy_mode ? "joystick" : "keypad"];
        for (int unit = 0; unit < 2; unit++)
            [out appendFormat:@"  Pad %d  word %04X  joystick byte %02X  keypad byte %02X  keypad %s%s%s%s%s%s%s\n", unit + 1,
                              ports.pad[unit].raw, ports.pad[unit].joystick_byte, ports.pad[unit].keypad_byte,
                              ports.pad[unit].keypad >= 0 ? ports.pad[unit].keypad_name : "-",
                              ports.pad[unit].up ? "  up" : "", ports.pad[unit].right ? "  right" : "",
                              ports.pad[unit].down ? "  down" : "", ports.pad[unit].left ? "  left" : "",
                              ports.pad[unit].fire_left ? "  fire L" : "", ports.pad[unit].fire_right ? "  fire R" : ""];
        [out appendFormat:@"\nVDP host side  R0-R7 %02X %02X %02X %02X %02X %02X %02X %02X  status %02X  address %04X\n",
                          ports.vdp_regs[0], ports.vdp_regs[1], ports.vdp_regs[2], ports.vdp_regs[3], ports.vdp_regs[4],
                          ports.vdp_regs[5], ports.vdp_regs[6], ports.vdp_regs[7], ports.vdp_status, ports.vdp_addr];
        [out appendFormat:@"  control latch %02X (%s)  read-ahead %02X  INT line %d\n", ports.vdp_latch,
                          ports.vdp_latched ? "waiting for the second byte" : "idle", ports.vdp_read_ahead, ports.vdp_int_line];
        [out appendFormat:@"  beam: line %d, T-state %d   frames %llu\n", ports.beam_line, ports.beam_tstate,
                          (unsigned long long)ports.frames];
        [out appendFormat:@"\nCPU  queue %02X  EI pending %d  IRQ pending %d  NMI pending %d  NMIs taken %llu\n", ports.cpu_q,
                          ports.ei_pending, ports.irq_pending, ports.nmi_pending, (unsigned long long)ports.nmi_count];
        [out appendFormat:@"Memory control %02X (lower %s, upper %s)  network control %02X (EOS ROM %s, AdamNet %s)  %s\n",
                          ports.mem_ctrl, ports.lower_name, ports.upper_name, ports.net_ctrl,
                          ports.eos_rom ? "visible" : "hidden", ports.net_reset ? "held in reset" : "running",
                          ports.game_mode ? "game mode" : "not in game mode"];
        if (ports.sgm) {
            [out appendFormat:@"SGM  RAM %s  BIOS %s  AY address %02X  registers", ports.sgm_ram_en ? "on" : "off",
                              ports.sgm_bios_off ? "off" : "on", ports.ay_addr];
            for (int idx = 0; idx < 16; idx++) [out appendFormat:@" %02X", ports.ay_regs[idx]];
            [out appendString:@"\n"];
        }
    } else {
        [out appendString:@"\nNo machine is running.\n"];
    }

    text[0] = '\0';
    adamdebug_io_state(_dbg, text, (int)sizeof text);
    if (text[0]) [out appendFormat:@"\nI/O state\n%@\n", cstr(text)];

    [out appendString:@"\nMemory map\n"];
    const int count = adamdebug_memory_map(_dbg, map, 16);
    if (count <= 0) [out appendString:@"  No machine is running.\n"];
    for (int idx = 0; idx < count; idx++)
        [out appendFormat:@"  %04X-%04X  %c%c  %s\n", map[idx].low, map[idx].high, map[idx].readable ? 'r' : '-',
                          map[idx].writable ? 'w' : '-', map[idx].name];
    setText(_cartText, out, NO);
}

/* Trace (live) and History (when stopped), from the one ring. */
- (void)refreshHist:(int)idx force:(BOOL)force
{
    static adamdebug_trace buf[HIST_BUF];
    const BOOL live = idx == 0;
    const BOOL stopped = adamdebug_is_stopped(_dbg) != 0;
    const BOOL traceOn = adamdebug_trace_enabled(_dbg) != 0, histOn = adamdebug_history_enabled(_dbg) != 0;
    const BOOL on = live ? traceOn : histOn;
    const uint64_t total = live ? adamdebug_trace_total(_dbg) : adamdebug_history_count(_dbg);
    uint16_t first = 0, last = 0xFFFF;
    int filter = ADAMDEBUG_TRACE_ALL;

    for (int pair = 0; pair < 2; pair++) {      /* the Record buttons follow the engine */
        const BOOL pairOn = pair == 0 ? traceOn : histOn;
        _hRec[pair].state = pairOn ? NSControlStateValueOn : NSControlStateValueOff;
        _hRec[pair].bezelColor = pairOn ? accentColor() : nil;
        _hRec[pair].contentTintColor = pairOn ? accentInk() : nil;
    }
    if (!force && total == _hSeen[idx] && (live || stopped)) return;
    _hSeen[idx] = total;
    if (!live && !stopped) {
        _hNote[idx].stringValue = @"Stop the machine (F5) to read its history.";
        return;
    }
    switch (_hFilter[idx].indexOfSelectedItem) {
    case 1: filter = ADAMDEBUG_TRACE_BRANCHES; break;
    case 2:
        if (adamsym_parse_range(_dbg, _hRange[idx].stringValue.UTF8String, &first, &last)) filter = ADAMDEBUG_TRACE_RANGE;
        break;
    }
    /* the newest matches, scanning back from the newest; shown oldest first */
    int hit[HIST_SHOWN], shown = 0, count;
    if (live) {
        count = adamdebug_trace_read(_dbg, buf, HIST_BUF);                  /* newest first */
        for (int rec = 0; rec < count && shown < HIST_SHOWN; rec++)
            if (adamdebug_trace_match(&buf[rec], filter, first, last)) hit[shown++] = rec;
    } else {
        count = adamdebug_history_copy(_dbg, buf, HIST_BUF);                /* oldest first */
        for (int rec = count - 1; rec >= 0 && shown < HIST_SHOWN; rec--)
            if (adamdebug_history_match(&buf[rec], filter, first, last)) hit[shown++] = rec;
    }
    NSMutableString *slot = [NSMutableString string];
    if (!on && count == 0) [slot appendString:@"Recording is off. Press Record, then let the machine run."];
    for (int pair = shown - 1; pair >= 0; pair--) {
        char line[256];
        line[0] = '\0';
        if (live) adamdebug_trace_format(&buf[hit[pair]], line, (int)sizeof line);
        else adamdebug_history_format(&buf[hit[pair]], line, (int)sizeof line);
        [slot appendFormat:@"%@\n", cstr(line)];
    }
    setText(_hText[idx], slot, YES);
    _hNote[idx].stringValue = [NSString stringWithFormat:@"%@%llu instructions recorded, last %d shown",
                                                          on ? @"Recording. " : @"", (unsigned long long)total, shown];
}

/* The SN76489, and the AY-3-8910 of a Super Game Module when one is fitted.
 * Only a stopped machine is read: the mirrors are the emulator thread's. */
- (void)refreshSound
{
    static adamdebug_sound snd;
    if (!adamdebug_is_stopped(_dbg)) {
        setText(_soundText, @"Stop the machine (F5) to read the sound chips.", NO);
        return;
    }
    if (!adamdebug_sound_get(_dbg, &snd)) {
        setText(_soundText, @"The sound chips could not be read.", NO);
        return;
    }
    NSMutableString *slot = [NSMutableString string];
    if (snd.sn.present) {
        const adamdebug_sn *chip = &snd.sn;
        [slot appendFormat:@"SN76489  latched register %d\n", chip->latched_reg];
        for (int voice = 0; voice < 3; voice++)
            [slot appendFormat:@"  Tone %d   period %4d  %8.1f Hz  attenuation %2d  level %2d\n", voice + 1, chip->period[voice],
                               chip->freq[voice], chip->atten[voice], chip->level[voice]];
        [slot appendFormat:@"  Noise    %s, %s  %8.1f Hz  attenuation %2d  level %2d\n",
                           chip->noise_white ? "white" : "periodic",
                           chip->noise_rate == 3 ? "follows tone 3" : (chip->noise_rate == 2 ? "rate 2" : (chip->noise_rate == 1 ? "rate 1" : "rate 0")),
                           chip->noise_hz, chip->atten[3], chip->level[3]];
        [slot appendFormat:@"  Noise control %02X\n\n", chip->noise_ctrl];
    } else {
        [slot appendString:@"SN76489: not present\n\n"];
    }
    if (snd.ay.present) {
        const adamdebug_ay *chip = &snd.ay;
        [slot appendFormat:@"AY-3-8910 (Super Game Module)  latched register %d\n  Registers  ", chip->addr];
        for (int idx = 0; idx < 16; idx++) [slot appendFormat:@"%02X ", chip->reg[idx]];
        [slot appendString:@"\n"];
        for (int voice = 0; voice < 3; voice++)
            [slot appendFormat:@"  Voice %c  period %4d  %8.1f Hz  tone %@  noise %@  level %2d%@\n", 'A' + voice,
                               chip->period[voice], chip->freq[voice], chip->tone_on[voice] ? @"on " : @"off",
                               chip->noise_on[voice] ? @"on " : @"off", chip->level[voice],
                               chip->envelope[voice] ? @" (envelope)" : @""];
        [slot appendFormat:@"  Noise      period %d\n", chip->noise_period];
        [slot appendFormat:@"  Envelope   period %d  shape %X\n", chip->env_period, chip->env_shape];
    } else {
        [slot appendString:@"AY-3-8910: no Super Game Module fitted\n"];
    }
    setText(_soundText, slot, NO);
}

/* The list is rebuilt only when the symbol count or the filter changed. */
- (void)refreshSymbols
{
    NSString *filter = _symFilter.stringValue ?: @"";
    const int total = adamdebug_symbols_count(_dbg);
    NSString *sig = [NSString stringWithFormat:@"%d|%d|%@", total, _symList.picked, filter];
    if ([sig isEqualToString:_symSig]) return;
    _symSig = sig;

    int *index = total > 0 ? malloc((size_t)total * sizeof *index) : NULL;
    const int shown = index ? adamdebug_symbols_filter(_dbg, filter.UTF8String, index, total) : 0;
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    [_symAddrs removeAllObjects];
    for (int idx = 0; idx < shown; idx++) {
        uint16_t addr;
        char name[96];
        name[0] = '\0';
        if (!adamdebug_symbols_get(_dbg, index[idx], &addr, name, (int)sizeof name)) continue;
        [lines addObject:[NSString stringWithFormat:@"%04X  %s", addr, name]];
        [_symAddrs addObject:@(addr)];
    }
    free(index);
    if (lines.count == 0) [lines addObject:@"(no symbols match)"];
    if (_symList.picked >= (int)_symAddrs.count) _symList.picked = -1;
    setPickText(_symList, lines);
    _symCount.stringValue = [NSString stringWithFormat:@"%d symbol%s%s", (int)_symAddrs.count, _symAddrs.count == 1 ? "" : "s",
                                                       filter.length > 0 ? " (filtered)" : ""];
}

/* The log is rebuilt only when it changed: a new packet, or a repeat folded
 * into the newest. Built as UTF-8 first: %s in an NSString format is not
 * UTF-8. */
- (void)refreshFujinet:(BOOL)force
{
    static adam_fuji_packet packets[TRACE_MAX];
    static char logText[TRACE_MAX * 336 + 128]; /* an entry is at most ~330 */
    static char state[2048];
    int total = adamdebug_fujinet_trace(_dbg, packets, TRACE_MAX);
    size_t len = 0;
    if (total < 0) total = 0;

    if (adamsession_fujinet_external(_session))
        _fujiNote.stringValue = [NSString
            stringWithFormat:@"External FujiNet %s, AdamNet link %s.",
                             adamsession_boip_endpoint(_session),
                             adamsession_fujinet_link(_session)
                                 ? "connected" : "not connected"];
    else if (!adamsession_fujinet_running(_session))
        _fujiNote.stringValue = @"FujiNet is off (disabled, or its runtime did not start).";
    else
        _fujiNote.stringValue = adamsession_fujinet_link(_session)
                                    ? @"FujiNet is on, AdamNet link up."
                                    : @"FujiNet is on, AdamNet link down.";
    state[0] = '\0';
    adamdebug_fujinet_state(_dbg, state, (int)sizeof(state));
    setText(_fujiState, cstr(state), NO);

    if (!force && total == _fujiCount && (total == 0 || packets[total - 1].serial == _fujiLast))
        return;
    const BOOL follow = force || _fujiCount < 0 || atEnd(_fujiText);
    _fujiCount = total;
    _fujiLast = total > 0 ? packets[total - 1].serial : 0;
    logText[0] = '\0';
    for (int entry = 0; entry < total; entry++) {
        len += (size_t)snprintf(logText + len, sizeof(logText) - len, "%s%s", entry ? "\n" : "", packets[entry].text);
        if (packets[entry].repeat > 1)
            len += (size_t)snprintf(logText + len, sizeof(logText) - len, " \xc3\x97%u", packets[entry].repeat);
        len += (size_t)snprintf(logText + len, sizeof(logText) - len, "\n     ");
        for (int bytePos = 0; bytePos < packets[entry].nbytes && bytePos < (int)sizeof(packets[entry].data); bytePos++)
            len += (size_t)snprintf(logText + len, sizeof(logText) - len, "%02X ", packets[entry].data[bytePos]);
        if (packets[entry].nbytes > (int)sizeof(packets[entry].data))
            len += (size_t)snprintf(logText + len, sizeof(logText) - len, "\xe2\x80\xa6");
    }
    if (total == 0)
        snprintf(logText, sizeof(logText), "(no FujiBus packets since the trace was cleared)");
    setText(_fujiText, cstr(logText), follow);
}

@end

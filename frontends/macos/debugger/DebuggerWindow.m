/*
 * Debugger window (AppKit): disassembly with click-to-toggle breakpoints,
 * editable registers, memory view, breakpoints, instruction trace, and the
 * VDP visualizers, over the shared adamdebug engine -- the same layout as
 * the GTK and Qt debuggers. Stop events arrive on the emulator thread and
 * are marshaled here with dispatch_async onto the main queue.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"

#include "adamdebug.h"

#define DISASM_LINES 40
#define MEM_ROWS 16

static DebuggerWindow *g_debugger;

/* Disassembly text view: a click toggles the breakpoint on the clicked
 * line (the address lives in columns 2-5 of each row, as in the other
 * frontends). */
@interface DasmTextView : NSTextView
@property(nonatomic, copy) void (^onToggleAddr)(uint16_t addr);
@end

@implementation DasmTextView
- (void)mouseDown:(NSEvent *)event
{
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger idx = [self characterIndexForInsertionAtPoint:p];
    NSString *text = self.string;
    if (idx > text.length)
        idx = text.length;
    NSUInteger start = [text lineRangeForRange:NSMakeRange(idx, 0)].location;
    NSString *line = [text substringFromIndex:start];
    if (line.length >= 6) {
        unsigned addr = 0;
        NSScanner *scan =
            [NSScanner scannerWithString:[line substringWithRange:
                                                   NSMakeRange(2, 4)]];
        if ([scan scanHexInt:&addr] && addr <= 0xFFFF && self.onToggleAddr)
            self.onToggleAddr((uint16_t)addr);
    }
}
@end

@interface DebuggerWindow () <NSWindowDelegate>
- (instancetype)initWithSession:(adamsession *)session;
- (void)onStopped:(adamdebug_stop_reason)reason pc:(uint16_t)pc;
- (void)refreshDisasm;
- (void)refreshBps;
@end

@implementation DebuggerWindow {
    adamsession *_session;
    adamdebug *_dbg;
    NSWindow *_window;

    NSButton *_pauseBtn;
    NSTextField *_status;
    DasmTextView *_disasm;
    uint16_t _disasmBase;
    BOOL _followPc;

    NSTextField *_regField[8];
    NSTextField *_flags;

    NSTextField *_memAddr;
    NSTextView *_memView;
    uint16_t _memBase;

    NSTextField *_bpEntry;
    NSTextView *_bpView;
    NSTextView *_traceView;

    NSImageView *_nt, *_pat, *_spr, *_pal;
    NSPopUpButton *_patBank;
    NSTextView *_spriteInfo;

    NSTimer *_tick;
}

static const char *const kRegNames[8] = {"AF", "BC", "DE", "HL",
                                         "IX", "IY", "SP", "PC"};

static uint16_t reg_get(const adamcore_z80_regs *r, int i)
{
    switch (i) {
    case 0: return (uint16_t)((r->a << 8) | r->f);
    case 1: return (uint16_t)((r->b << 8) | r->c);
    case 2: return (uint16_t)((r->d << 8) | r->e);
    case 3: return (uint16_t)((r->h << 8) | r->l);
    case 4: return r->ix;
    case 5: return r->iy;
    case 6: return r->sp;
    default: return r->pc;
    }
}

static void reg_set(adamcore_z80_regs *r, int i, uint16_t v)
{
    switch (i) {
    case 0: r->a = (uint8_t)(v >> 8); r->f = (uint8_t)v; break;
    case 1: r->b = (uint8_t)(v >> 8); r->c = (uint8_t)v; break;
    case 2: r->d = (uint8_t)(v >> 8); r->e = (uint8_t)v; break;
    case 3: r->h = (uint8_t)(v >> 8); r->l = (uint8_t)v; break;
    case 4: r->ix = v; break;
    case 5: r->iy = v; break;
    case 6: r->sp = v; break;
    default: r->pc = v; break;
    }
}

static NSImage *imageFromRGBA(const uint8_t *rgba, int w, int h)
{
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider =
        CGDataProviderCreateWithData(NULL, rgba, (size_t)w * h * 4, NULL);
    CGImageRef cg = CGImageCreate(
        w, h, 8, 32, (size_t)w * 4, space,
        kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault, provider,
        NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(space);
    NSImage *img = [[NSImage alloc] initWithCGImage:cg
                                               size:NSMakeSize(w, h)];
    CGImageRelease(cg);
    return img;
}

static void stop_trampoline(void *ud, adamdebug_stop_reason reason,
                            uint16_t pc)
{
    DebuggerWindow *w = (__bridge DebuggerWindow *)ud;
    dispatch_async(dispatch_get_main_queue(), ^{
        [w onStopped:reason pc:pc];
    });
}

+ (void)showForSession:(adamsession *)session
{
    if (!g_debugger)
        g_debugger = [[DebuggerWindow alloc] initWithSession:session];
    [g_debugger->_window makeKeyAndOrderFront:nil];
}

- (instancetype)initWithSession:(adamsession *)session
{
    self = [super init];
    if (!self)
        return nil;
    _session = session;
    _dbg = adamsession_debugger(session);
    _followPc = YES;
    _memBase = 0xFC30;
    [self buildWindow];
    adamdebug_set_stop_callback(_dbg, stop_trampoline,
                                (__bridge void *)self);
    __weak DebuggerWindow *weakSelf = self;
    _tick = [NSTimer scheduledTimerWithTimeInterval:0.1
                                            repeats:YES
                                              block:^(NSTimer *t) {
                                                (void)t;
                                                [weakSelf onTick];
                                              }];
    [self refreshAll];
    return self;
}

- (void)onTick
{
    if (!adamdebug_is_paused(_dbg)) {
        [self refreshVdp];
        [self refreshRegs];
    }
}

/* ---- construction helpers ------------------------------------------------ */

static NSTextView *monoView(NSScrollView **scrollOut)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    NSTextView *view =
        [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    view.editable = NO;
    view.richText = NO;
    view.font = [NSFont monospacedSystemFontOfSize:11
                                            weight:NSFontWeightRegular];
    view.autoresizingMask = NSViewWidthSizable;
    scroll.documentView = view;
    *scrollOut = scroll;
    return view;
}

- (NSButton *)button:(NSString *)title action:(SEL)sel
{
    NSButton *b = [NSButton buttonWithTitle:title target:self action:sel];
    return b;
}

- (NSTextField *)label:(NSString *)text
{
    NSTextField *l = [NSTextField labelWithString:text];
    return l;
}

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

- (void)buildWindow
{
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1180, 820)
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable |
                            NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"ADAM Debugger";
    _window.releasedWhenClosed = NO;

    /* Toolbar row */
    _pauseBtn = [self button:@"Pause (F5)" action:@selector(pauseContinue:)];
    NSButton *stepIn = [self button:@"Step Into (F7)"
                             action:@selector(stepInto:)];
    NSButton *stepOver = [self button:@"Step Over (F8)"
                               action:@selector(stepOver:)];
    NSButton *stepOut = [self button:@"Step Out (⇧F8)"
                              action:@selector(stepOut:)];
    _pauseBtn.keyEquivalent =
        [NSString stringWithFormat:@"%C", (unichar)NSF5FunctionKey];
    stepIn.keyEquivalent =
        [NSString stringWithFormat:@"%C", (unichar)NSF7FunctionKey];
    stepOver.keyEquivalent =
        [NSString stringWithFormat:@"%C", (unichar)NSF8FunctionKey];

    NSTextField *gotoField = [[NSTextField alloc] init];
    gotoField.placeholderString = @"Go to addr / symbol";
    gotoField.target = self;
    gotoField.action = @selector(gotoAddr:);
    [gotoField.widthAnchor constraintEqualToConstant:170].active = YES;

    NSButton *trace = [[NSButton alloc] init];
    [trace setButtonType:NSButtonTypeSwitch];
    trace.title = @"Trace";
    trace.target = self;
    trace.action = @selector(toggleTrace:);

    _status = [self label:@"Running"];
    _status.alignment = NSTextAlignmentRight;

    NSStackView *toolbar = [NSStackView stackViewWithViews:@[
        _pauseBtn, stepIn, stepOver, stepOut, gotoField, trace, _status
    ]];
    toolbar.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    toolbar.spacing = 8;

    /* CPU tab: disasm | (registers, breakpoints, memory) */
    NSScrollView *dasmScroll = [[NSScrollView alloc] init];
    dasmScroll.hasVerticalScroller = YES;
    _disasm = [[DasmTextView alloc] initWithFrame:NSMakeRect(0, 0, 500, 600)];
    _disasm.editable = NO;
    _disasm.richText = NO;
    _disasm.font = [NSFont monospacedSystemFontOfSize:11
                                               weight:NSFontWeightRegular];
    _disasm.autoresizingMask = NSViewWidthSizable;
    __weak DebuggerWindow *weakSelf = self;
    _disasm.onToggleAddr = ^(uint16_t addr) {
        DebuggerWindow *s = weakSelf;
        if (!s)
            return;
        adamdebug_bp_toggle(s->_dbg, addr);
        [s refreshDisasm];
        [s refreshBps];
    };
    dasmScroll.documentView = _disasm;

    NSMutableArray *gridRows = [NSMutableArray array];
    for (int row = 0; row < 2; row++) {
        NSMutableArray *cells = [NSMutableArray array];
        for (int col = 0; col < 4; col++) {
            int i = row * 4 + col;
            [cells addObject:[self label:@(kRegNames[i])]];
            NSTextField *f = [[NSTextField alloc] init];
            f.tag = i;
            f.target = self;
            f.action = @selector(applyRegister:);
            [f.widthAnchor constraintEqualToConstant:56].active = YES;
            _regField[i] = f;
            [cells addObject:f];
        }
        [gridRows addObject:cells];
    }
    NSGridView *regGrid = [NSGridView gridViewWithViews:gridRows];
    _flags = [self label:@""];

    _bpEntry = [[NSTextField alloc] init];
    _bpEntry.placeholderString = @"Add: addr or symbol";
    _bpEntry.target = self;
    _bpEntry.action = @selector(addBreakpoint:);
    NSButton *bpClear = [self button:@"Clear all"
                              action:@selector(clearBreakpoints:)];
    NSStackView *bpRow = [NSStackView
        stackViewWithViews:@[ _bpEntry, bpClear ]];
    bpRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    NSScrollView *bpScroll;
    _bpView = monoView(&bpScroll);
    [bpScroll.heightAnchor constraintEqualToConstant:100].active = YES;

    _memAddr = [[NSTextField alloc] init];
    _memAddr.placeholderString = @"Memory addr / symbol";
    _memAddr.target = self;
    _memAddr.action = @selector(gotoMem:);
    NSScrollView *memScroll;
    _memView = monoView(&memScroll);

    NSStackView *side = [NSStackView stackViewWithViews:@[
        [self label:@"Registers (Enter applies while paused)"], regGrid,
        _flags, [self label:@"Breakpoints"], bpRow, bpScroll,
        [self label:@"Memory"], _memAddr, memScroll
    ]];
    side.orientation = NSUserInterfaceLayoutOrientationVertical;
    side.alignment = NSLayoutAttributeLeading;
    side.spacing = 6;

    NSSplitView *cpuSplit = [[NSSplitView alloc] init];
    cpuSplit.vertical = YES;
    [cpuSplit addArrangedSubview:dasmScroll];
    [cpuSplit addArrangedSubview:side];

    /* VDP tab */
    _nt = [self pixelView:512 height:384];
    _pat = [self pixelView:512 height:128];
    _spr = [self pixelView:256 height:128];
    _pal = [self pixelView:256 height:16];
    _patBank = [[NSPopUpButton alloc] init];
    [_patBank addItemsWithTitles:@[ @"Bank 0", @"Bank 1", @"Bank 2" ]];
    NSScrollView *spriteScroll;
    _spriteInfo = monoView(&spriteScroll);
    [spriteScroll.widthAnchor constraintEqualToConstant:320].active = YES;

    NSStackView *vdpLeft = [NSStackView stackViewWithViews:@[
        [self label:@"Nametable"], _nt, [self label:@"Palette"], _pal
    ]];
    vdpLeft.orientation = NSUserInterfaceLayoutOrientationVertical;
    vdpLeft.alignment = NSLayoutAttributeLeading;
    NSStackView *vdpMid = [NSStackView stackViewWithViews:@[
        [self label:@"Patterns"], _patBank, _pat, [self label:@"Sprites"],
        _spr
    ]];
    vdpMid.orientation = NSUserInterfaceLayoutOrientationVertical;
    vdpMid.alignment = NSLayoutAttributeLeading;
    NSStackView *vdp = [NSStackView
        stackViewWithViews:@[ vdpLeft, vdpMid, spriteScroll ]];
    vdp.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    vdp.alignment = NSLayoutAttributeTop;
    vdp.spacing = 12;

    /* Trace tab */
    NSScrollView *traceScroll;
    _traceView = monoView(&traceScroll);

    NSTabView *tabs = [[NSTabView alloc] init];
    NSTabViewItem *cpuItem = [NSTabViewItem tabViewItemWithViewController:nil];
    cpuItem.label = @"CPU";
    cpuItem.view = cpuSplit;
    [tabs addTabViewItem:cpuItem];
    NSTabViewItem *vdpItem = [NSTabViewItem tabViewItemWithViewController:nil];
    vdpItem.label = @"VDP";
    vdpItem.view = vdp;
    [tabs addTabViewItem:vdpItem];
    NSTabViewItem *traceItem =
        [NSTabViewItem tabViewItemWithViewController:nil];
    traceItem.label = @"Trace";
    traceItem.view = traceScroll;
    [tabs addTabViewItem:traceItem];

    NSStackView *root = [NSStackView stackViewWithViews:@[ toolbar, tabs ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [toolbar.widthAnchor constraintEqualToAnchor:root.widthAnchor
                                        constant:-16].active = YES;
    [tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor
                                     constant:-16].active = YES;

    _window.contentView = root;
    [_window center];
}

/* ---- helpers -------------------------------------------------------------- */

- (BOOL)parseAddr:(NSString *)text into:(uint16_t *)out
{
    NSString *t = [text stringByTrimmingCharactersInSet:
                            [NSCharacterSet whitespaceCharacterSet]];
    if ([t hasPrefix:@"$"])
        t = [t substringFromIndex:1];
    unsigned v = 0;
    NSScanner *scan = [NSScanner scannerWithString:t];
    if ([scan scanHexInt:&v] && scan.atEnd && v <= 0xFFFF) {
        *out = (uint16_t)v;
        return YES;
    }
    uint16_t addr;
    if (adamdebug_symbol_find(_dbg, text.UTF8String, &addr)) {
        *out = addr;
        return YES;
    }
    return NO;
}

/* ---- actions -------------------------------------------------------------- */

- (void)pauseContinue:(id)sender
{
    (void)sender;
    if (adamdebug_is_paused(_dbg)) {
        adamdebug_resume(_dbg);
        _status.stringValue = @"Running";
        [self refreshAll];
    } else {
        adamdebug_pause(_dbg);
    }
}

- (void)stepInto:(id)sender
{
    (void)sender;
    adamdebug_step_into(_dbg);
}

- (void)stepOver:(id)sender
{
    (void)sender;
    adamdebug_step_over(_dbg);
}

- (void)stepOut:(id)sender
{
    (void)sender;
    adamdebug_step_out(_dbg);
}

- (void)gotoAddr:(NSTextField *)sender
{
    uint16_t addr;
    if ([self parseAddr:sender.stringValue into:&addr]) {
        _followPc = NO;
        _disasmBase = addr;
        [self refreshDisasm];
    }
}

- (void)gotoMem:(NSTextField *)sender
{
    uint16_t addr;
    if ([self parseAddr:sender.stringValue into:&addr]) {
        _memBase = addr;
        [self refreshMem];
    }
}

- (void)addBreakpoint:(NSTextField *)sender
{
    uint16_t addr;
    if ([self parseAddr:sender.stringValue into:&addr]) {
        adamdebug_bp_set(_dbg, addr);
        sender.stringValue = @"";
        [self refreshBps];
        [self refreshDisasm];
    }
}

- (void)clearBreakpoints:(id)sender
{
    (void)sender;
    adamdebug_bp_clear_all(_dbg);
    [self refreshBps];
    [self refreshDisasm];
}

- (void)toggleTrace:(NSButton *)sender
{
    adamdebug_trace_enable(_dbg, sender.state == NSControlStateValueOn);
}

- (void)applyRegister:(NSTextField *)sender
{
    if (!adamdebug_is_paused(_dbg))
        return;
    unsigned v = 0;
    NSScanner *scan = [NSScanner scannerWithString:sender.stringValue];
    if (![scan scanHexInt:&v] || v > 0xFFFF)
        return;
    adamcore_z80_regs r;
    adamdebug_get_regs(_dbg, &r);
    reg_set(&r, (int)sender.tag, (uint16_t)v);
    adamdebug_set_regs(_dbg, &r);
    [self refreshRegs];
    [self refreshDisasm];
}

- (void)onStopped:(adamdebug_stop_reason)reason pc:(uint16_t)pc
{
    static const char *const names[] = {"paused", "breakpoint", "step",
                                        "run-to"};
    uint16_t off = 0;
    const char *sym = adamdebug_symbol_at(_dbg, pc, &off);
    NSString *text =
        [NSString stringWithFormat:@"Stopped (%s) at %04X",
                                   names[reason], pc];
    if (sym)
        text = [text stringByAppendingFormat:@"  %s+%X", sym, off];
    _status.stringValue = text;
    _followPc = YES;
    [self refreshAll];
}

/* ---- refreshers ------------------------------------------------------------ */

- (void)refreshAll
{
    [self refreshDisasm];
    [self refreshRegs];
    [self refreshMem];
    [self refreshBps];
    [self refreshTrace];
    [self refreshVdp];
    _pauseBtn.title = adamdebug_is_paused(_dbg) ? @"Continue (F5)"
                                                : @"Pause (F5)";
}

- (void)refreshDisasm
{
    adamdasm_line lines[DISASM_LINES];
    adamcore_z80_regs r;
    adamdebug_get_regs(_dbg, &r);
    if (_followPc)
        _disasmBase = r.pc;

    int n = adamdebug_disassemble(_dbg, _disasmBase, DISASM_LINES, lines);
    NSMutableString *text = [NSMutableString string];
    for (int i = 0; i < n; i++) {
        if (lines[i].symbol)
            [text appendFormat:@"%17s%s:\n", "", lines[i].symbol];
        char bytes[16] = "";
        for (int b = 0; b < lines[i].len; b++)
            snprintf(bytes + b * 3, 4, "%02X ", lines[i].bytes[b]);
        [text appendFormat:@"%c%c%04X  %-13s %s\n",
                           adamdebug_bp_is_set(_dbg, lines[i].addr) ? '*'
                                                                    : ' ',
                           lines[i].addr == r.pc ? '>' : ' ',
                           lines[i].addr, bytes, lines[i].text];
    }
    _disasm.string = text;
}

- (void)refreshRegs
{
    adamcore_z80_regs r;
    adamdebug_get_regs(_dbg, &r);
    for (int i = 0; i < 8; i++) {
        if (!_regField[i].currentEditor)
            _regField[i].stringValue =
                [NSString stringWithFormat:@"%04X", reg_get(&r, i)];
    }
    _flags.stringValue = [NSString
        stringWithFormat:@"F: %c%c%c%c%c%c  IFF1:%d IM:%d  cyc:%llu",
                         (r.f & 0x80) ? 'S' : '-', (r.f & 0x40) ? 'Z' : '-',
                         (r.f & 0x10) ? 'H' : '-', (r.f & 0x04) ? 'P' : '-',
                         (r.f & 0x02) ? 'N' : '-', (r.f & 0x01) ? 'C' : '-',
                         r.iff1, r.im, (unsigned long long)r.cycles];
}

- (void)refreshMem
{
    uint8_t data[MEM_ROWS * 16];
    adamdebug_read_mem(_dbg, _memBase, data, sizeof(data));
    NSMutableString *text = [NSMutableString string];
    for (int row = 0; row < MEM_ROWS; row++) {
        [text appendFormat:@"%04X  ", (uint16_t)(_memBase + row * 16)];
        for (int col = 0; col < 16; col++)
            [text appendFormat:@"%02X ", data[row * 16 + col]];
        [text appendString:@" "];
        for (int col = 0; col < 16; col++) {
            uint8_t c = data[row * 16 + col];
            [text appendFormat:@"%c",
                               (c >= 0x20 && c <= 0x7E) ? (char)c : '.'];
        }
        [text appendString:@"\n"];
    }
    _memView.string = text;
}

- (void)refreshBps
{
    uint16_t bps[128];
    int n = adamdebug_bp_list(_dbg, bps, 128);
    NSMutableString *text = [NSMutableString
        stringWithString:n ? @"" : @"(no breakpoints)\n"];
    for (int i = 0; i < n; i++) {
        uint16_t off = 0;
        const char *sym = adamdebug_symbol_at(_dbg, bps[i], &off);
        if (sym)
            [text appendFormat:@"%04X  %s+%X\n", bps[i], sym, off];
        else
            [text appendFormat:@"%04X\n", bps[i]];
    }
    _bpView.string = text;
}

- (void)refreshTrace
{
    static adamtrace_entry entries[256];
    int n = adamdebug_trace_read(_dbg, entries, 256);
    NSMutableString *text = [NSMutableString
        stringWithString:n ? @"  PC    AF   BC   DE   HL   SP\n"
                           : @"(trace empty; enable tracing and run)\n"];
    for (int i = 0; i < n; i++)
        [text appendFormat:@"%04X  %04X %04X %04X %04X %04X\n",
                           entries[i].pc, entries[i].af, entries[i].bc,
                           entries[i].de, entries[i].hl, entries[i].sp];
    _traceView.string = text;
}

- (void)refreshVdp
{
    static adamvdp_snapshot snap;
    static uint8_t nt[256 * 192 * 4], pat[256 * 64 * 4], spr[128 * 64 * 4],
        pal[16 * 4];
    adamvdp_sprite info[32];

    adamdebug_vdp_snapshot(_dbg, &snap);

    adamvdp_render_nametable(&snap, nt);
    _nt.image = imageFromRGBA(nt, 256, 192);

    adamvdp_render_patterns(&snap, (int)_patBank.indexOfSelectedItem, pat);
    _pat.image = imageFromRGBA(pat, 256, 64);

    adamvdp_render_sprites(&snap, spr, info);
    _spr.image = imageFromRGBA(spr, 128, 64);

    adamvdp_render_palette(&snap, pal);
    _pal.image = imageFromRGBA(pal, 16, 1);

    NSMutableString *text =
        [NSMutableString stringWithString:@"##  Y    X  PAT CLR EC   R0-R7: "];
    for (int i = 0; i < 8; i++)
        [text appendFormat:@"%02X ", snap.regs[i]];
    [text appendFormat:@" ST:%02X\n", snap.status];
    for (int i = 0; i < 32; i++)
        [text appendFormat:@"%02d %3d  %3d  %02X  %2d  %d\n", i, info[i].y,
                           info[i].x, info[i].pattern, info[i].color,
                           info[i].early_clock];
    _spriteInfo.string = text;
}

@end

/*
 * AppDelegate: builds the menu bar and main window, owns the session
 * lifecycle, and hosts the FujiNet configuration (WKWebView) and console
 * log windows. Mirrors the GTK/Qt frontends over the same adamsession API,
 * with the native debugger window in debugger/DebuggerWindow.m.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "AppDelegate.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <WebKit/WebKit.h>

#import "DisplayView.h"
#import "KeyForward.h"
#import "debugger/DebuggerWindow.h"
#import "keypad/KeypadWindow.h"

#include <stdlib.h>

static NSArray<UTType *> *typesForExtensions(NSArray<NSString *> *exts)
{
    NSMutableArray<UTType *> *types = [NSMutableArray array];
    for (NSString *ext in exts) {
        UTType *t = [UTType typeWithFilenameExtension:ext];
        if (t)
            [types addObject:t];
    }
    return types;
}

/* Settings that describe the machine: changing one needs the session
 * restarted, which is deferred until the Settings window closes so editing
 * several options does not reboot the ADAM under the user. Everything else
 * (the display options) applies live. */
static NSArray<NSString *> *machineSettingKeys(void)
{
    static NSArray<NSString *> *keys;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
      keys = @[
          @"machine", @"palette", @"expansion", @"joystick_mode",
          @"swap_buttons", @"reverse_keypad", @"fujinet_external",
          @"fujinet_external_addr"
      ];
    });
    return keys;
}

@interface AppDelegate () <NSWindowDelegate, WKUIDelegate>
@end

@implementation AppDelegate {
    adamsession *_session;
    NSWindow *_window;
    DisplayView *_display;
    NSWindow *_configWindow;
    NSWindow *_logWindow;
    NSTextView *_logView;
    NSTimer *_logTimer;
    NSWindow *_settingsWindow;
    NSTextField *_externalField;
    BOOL _machineDirty;
}

- (instancetype)initWithSession:(adamsession *)session
{
    self = [super init];
    if (self)
        _session = session;
    return self;
}

/* ---- session helpers ------------------------------------------------------ */

- (void)restartSession
{
    adamsession_stop(_session);
    adamsession_start_opts opts;
    adamsession_default_opts(_session, &opts);
    if (adamsession_start(_session, &opts) != 0)
        NSLog(@"session restart: %s", adamsession_last_error(_session));
}

- (void)applyDisplaySettings
{
    [_display setAspectMode:adamsession_get_int(_session, "aspect_mode", 0)];
    [_display setSmooth:adamsession_get_int(_session, "smooth_scaling", 0)];
}

/* ---- actions -------------------------------------------------------------- */

- (void)resetAdam:(id)sender
{
    (void)sender;
    adamsession_reset(_session, 0);
}

- (void)resetGame:(id)sender
{
    (void)sender;
    adamsession_reset(_session, 1);
}

- (void)importMedia:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title = @"Import Disk or Data Pack";
    panel.allowedContentTypes = typesForExtensions(@[ @"dsk", @"ddp" ]);
    if ([panel runModal] != NSModalResponseOK || !panel.URL)
        return;
    char dest[1024];
    if (adamsession_import_media(_session, panel.URL.path.UTF8String, dest,
                                 sizeof(dest)) != 0)
        NSLog(@"import: %s", adamsession_last_error(_session));
}

- (void)loadCartridge:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title = @"Load Cartridge";
    panel.allowedContentTypes =
        typesForExtensions(@[ @"rom", @"col", @"bin" ]);
    if ([panel runModal] != NSModalResponseOK || !panel.URL)
        return;
    char dest[1024];
    if (adamsession_import_media(_session, panel.URL.path.UTF8String, dest,
                                 sizeof(dest)) != 0) {
        NSLog(@"cartridge: %s", adamsession_last_error(_session));
        return;
    }
    adamsession_set_str(_session, "cart_path", dest);
    adamsession_set_int(_session, "machine", 1);
    [self restartSession];
}

- (void)ejectCartridge:(id)sender
{
    (void)sender;
    adamsession_set_str(_session, "cart_path", "");
    adamsession_set_int(_session, "machine", 0);
    [self restartSession];
}

- (void)showFujiNetConfig:(id)sender
{
    (void)sender;
    if (adamsession_fujinet_external(_session)) {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"FujiNet is external";
        alert.informativeText = @"Use the web UI of the FujiNet instance "
                                @"that connects to this app.";
        [alert runModal];
        return;
    }
    if (_configWindow) {
        [_configWindow makeKeyAndOrderFront:nil];
        return;
    }
    NSRect frame = NSMakeRect(0, 0, 1000, 760);
    /* Miniaturizable so Window ▸ Minimize (and the yellow button) apply to
     * the auxiliary windows too, not just the machine window. */
    _configWindow = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _configWindow.title = @"FujiNet Configuration";
    _configWindow.releasedWhenClosed = NO;
    WKWebView *web = [[WKWebView alloc] initWithFrame:frame];
    web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    web.UIDelegate = self;
    NSString *url = [NSString
        stringWithUTF8String:adamsession_fujinet_webui_url(_session)];
    [web loadRequest:[NSURLRequest
                         requestWithURL:[NSURL URLWithString:url]]];
    _configWindow.contentView = web;
    [_configWindow center];
    [_configWindow makeKeyAndOrderFront:nil];
}

/* The FujiNet web UI's OneDrive/Google Drive "Authorize" buttons open the
 * provider's consent page via window.open(). WKWebView has no popup window
 * of its own to show it in unless a UIDelegate supplies one, so hand the URL
 * to the system browser instead: Google and Microsoft both reject OAuth
 * flows from an embedded webview's user-agent anyway. */
- (WKWebView *)webView:(WKWebView *)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration
               forNavigationAction:(WKNavigationAction *)navigationAction
                    windowFeatures:(WKWindowFeatures *)windowFeatures
{
    (void)webView;
    (void)configuration;
    (void)windowFeatures;
    if (navigationAction.request.URL)
        [[NSWorkspace sharedWorkspace] openURL:navigationAction.request.URL];
    return nil;
}

- (void)showFujiNetLog:(id)sender
{
    (void)sender;
    if (adamsession_fujinet_external(_session)) {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"FujiNet is external";
        alert.informativeText = @"Its console output is in the process "
                                @"that is running it.";
        [alert runModal];
        return;
    }
    if (_logWindow) {
        [_logWindow makeKeyAndOrderFront:nil];
        return;
    }
    NSRect frame = NSMakeRect(0, 0, 820, 560);
    _logWindow = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _logWindow.title = @"FujiNet Console Log";
    _logWindow.releasedWhenClosed = NO;

    NSScrollView *scroll =
        [[NSScrollView alloc] initWithFrame:frame];
    scroll.hasVerticalScroller = YES;
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _logView = [[NSTextView alloc] initWithFrame:frame];
    _logView.editable = NO;
    _logView.font = [NSFont monospacedSystemFontOfSize:11
                                                weight:NSFontWeightRegular];
    scroll.documentView = _logView;
    _logWindow.contentView = scroll;

    _logTimer = [NSTimer
        scheduledTimerWithTimeInterval:1.0
                               repeats:YES
                                 block:^(NSTimer *timer) {
                                   (void)timer;
                                   [self refreshLog];
                                 }];
    [self refreshLog];
    [_logWindow center];
    [_logWindow makeKeyAndOrderFront:nil];
}

- (void)refreshLog
{
    static char buf[128 * 1024];
    int n = adamsession_fujinet_copy_log(_session, buf, sizeof(buf));
    NSString *text = n > 0 ? [NSString stringWithUTF8String:buf]
                           : @"(no FujiNet output yet)";
    if (text)
        _logView.string = text;
    [_logView scrollToEndOfDocument:nil];
}

/* F12 and View ▸ Debugger: shows the window (which stops the machine) or
 * closes it (which lets it run). */
- (void)toggleDebugger:(id)sender
{
    (void)sender;
    [DebuggerWindow toggleForSession:_session];
}

- (void)toggleKeypad:(id)sender
{
    (void)sender;
    [ADAMKeypadWindow toggleWithSession:_session];
}

/* ---- settings ------------------------------------------------------------- */

/* Rows write straight through to the shared settings store, the way the
 * GNOME preferences window does -- same keys, same defaults, same option
 * order as the GTK and Qt frontends (and the Android app's Settings.kt), so
 * a machine configured in one shows up the same in the others. */

- (NSTextField *)sectionLabel:(NSString *)title
{
    NSTextField *label = [NSTextField labelWithString:title];
    label.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    return label;
}

- (NSPopUpButton *)popUpForKey:(const char *)key
                       fallback:(int)def
                          items:(NSArray<NSString *> *)items
{
    NSPopUpButton *popup = [[NSPopUpButton alloc] init];
    [popup addItemsWithTitles:items];
    NSInteger current = adamsession_get_int(_session, key, def);
    if (current < 0 || current >= (NSInteger)items.count)
        current = def;
    [popup selectItemAtIndex:current];
    popup.identifier = @(key);
    popup.target = self;
    popup.action = @selector(settingChanged:);
    return popup;
}

- (NSButton *)checkBoxForKey:(const char *)key fallback:(int)def
{
    NSButton *box = [NSButton checkboxWithTitle:@""
                                         target:self
                                         action:@selector(settingChanged:)];
    box.state = adamsession_get_int(_session, key, def)
                    ? NSControlStateValueOn
                    : NSControlStateValueOff;
    box.identifier = @(key);
    return box;
}

- (void)settingChanged:(id)sender
{
    NSControl *control = sender;
    NSString *key = control.identifier;
    int value;

    if ([control isKindOfClass:[NSPopUpButton class]])
        value = (int)((NSPopUpButton *)control).indexOfSelectedItem;
    else
        value = ((NSButton *)control).state == NSControlStateValueOn ? 1 : 0;

    adamsession_set_int(_session, key.UTF8String, value);

    if ([key isEqualToString:@"fujinet_external"])
        _externalField.enabled = value != 0;
    if ([machineSettingKeys() containsObject:key])
        _machineDirty = YES;
    else
        [self applyDisplaySettings];
}

/* The external-FujiNet address field: valid text (or none, meaning
 * 127.0.0.1:65216) is saved; anything else is shown in red and not saved. */
- (void)externalAddrChanged:(id)sender
{
    NSTextField *field = sender;
    NSString *text = [field.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet
                                            .whitespaceAndNewlineCharacterSet];
    char host[64];
    int port;
    if (adamsession_parse_hostport(text.UTF8String, host, sizeof host,
                                   &port) != 0) {
        field.textColor = NSColor.systemRedColor;
        return;
    }
    field.textColor = NSColor.controlTextColor;
    adamsession_set_str(_session, "fujinet_external_addr", text.UTF8String);
    _machineDirty = YES;
}

- (void)showSettings:(id)sender
{
    (void)sender;
    if (_settingsWindow) {
        [_settingsWindow makeKeyAndOrderFront:nil];
        return;
    }

    _externalField = [NSTextField textFieldWithString:@(
        adamsession_get_str(_session, "fujinet_external_addr", ""))];
    _externalField.placeholderString = @"0.0.0.0:65216";
    _externalField.preferredMaxLayoutWidth = 240;
    _externalField.enabled =
        adamsession_get_int(_session, "fujinet_external", 0) != 0;
    _externalField.target = self;
    _externalField.action = @selector(externalAddrChanged:);
    [_externalField.cell setSendsActionOnEndEditing:YES];

    NSArray<NSArray<NSView *> *> *rows = @[
        @[ [self sectionLabel:@"Machine"], [NSTextField labelWithString:@""] ],
        @[
            [NSTextField labelWithString:@"Machine"],
            [self popUpForKey:"machine"
                     fallback:0
                        items:@[ @"ADAM (computer)", @"ColecoVision (game)" ]]
        ],
        @[
            [NSTextField labelWithString:@"Palette"],
            [self popUpForKey:"palette"
                     fallback:0
                        items:@[
                            @"Default (TMS9928)", @"Palette 2", @"Palette 3",
                            @"Palette 4"
                        ]]
        ],
        @[
            [NSTextField labelWithString:@"Expansion module"],
            [self popUpForKey:"expansion"
                     fallback:0
                        items:@[
                            @"None", @"Roller controller (mouse)",
                            @"Roller controller (joystick)",
                            @"Driving module (joystick)",
                            @"Driving module (mouse)",
                            @"Super Action speed roller, both ports (mouse)",
                            @"Speed roller, port 1 (mouse)",
                            @"Speed roller, port 2 (mouse)"
                        ]]
        ],
        @[
            [NSTextField labelWithString:@"Joystick mode"],
            [self popUpForKey:"joystick_mode"
                     fallback:1
                        items:@[
                            @"No joystick", @"Both ports", @"Port 2 only",
                            @"Port 1 only"
                        ]]
        ],
        @[
            [NSTextField labelWithString:@"Swap joystick buttons"],
            [self checkBoxForKey:"swap_buttons" fallback:0]
        ],
        @[
            [NSTextField labelWithString:@"Reverse keypad"],
            [self checkBoxForKey:"reverse_keypad" fallback:0]
        ],
        @[ [self sectionLabel:@"FujiNet"], [NSTextField labelWithString:@""] ],
        @[
            [NSTextField labelWithString:@"Use external FujiNet"],
            [self checkBoxForKey:"fujinet_external" fallback:0]
        ],
        @[
            [NSTextField labelWithString:@"Listen address:port"],
            _externalField
        ],
        @[ [self sectionLabel:@"Display"], [NSTextField labelWithString:@""] ],
        @[
            [NSTextField labelWithString:@"Aspect ratio"],
            [self popUpForKey:"aspect_mode"
                     fallback:0
                        items:@[
                            @"Square pixels (256:212)", @"TV (4:3)",
                            @"Integer scale"
                        ]]
        ],
        @[
            [NSTextField labelWithString:@"Smooth scaling"],
            [self checkBoxForKey:"smooth_scaling" fallback:0]
        ],
    ];

    NSGridView *grid = [NSGridView gridViewWithViews:rows];
    grid.rowSpacing = 8;
    grid.columnSpacing = 12;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;

    NSTextField *note = [NSTextField
        labelWithString:@"Display options apply immediately. Machine options "
                        @"take effect when this window is closed.\nAn "
                        @"external FujiNet connects to the address above: "
                        @"set its [BOIP] host and port to match."];
    note.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    note.textColor = NSColor.secondaryLabelColor;

    NSStackView *root = [NSStackView stackViewWithViews:@[ grid, note ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.spacing = 12;
    root.edgeInsets = NSEdgeInsetsMake(16, 16, 16, 16);

    _settingsWindow = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 520, 420)
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _settingsWindow.title = @"Settings";
    _settingsWindow.releasedWhenClosed = NO;
    _settingsWindow.delegate = self;
    _settingsWindow.contentView = root;
    [_settingsWindow center];
    [_settingsWindow makeKeyAndOrderFront:nil];
}

/* Machine options are collected while the window is open and applied in one
 * restart when it closes. */
- (void)windowWillClose:(NSNotification *)note
{
    if (note.object == _window)
        [self saveWindowSize];
    if (note.object == _settingsWindow)
        [_settingsWindow makeFirstResponder:nil]; /* commit a field edit */
    if (note.object != _settingsWindow || !_machineDirty)
        return;
    _machineDirty = NO;
    [self restartSession];
}

/* Losing key status with keys held would leave the machine believing they
 * are still down. */
- (void)windowDidResignKey:(NSNotification *)note
{
    if (note.object == _window)
        ADAMKeyFocusLost(_session);
}

/* ---- menus ---------------------------------------------------------------- */

- (NSMenuItem *)item:(NSString *)title action:(SEL)sel key:(NSString *)key
{
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title
                                                  action:sel
                                           keyEquivalent:key];
    item.target = self;
    return item;
}

- (void)buildMenus
{
    NSString *appName = @"FujiNet Go Adam";
    NSMenu *menubar = [[NSMenu alloc] init];

    /* A menu bar built in code gets nothing for free: the standard items
     * every Mac app is expected to have (Services, Hide/Hide Others/Show
     * All, Minimize/Zoom, the window list) exist only if they are added
     * here -- and Services and the window list stay empty until AppKit is
     * told which menus they belong to (servicesMenu / windowsMenu). */
    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    NSMenu *appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:[@"About " stringByAppendingString:appName]
                       action:@selector(orderFrontStandardAboutPanel:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];

    [appMenu addItem:[self item:@"Settings…"
                         action:@selector(showSettings:)
                            key:@","]];
    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenu *servicesMenu = [[NSMenu alloc] initWithTitle:@"Services"];
    NSMenuItem *servicesItem = [appMenu addItemWithTitle:@"Services"
                                                  action:NULL
                                           keyEquivalent:@""];
    servicesItem.submenu = servicesMenu;
    NSApp.servicesMenu = servicesMenu;
    [appMenu addItem:[NSMenuItem separatorItem]];

    [appMenu addItemWithTitle:[@"Hide " stringByAppendingString:appName]
                       action:@selector(hide:)
                keyEquivalent:@"h"];
    NSMenuItem *hideOthers =
        [appMenu addItemWithTitle:@"Hide Others"
                           action:@selector(hideOtherApplications:)
                    keyEquivalent:@"h"];
    hideOthers.keyEquivalentModifierMask =
        NSEventModifierFlagOption | NSEventModifierFlagCommand;
    [appMenu addItemWithTitle:@"Show All"
                       action:@selector(unhideAllApplications:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];

    [appMenu addItemWithTitle:[@"Quit " stringByAppendingString:appName]
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    [menubar addItem:appItem];

    NSMenuItem *machineItem = [[NSMenuItem alloc] init];
    NSMenu *machine = [[NSMenu alloc] initWithTitle:@"Machine"];
    [machine addItem:[self item:@"Reset Computer (ADAM)"
                         action:@selector(resetAdam:)
                            key:@""]];
    [machine addItem:[self item:@"Reset Game (ColecoVision)"
                         action:@selector(resetGame:)
                            key:@""]];
    machineItem.submenu = machine;
    [menubar addItem:machineItem];

    NSMenuItem *mediaItem = [[NSMenuItem alloc] init];
    NSMenu *media = [[NSMenu alloc] initWithTitle:@"Media"];
    [media addItem:[self item:@"Import Disk or Data Pack…"
                       action:@selector(importMedia:)
                          key:@"i"]];
    [media addItem:[self item:@"Load Cartridge…"
                       action:@selector(loadCartridge:)
                          key:@""]];
    [media addItem:[self item:@"Eject Cartridge"
                       action:@selector(ejectCartridge:)
                          key:@""]];
    mediaItem.submenu = media;
    [menubar addItem:mediaItem];

    NSMenuItem *fujiItem = [[NSMenuItem alloc] init];
    NSMenu *fuji = [[NSMenu alloc] initWithTitle:@"FujiNet"];
    [fuji addItem:[self item:@"Configuration…"
                      action:@selector(showFujiNetConfig:)
                         key:@""]];
    [fuji addItem:[self item:@"Console Log…"
                      action:@selector(showFujiNetLog:)
                         key:@""]];
    fujiItem.submenu = fuji;
    [menubar addItem:fujiItem];

    NSMenuItem *viewItem = [[NSMenuItem alloc] init];
    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
    NSMenuItem *fs = [view addItemWithTitle:@"Toggle Full Screen"
                                     action:@selector(toggleFullScreen:)
                              keyEquivalent:@"f"];
    fs.keyEquivalentModifierMask =
        NSEventModifierFlagControl | NSEventModifierFlagCommand;
    [view addItem:[self item:@"Controllers"
                      action:@selector(toggleKeypad:)
                         key:@"k"]];
    NSMenuItem *dbg = [self item:@"Debugger"
                          action:@selector(toggleDebugger:)
                             key:[NSString stringWithFormat:@"%C",
                                           (unichar)NSF12FunctionKey]];
    dbg.keyEquivalentModifierMask = 0;
    [view addItem:dbg];
    viewItem.submenu = view;
    [menubar addItem:viewItem];

    /* Window menu. AppKit appends the list of open windows (the main
     * window, the debugger, the FujiNet console log) to whichever menu is
     * handed to windowsMenu, and manages their check marks -- that list is
     * what "show or hide windows" means to a Mac user. */
    NSMenuItem *windowItem = [[NSMenuItem alloc] init];
    NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    /* Close lives here rather than in a File menu: there is no File menu,
     * and the debugger / config / log windows need a keyboard close. */
    [windowMenu addItemWithTitle:@"Close"
                          action:@selector(performClose:)
                   keyEquivalent:@"w"];
    [windowMenu addItemWithTitle:@"Minimize"
                          action:@selector(performMiniaturize:)
                   keyEquivalent:@"m"];
    [windowMenu addItemWithTitle:@"Zoom"
                          action:@selector(performZoom:)
                   keyEquivalent:@""];
    [windowMenu addItem:[NSMenuItem separatorItem]];
    [windowMenu addItemWithTitle:@"Bring All to Front"
                          action:@selector(arrangeInFront:)
                   keyEquivalent:@""];
    windowItem.submenu = windowMenu;
    [menubar addItem:windowItem];
    NSApp.windowsMenu = windowMenu;

    NSApp.mainMenu = menubar;
}

/* ---- lifecycle ------------------------------------------------------------ */

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;
    [self buildMenus];

    NSRect frame = NSMakeRect(0, 0, 1088, 902);
    {
        const int w = adamsession_get_int(_session, "window_width", 1088);
        const int h = adamsession_get_int(_session, "window_height", 902);
        if (w >= 200 && h >= 200)
            frame = NSMakeRect(0, 0, w, h);
    }
    _window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"FujiNet Go Adam";
    _window.delegate = self;
    _window.contentMinSize =
        NSMakeSize(ADAMSESSION_FB_WIDTH, ADAMSESSION_FB_HEIGHT);

    _display = [[DisplayView alloc] initWithSession:_session];
    _display.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _display.frame = ((NSView *)_window.contentView).bounds;
    [_window.contentView addSubview:_display];
    [self applyDisplaySettings];

    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_display];
    [_display start];

    /* Developer affordance shared with the other frontends. */
    if (getenv("ADAM_OPEN_DEBUGGER"))
        [DebuggerWindow showForSession:_session];
    if (getenv("ADAM_OPEN_KEYPAD"))
        [ADAMKeypadWindow toggleWithSession:_session];
}

/* The window's content size for the next start (not while fullscreen). */
- (void)saveWindowSize
{
    if (!_window || (_window.styleMask & NSWindowStyleMaskFullScreen))
        return;
    NSSize size = ((NSView *)_window.contentView).bounds.size;
    if (size.width < 200 || size.height < 200)
        return;
    adamsession_set_int(_session, "window_width", (int)size.width);
    adamsession_set_int(_session, "window_height", (int)size.height);
    adamsession_settings_flush(_session);
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    [self saveWindowSize];
    [_logTimer invalidate];
    [_display stop];
    adamsession_stop(_session);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:
    (NSApplication *)sender
{
    (void)sender;
    return YES;
}

@end

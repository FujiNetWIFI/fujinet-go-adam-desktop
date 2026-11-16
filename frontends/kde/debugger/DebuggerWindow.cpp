/*
 * Debugger window (Qt6 Widgets) over the Z80 / TMS9918A debugger engine, via
 * core/include/adamdebug.h and adamvidtab.h. Mirrors the GNOME one tab for
 * tab, and follows the MSX and INTV KDE debuggers (the family's newest):
 * twelve tabs with the editable register strip under every one.
 *
 * Tabs: the Console (the engine's command line, with a history and Tab
 * completion), the Disassembly (click a line to select it for Run to Cursor;
 * click its left edge, double-click it or press F9 for its breakpoint),
 * Memory (the 64K the CPU sees, Go to, Poke, search), Breakpoints (execute,
 * read, write, access, port IN and OUT, with an expression condition),
 * Watches, Cart & I/O (the machine, the cartridge, the memory map and the
 * controllers, VDP, CPU and AdamNet/FujiNet state), Trace (live) and History
 * (what ran before the stop), Video (VideoTab over the core's adamvidtab.h),
 * Sound (SN76489, and the AY-3-8910 of a ColecoVision Super Game Module), Symbols and
 * FujiNet (the AdamNet packets on the BoIP link). The toolbar carries
 * the stepping controls, symbol loading, Save and Pop Out; the family's keys
 * apply (F5 run/stop, F7 step, F8 step over, Shift+F8 step out, Shift+F7
 * frame, F9 breakpoint, F12 close).
 *
 * Showing the window attaches the engine, which stops the machine (as on every
 * sibling); hiding it detaches, which takes the breakpoints out of the core and
 * lets the machine run on.
 *
 * A short timer polls the engine's stop snapshot and refreshes the tabs showing when it moved; while the machine runs, the live
 * tabs refresh twice a second.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"
#include "../help/HelpWindow.h"
#include "Accent.h"
#include "VideoTab.h"

#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QShowEvent>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>
#include <QStyle>
#include <QTextBlock>
#include <QTextCursor>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

extern "C" {
#include "adammemtools.h"
}

#define DISASM_WINDOW 64        /* lines fetched; as many show as fit */
#define MEM_ROWS 256            /* 4K of memory, 16 bytes a row */
#define BP_MAX 256
#define TRACE_MAX 512
#define CONSOLE_LINES 5000
#define HIST_FETCH 4000         /* ring entries scanned for a Trace / History view */
#define HIST_SHOWN 400          /* matching lines shown */

namespace {

enum { TAB_CONSOLE, TAB_DISASM, TAB_MEMORY, TAB_BREAKS, TAB_WATCHES, TAB_CART, TAB_TRACE, TAB_HISTORY,
       TAB_VIDEO, TAB_SOUND, TAB_SYMBOLS, TAB_FUJINET, TAB_COUNT };

/* The tabs, in order: the names ADAM_DEBUGGER_TAB and ADAM_DEBUGGER_POPOUT take, and their
 * labels (which they take too); the Help context of each is its name. */
const struct { const char *name, *label; } kTabs[TAB_COUNT] = {
    { "console", "Console" }, { "disassembly", "Disassembly" }, { "memory", "Memory" },
    { "breakpoints", "Breakpoints" }, { "watches", "Watches" }, { "cart", "Cart & I/O" },
    { "trace", "Trace" }, { "history", "History" }, { "video", "Video" }, { "sound", "Sound" },
    { "symbols", "Symbols" }, { "fujinet", "FujiNet" },
};

/* The older names of the tabs that were folded in. */
const struct { const char *name; int tab; } kTabAliases[] = {
    { "cpu", TAB_DISASM }, { "registers", TAB_DISASM }, { "vdp", TAB_VIDEO }, { "vram", TAB_VIDEO }, { "io", TAB_CART },
};

/* The register entries, in the order shown, and the name the engine's
 * expression grammar knows each by (adamdebug_register_set). Pairs show four
 * digits, I and R two, IM bare. */
const struct { const char *name, *engine; int digits; } kRegs[15] = {
    { "AF", "af", 4 },   { "BC", "bc", 4 },   { "DE", "de", 4 },   { "HL", "hl", 4 },
    { "IX", "ix", 4 },   { "IY", "iy", 4 },   { "SP", "sp", 4 },   { "PC", "pc", 4 },
    { "AF'", "af'", 4 }, { "BC'", "bc'", 4 }, { "DE'", "de'", 4 }, { "HL'", "hl'", 4 },
    { "I", "i", 2 },     { "R", "r", 2 },     { "IM", "im", 0 },
};
enum { REG_PC = 7 };

/* The flags: their bit in F. */
const struct { const char *name, *engine; int mask; } kFlags[6] = {
    { "S", "sf", 0x80 }, { "Z", "zf", 0x40 }, { "H", "hf", 0x10 }, { "PV", "pf", 0x04 }, { "N", "nf", 0x02 }, { "C", "cf", 0x01 },
};

const QString kGray = QStringLiteral("color: gray;");

/* The accent: the Run button while stopped, the PC's line, a marked row. On a
 * dark theme the light variant with black ink (Accent.h). */
QTextCharFormat accentFormat(const QWidget *wid)
{
    QTextCharFormat fmt;
    fmt.setBackground(adamDebugAccentFill(wid));
    fmt.setForeground(adamDebugAccentFillInk(wid));
    return fmt;
}

void setMono(QWidget *widget)
{
    QFont font = widget->font();
    font.setFamily(QStringLiteral("monospace"));
    font.setStyleHint(QFont::TypeWriter);
    widget->setFont(font);
}

QPlainTextEdit *monoView()
{
    auto *view = new QPlainTextEdit;
    view->setReadOnly(true);
    setMono(view);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    return view;
}

/* Engine text for a view: control bytes dropped. */
QString stripControl(const char *chars)
{
    const QString in = QString::fromUtf8(chars);
    QString out;
    out.reserve(in.size());
    for (const QChar letter : in)
        if (letter.unicode() >= 0x20 || letter == QLatin1Char('\n') || letter == QLatin1Char('\t')) out += letter;
    return out;
}

QString hexText(unsigned value, int digits)
{
    return QStringLiteral("%1").arg(value, digits, 16, QLatin1Char('0')).toUpper();
}

QLabel *titleLabel(const QString &text)
{
    auto *label = new QLabel(text);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

QLabel *hintLabel(const QString &text)
{
    auto *label = new QLabel(text);
    label->setStyleSheet(kGray);
    label->setWordWrap(true);
    return label;
}

QLineEdit *smallEntry(const QString &placeholder, int chars)
{
    auto *entry = new QLineEdit;
    entry->setPlaceholderText(placeholder);
    entry->setFixedWidth(entry->fontMetrics().averageCharWidth() * chars + 16);
    return entry;
}

/* A register entry exactly wide enough for its hex digits. */
void setDigits(QLineEdit *entry, int digits)
{
    entry->setFixedWidth(entry->fontMetrics().horizontalAdvance(QString(digits, QLatin1Char('0'))) + 16);
}

/* Whether a view's reader is at its end (a log then follows new lines). */
bool atEnd(QPlainTextEdit *view)
{
    return view->verticalScrollBar()->value() >= view->verticalScrollBar()->maximum() - 1;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
void setText(QPlainTextEdit *view, const QString &text, bool follow = false)
{
    if (view->toPlainText() == text) {
        if (follow) view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
        return;
    }
    const int top = view->verticalScrollBar()->value(), left = view->horizontalScrollBar()->value();
    view->setPlainText(text);
    view->verticalScrollBar()->setValue(follow ? view->verticalScrollBar()->maximum() : top);
    view->horizontalScrollBar()->setValue(left);
}

/* A whole line of a view in a format: the accent fill, a mark. */
QTextEdit::ExtraSelection lineSelection(QPlainTextEdit *view, int line, const QTextCharFormat &fmt)
{
    QTextEdit::ExtraSelection sel;
    sel.cursor = QTextCursor(view->document()->findBlockByNumber(line));
    sel.format = fmt;
    sel.format.setProperty(QTextFormat::FullWidthSelection, true);
    return sel;
}

/* A status line that never widens the window. It is clipped, not elided: the
 * stop reason ("wrote $7000 = $12 (was $11) at $4A3C") is read whole in the
 * tooltip and the Breakpoints tab, and never turns into "...". */
class StatusLabel : public QLabel {
public:
    StatusLabel() { setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); }
    QSize minimumSizeHint() const override { return QSize(40, QLabel::minimumSizeHint().height()); }
};

/* ADAM_DEBUGGER_TAB and ADAM_DEBUGGER_POPOUT: a page number, or a tab's name or label
 * (any case), or the start of one, or of a word of a label ("memory"), or an
 * older name (cpu, vdp, io ...). */
int tabFromEnv(const char *variable = "ADAM_DEBUGGER_TAB")
{
    const QString tab = qEnvironmentVariable(variable).trimmed();
    if (tab.isEmpty()) return -1;
    bool isNum = false;
    const int number = tab.toInt(&isNum);
    if (isNum) return number >= 0 && number < TAB_COUNT ? number : -1;
    for (const auto &alias : kTabAliases)
        if (tab.compare(QString::fromUtf8(alias.name), Qt::CaseInsensitive) == 0) return alias.tab;
    for (int pass = 0; pass < 3; ++pass)
        for (int tabIdx = 0; tabIdx < TAB_COUNT; ++tabIdx) {
            const QString name = QString::fromUtf8(kTabs[tabIdx].name), label = QString::fromUtf8(kTabs[tabIdx].label);
            if (pass == 0 && name.startsWith(tab, Qt::CaseInsensitive)) return tabIdx;
            if (pass == 1 && label.startsWith(tab, Qt::CaseInsensitive)) return tabIdx;
            if (pass == 2) {
                const QStringList words = label.split(QLatin1Char(' '));
                for (int wordPos = 1; wordPos < words.size(); ++wordPos)
                    if (words[wordPos].startsWith(tab, Qt::CaseInsensitive)) return tabIdx;
            }
        }
    return -1;
}

/* The label a tab shows: "&" starts a mnemonic in tab text, so it is doubled. */
QString tabLabel(int tab)
{
    return QString::fromUtf8(kTabs[tab].label).replace(QLatin1Char('&'), QLatin1String("&&"));
}

/* The family's keys: F5 run/stop, F7 step, Shift+F7 frame, F8 step over,
 * Shift+F8 step out, F9 breakpoint, F12 close. */
bool isDebuggerKey(const QKeyEvent *evt)
{
    const Qt::KeyboardModifiers mods = evt->modifiers() & ~Qt::KeypadModifier;
    switch (evt->key()) {
    case Qt::Key_F5: case Qt::Key_F9: case Qt::Key_F12: return mods == Qt::NoModifier;
    case Qt::Key_F7: case Qt::Key_F8: return mods == Qt::NoModifier || mods == Qt::ShiftModifier;
    default: return false;
    }
}

/* A tone as a note: "A4 (+3 cents)". */
QString noteName(double hertz)
{
    static const char *const kNotes[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    if (hertz < 8.0) return QStringLiteral("-");
    const double midi = 69.0 + 12.0 * std::log2(hertz / 440.0);
    const long note = std::lround(midi);
    return QStringLiteral("%1 Hz  %2%3 (%4 cents)")
        .arg(hertz, 0, 'f', 1)
        .arg(QLatin1String(kNotes[((note % 12) + 12) % 12]))
        .arg(note / 12 - 1)
        .arg(std::lround((midi - double(note)) * 100.0));
}

QString onOff(int value) { return value ? QStringLiteral("on ") : QStringLiteral("off"); }

/* A tab's window: closing it docks the tab again. The family's keys work in
 * it too, handed to the debugger. */
class PopWindow : public QWidget {
public:
    std::function<void()> onClose;
    std::function<bool(QKeyEvent *)> onKey;
protected:
    void closeEvent(QCloseEvent *evt) override
    {
        if (onClose) onClose();
        QWidget::closeEvent(evt);
    }
    bool event(QEvent *evt) override
    {
        if (evt->type() == QEvent::ShortcutOverride && isDebuggerKey(static_cast<QKeyEvent *>(evt))) {
            evt->accept();
            return true;
        }
        return QWidget::event(evt);
    }
    void keyPressEvent(QKeyEvent *evt) override
    {
        if (!onKey || !onKey(evt)) QWidget::keyPressEvent(evt);
    }
};

QPointer<DebuggerWindow> s_win;

/* The frame button and Shift+F7: one frame. */
void frameOne(adamdebug *dbg)
{
    adamdebug_frame(dbg, 1);
}

} // namespace

void DebuggerWindow::showFor(QWidget *parent, adamsession *session)
{
    if (!s_win) s_win = new DebuggerWindow(session, parent);
    /* the machine keeps running */
    s_win->show();
    s_win->raise();
    s_win->activateWindow();
}

void DebuggerWindow::toggleFor(QWidget *parent, adamsession *session)
{
    if (s_win && s_win->isVisible()) s_win->hide();
    else showFor(parent, session);
}

DebuggerWindow::DebuggerWindow(adamsession *session, QWidget *parent)
    : QMainWindow(parent, Qt::Window), m_session(session), m_dbg(adamsession_debugger(session))
{
    setObjectName(QStringLiteral("debugger"));
    setWindowTitle(QStringLiteral("ADAM Debugger"));
    resize(1150, 800);

    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->addWidget(buildToolbar());
    m_tabs = new QTabWidget;
    m_tabs->setUsesScrollButtons(true);
    QWidget *pages[TAB_COUNT];
    pages[TAB_CONSOLE] = buildConsole();
    pages[TAB_DISASM] = buildDisasm();
    pages[TAB_MEMORY] = buildMemory();
    pages[TAB_BREAKS] = buildBreaks();
    pages[TAB_WATCHES] = buildWatches();
    pages[TAB_CART] = buildCart();
    pages[TAB_TRACE] = buildTrace(0);
    pages[TAB_HISTORY] = buildTrace(1);
    pages[TAB_VIDEO] = buildVideo();
    pages[TAB_SOUND] = buildSound();
    pages[TAB_SYMBOLS] = buildSymbols();
    pages[TAB_FUJINET] = buildFujinet();
    for (int idx = 0; idx < TAB_COUNT; ++idx) {
        pages[idx]->setProperty("tab", idx);     /* each page keeps its identity when tabs move */
        m_pages[idx] = pages[idx];
        m_tabs->addTab(pages[idx], tabLabel(idx));
    }
    const int tab = tabFromEnv();
    if (tab >= 0) m_tabs->setCurrentIndex(tab);
    /* Help's contexts (adamhelp.h), one per tab */
    auto setHelpContext = [this]() {
        const int page = currentTab();
        if (page >= 0 && page < TAB_COUNT)
            setProperty("adamHelpContext", QStringLiteral("debugger/%1").arg(QLatin1String(kTabs[page].name)));
    };
    setHelpContext();
    new QShortcut(QKeySequence(Qt::Key_F1), this, [this] { adamHelpFor(this); });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this, setHelpContext](int) {
        setHelpContext();
        /* the view has no size until it shows: fill it now */
        refreshTab(currentTab(), false);
        if (currentTab() == TAB_CONSOLE) m_conIn->setFocus();
    });
    root->addWidget(m_tabs, 1);
    root->addWidget(buildRegs());
    setCentralWidget(central);
    statusBar();

    m_envWatch = qgetenv("ADAM_DEBUGGER_WATCH");
    m_envWatchBreak = qgetenv("ADAM_DEBUGGER_WATCH_BREAK");
    m_envMemGoto = qgetenv("ADAM_DEBUGGER_MEM_GOTO");
    m_envHistory = qEnvironmentVariableIsSet("ADAM_DEBUGGER_HISTORY");
    const int popTab = tabFromEnv("ADAM_DEBUGGER_POPOUT");
    if (popTab >= 0) popOut(popTab);     /* for captures: peel a tab off at the start */

    /* the timer runs only while the window is shown (showEvent/hideEvent) */
    connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
}

DebuggerWindow::~DebuggerWindow()
{
    m_closing = true;
    if (adamdebug_is_attached(m_dbg)) adamdebug_detach(m_dbg);
    for (QWidget *&win : m_popped)
        if (win) {
            QWidget *popped = win;
            win = nullptr;
            popped->close();
        }
}

/* ---- building ------------------------------------------------------------- */

QWidget *DebuggerWindow::buildToolbar()
{
    /* a tool bar, so a narrow window folds the buttons that do not fit into its extension menu */
    auto *bar = new QToolBar;
    bar->setMovable(false);
    auto add = [&](const QString &label, const QString &tip, std::function<void()> action) {
        auto *button = new QPushButton(label);
        button->setFocusPolicy(Qt::NoFocus);
        button->setToolTip(tip);
        connect(button, &QPushButton::clicked, this, action);
        bar->addWidget(button);
        return button;
    };
    m_runBtn = add(QStringLiteral("Stop (F5)"), QStringLiteral("Run, or stop before the next instruction"),
                   [this] { toggleRun(); });
    add(QStringLiteral("Step (F7)"), QStringLiteral("One instruction"), [this] { stepAnd(adamdebug_step); });
    add(QStringLiteral("Step Over (F8)"), QStringLiteral("Over a CALL, HALT or block instruction"),
        [this] { stepAnd(adamdebug_step_over); });
    add(QStringLiteral("Step Out (Shift+F8)"), QStringLiteral("On to the RET"),
        [this] { stepAnd(adamdebug_step_out); });
    add(QStringLiteral("Run to Cursor"), QStringLiteral("On to the line selected in the Disassembly"),
        [this] { runToCursor(); });
    add(QStringLiteral("Frame (Shift+F7)"), QStringLiteral("On to the end of the frame"),
        [this] { stepAnd(frameOne); });
    auto *spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(spacer);

    auto *sym = new QPushButton(QStringLiteral("Symbols"));
    sym->setFocusPolicy(Qt::NoFocus);
    sym->setToolTip(QStringLiteral("Labels from a .sym file (HHHH NAME per line)"));
    auto *symMenu = new QMenu(sym);
    symMenu->addAction(QStringLiteral("Symbol file…"), this, [this] { loadSymbols(true); });
    symMenu->addAction(QStringLiteral("Next to the cartridge"), this, [this] { loadSymbols(false); });
    symMenu->addAction(QStringLiteral("Clear loaded symbols"), this, [this] {
        adamdebug_clear_symbols(m_dbg);
        refreshSymbols(true);
        resync();
    });
    sym->setMenu(symMenu);
    bar->addWidget(sym);

    auto *saveBtn = new QPushButton(QStringLiteral("Save"));
    saveBtn->setFocusPolicy(Qt::NoFocus);
    auto *menu = new QMenu(saveBtn);
    menu->addAction(QStringLiteral("Disassembly ($0000-$FFFF as mapped)"), this,
                    [this] { save("dis", QStringLiteral("Disassembly"), QStringLiteral("disassembly.asm")); });
    menu->addAction(QStringLiteral("Memory (the 64K the CPU sees)"), this,
                    [this] { save("mem", QStringLiteral("Memory"), QStringLiteral("memory.bin")); });
    menu->addAction(QStringLiteral("VRAM"), this,
                    [this] { save("vram", QStringLiteral("VRAM"), QStringLiteral("vram.bin")); });
    menu->addAction(QStringLiteral("CPU registers"), this,
                    [this] { save("regs", QStringLiteral("CPU registers"), QStringLiteral("registers.txt")); });
    saveBtn->setMenu(menu);
    bar->addWidget(saveBtn);
    add(QStringLiteral("Pop Out"), QStringLiteral("Move the tab that shows into a window of its own; Dock puts it back"),
        [this] { popOut(currentTab()); });
    bar->addWidget(adamHelpButton(QString()));

    /* the status line sits in the status bar: the toolbar has no room for it */
    m_status = new StatusLabel;
    m_status->setAlignment(Qt::AlignLeft);
    statusBar()->addWidget(m_status, 1);
    return bar;
}

/* The Z80's registers, under every tab. Enter sets one (only while stopped);
 * a flag or IFF is a check box. */
QWidget *DebuggerWindow::buildRegs()
{
    auto *box = new QGroupBox(QStringLiteral("Registers"));
    auto *regs = new QGridLayout(box);
    regs->setVerticalSpacing(2);
    regs->setHorizontalSpacing(8);
    /* the main set on the first row, the shadow set and the rest on the second */
    for (int idx = 0; idx < 15; ++idx) {
        const int row = idx < 8 ? 0 : 1, col = 2 * (idx < 8 ? idx : idx - 8);
        auto *nameLabel = new QLabel(QString::fromUtf8(kRegs[idx].name));
        nameLabel->setStyleSheet(kGray);
        regs->addWidget(nameLabel, row, col, Qt::AlignRight);
        m_reg[idx] = new QLineEdit;
        setMono(m_reg[idx]);
        m_reg[idx]->setMaxLength(24);
        setDigits(m_reg[idx], kRegs[idx].digits ? kRegs[idx].digits : 2);
        m_reg[idx]->setToolTip(QStringLiteral("Enter sets it (a bare number is hex; an expression such as hl + 1 works; "
                                            "only while stopped)"));
        connect(m_reg[idx], &QLineEdit::returnPressed, this, [this, idx] { setReg(idx); });
        regs->addWidget(m_reg[idx], row, col + 1);
    }

    auto *flags = new QHBoxLayout;
    for (int idx = 0; idx < 6; ++idx) {
        m_flag[idx] = new QCheckBox(QString::fromUtf8(kFlags[idx].name));
        m_flag[idx]->setToolTip(QStringLiteral("Flag %1 of F (only while stopped)").arg(QString::fromUtf8(kFlags[idx].name)));
        connect(m_flag[idx], &QCheckBox::clicked, this, [this, idx](bool enabled) { setFlag(idx, enabled); });
        flags->addWidget(m_flag[idx]);
    }
    flags->addSpacing(16);
    for (int idx = 0; idx < 2; ++idx) {
        m_iff[idx] = new QCheckBox(QStringLiteral("IFF%1").arg(idx + 1));
        connect(m_iff[idx], &QCheckBox::clicked, this, [this, idx](bool enabled) { setIff(idx, enabled); });
        flags->addWidget(m_iff[idx]);
    }
    m_haltNote = new QLabel;
    m_haltNote->setStyleSheet(kGray);
    flags->addSpacing(16);
    flags->addWidget(m_haltNote);
    flags->addStretch(1);
    regs->addLayout(flags, 2, 0, 1, 16);
    regs->setColumnStretch(16, 1);
    return box;
}

QWidget *DebuggerWindow::buildConsole()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    m_conOut = monoView();
    m_conOut->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_conOut->setMaximumBlockCount(CONSOLE_LINES);
    m_conOut->setPlainText(QStringLiteral("Debugger console. Type 'help' for every command. Numbers: $8000 or 0x8000 "
                                          "for hex; a bare number is decimal in an expression and hex where a "
                                          "command takes an address (unless it names a label).\n"));
    m_conIn = new QLineEdit;
    setMono(m_conIn);
    m_conIn->setPlaceholderText(QStringLiteral("command (help, step, break, print, mem ...) — Tab completes, Up/Down recall"));
    m_conIn->installEventFilter(this);
    connect(m_conIn, &QLineEdit::returnPressed, this, [this] { runConsole(); });
    layout->addWidget(m_conOut, 1);
    layout->addWidget(m_conIn);
    return page;
}

/* The dump: Go to, a page back or on, Poke, and the search bar. */
QWidget *DebuggerWindow::buildMemory()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    auto *nav = new QHBoxLayout;
    nav->addWidget(new QLabel(QStringLiteral("Go to")));
    m_memGoto = smallEntry(QStringLiteral("$8000, label or lo-hi"), 22);
    connect(m_memGoto, &QLineEdit::returnPressed, this, [this] { memGoto(); });
    nav->addWidget(m_memGoto);
    for (int dir = -1; dir <= 1; dir += 2) {
        auto *arrow = new QToolButton;
        arrow->setArrowType(dir < 0 ? Qt::UpArrow : Qt::DownArrow);
        arrow->setToolTip(dir < 0 ? QStringLiteral("Back 4K") : QStringLiteral("On 4K"));
        connect(arrow, &QToolButton::clicked, this, [this, dir] {
            m_memTop = qBound(0, m_memTop + dir * MEM_ROWS * 16, 0x10000 - MEM_ROWS * 16);
            refreshMem();
        });
        nav->addWidget(arrow);
    }
    nav->addSpacing(12);
    nav->addWidget(new QLabel(QStringLiteral("Poke")));
    m_memAddr = smallEntry(QStringLiteral("address or label"), 16);
    m_memVal = smallEntry(QStringLiteral("value"), 10);
    m_memVal->setToolTip(QStringLiteral("A byte: hex, or an expression. Only while stopped; ROM ignores it."));
    auto *pokeBtn = new QPushButton(QStringLiteral("Poke"));
    connect(m_memAddr, &QLineEdit::returnPressed, this, [this] { memPoke(); });
    connect(m_memVal, &QLineEdit::returnPressed, this, [this] { memPoke(); });
    connect(pokeBtn, &QPushButton::clicked, this, [this] { memPoke(); });
    nav->addWidget(m_memAddr);
    nav->addWidget(m_memVal);
    nav->addWidget(pokeBtn);
    nav->addStretch(1);
    layout->addLayout(nav);

    auto *search = new QHBoxLayout;
    search->addWidget(new QLabel(QStringLiteral("Search")));
    m_srchKind = new QComboBox;
    m_srchKind->addItems({ QStringLiteral("Hex bytes"), QStringLiteral("Text") });
    m_srchKind->setToolTip(QStringLiteral("Hex bytes (3E 01) or text, one byte per character"));
    search->addWidget(m_srchKind);
    m_srchEntry = new QLineEdit;
    m_srchEntry->setPlaceholderText(QStringLiteral("bytes or text"));
    connect(m_srchEntry, &QLineEdit::returnPressed, this, [this] { memSearch(false); });
    search->addWidget(m_srchEntry, 1);
    auto *next = new QPushButton(QStringLiteral("Find Next"));
    auto *prev = new QPushButton(QStringLiteral("Find Previous"));
    connect(next, &QPushButton::clicked, this, [this] { memSearch(false); });
    connect(prev, &QPushButton::clicked, this, [this] { memSearch(true); });
    search->addWidget(next);
    search->addWidget(prev);
    m_srchNote = new QLabel;
    m_srchNote->setStyleSheet(kGray);
    search->addWidget(m_srchNote);
    layout->addLayout(search);

    layout->addWidget(titleLabel(QStringLiteral("Memory (the 64K as the CPU sees it)")));
    m_mem = monoView();
    layout->addWidget(m_mem, 1);
    return page;
}

QWidget *DebuggerWindow::buildDisasm()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *row = new QHBoxLayout;
    m_followPc = new QCheckBox(QStringLiteral("Follow PC"));
    m_followPc->setChecked(true);
    connect(m_followPc, &QCheckBox::clicked, this, [this] { refreshDisasm(); });
    m_jump = smallEntry(QStringLiteral("$address or label"), 20);
    connect(m_jump, &QLineEdit::returnPressed, this, [this] { jumpTo(); });
    row->addWidget(m_followPc);
    row->addSpacing(8);
    row->addWidget(new QLabel(QStringLiteral("Jump to")));
    row->addWidget(m_jump);
    row->addSpacing(8);
    row->addWidget(hintLabel(QStringLiteral("Click a line to select it (Run to Cursor); click its left edge, "
                                            "double-click it or press F9 for a breakpoint; scroll to browse")), 1);
    layout->addLayout(row);
    m_disasm = monoView();
    /* no scrollbar: the wheel scrolls by instructions, as many lines as fit */
    m_disasm->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_disasm->setFocusPolicy(Qt::NoFocus);
    m_disasm->viewport()->setCursor(Qt::ArrowCursor);
    m_disasm->viewport()->installEventFilter(this);
    layout->addWidget(m_disasm, 1);
    return page;
}

QWidget *DebuggerWindow::buildBreaks()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    auto *top = new QHBoxLayout;
    top->addWidget(hintLabel(QStringLiteral("Execute stops before the instruction runs; the others after the "
                                            "access. Breakpoints are kept while the debugger is closed, but only "
                                            "act while it is open.")), 1);
    layout->addLayout(top);

    m_bpTable = new QTableWidget(0, 6);
    m_bpTable->setHorizontalHeaderLabels({ QStringLiteral("On"), QStringLiteral("Type"), QStringLiteral("Range"),
                                           QStringLiteral("Condition"), QStringLiteral("Hits"), QString() });
    m_bpTable->verticalHeader()->setVisible(false);
    m_bpTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_bpTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_bpTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_bpTable->setEditTriggers(QAbstractItemView::DoubleClicked);     /* only the Condition cells are editable */
    m_bpTable->setFocusPolicy(Qt::NoFocus);
    connect(m_bpTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_bpFilling) return;
        if (item->column() == 3) {
            /* an edited condition: the engine checks it, and an empty one removes it */
            char err[200] = "";
            const QByteArray text = item->text().trimmed().toUtf8();
            if (adamdebug_breakpoint_set_condition(m_dbg, item->data(Qt::UserRole).toInt(), text.constData(), err, sizeof err))
                m_bpMsg->clear();
            else
                m_bpMsg->setText(err[0] ? stripControl(err) : QStringLiteral("The condition could not be set"));
            m_bpSig.clear();
            syncLater();
            return;
        }
        if (item->column() != 0) return;
        adamdebug_breakpoint_enable(m_dbg, item->data(Qt::UserRole).toInt(), item->checkState() == Qt::Checked);
        syncLater();
    });
    layout->addWidget(m_bpTable, 1);
    m_bpEmpty = hintLabel(QStringLiteral("No breakpoints. Click the left edge of a disassembly line, or add one below."));
    layout->addWidget(m_bpEmpty);

    auto *row = new QHBoxLayout;
    m_bpType = new QComboBox;
    m_bpType->addItems({ QStringLiteral("Execute"), QStringLiteral("Read"), QStringLiteral("Write"),
                         QStringLiteral("Access"), QStringLiteral("In"), QStringLiteral("Out") });
    m_bpType->setToolTip(QStringLiteral("Access is a read or a write of the range; In and Out take a port ($00-$FF)"));
    m_bpStart = smallEntry(QStringLiteral("start ($8000 or label)"), 22);
    m_bpEnd = smallEntry(QStringLiteral("end (optional)"), 16);
    m_bpCond = new QLineEdit;
    m_bpCond->setPlaceholderText(QStringLiteral("condition (optional): value != old, a == 5, [$8000] > 3"));
    m_bpCond->setToolTip(QStringLiteral(
        "An expression: numbers ($8000), the Z80 registers (a hl ix af' ...), flags (zf cf ...), labels, [addr] "
        "(a byte) and {addr} (a word), frame, cycles, vline, and for an access value, addr and old (the byte a "
        "write replaced). Empty stops every time."));
    auto *add = new QPushButton(QStringLiteral("Add"));
    auto *clearHits = new QPushButton(QStringLiteral("Clear Hits"));
    auto *clear = new QPushButton(QStringLiteral("Clear All"));
    connect(add, &QPushButton::clicked, this, [this] { addBreakpoint(); });
    connect(clearHits, &QPushButton::clicked, this, [this] {
        adamdebug_breakpoint_clear_hits(m_dbg);
        resync();
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        adamdebug_breakpoint_clear(m_dbg);
        resync();
    });
    for (QLineEdit *edit : { m_bpStart, m_bpEnd, m_bpCond })
        connect(edit, &QLineEdit::returnPressed, this, [this] { addBreakpoint(); });
    row->addWidget(m_bpType);
    row->addWidget(m_bpStart);
    row->addWidget(m_bpEnd);
    row->addWidget(m_bpCond, 1);
    row->addWidget(add);
    row->addWidget(clearHits);
    row->addWidget(clear);
    layout->addLayout(row);

    m_bpMsg = new QLabel;
    m_bpMsg->setWordWrap(true);
    m_bpMsg->setStyleSheet(QStringLiteral("color: #e01b24;"));
    layout->addWidget(m_bpMsg);
    m_bpStop = new QLabel;
    m_bpStop->setStyleSheet(kGray);
    layout->addWidget(m_bpStop);
    return page;
}

QWidget *DebuggerWindow::buildWatches()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *form = new QHBoxLayout;
    form->addWidget(new QLabel(QStringLiteral("Watch")));
    m_wtEntry = new QLineEdit;
    m_wtEntry->setPlaceholderText(QStringLiteral("address, label, expression or first-last"));
    connect(m_wtEntry, &QLineEdit::returnPressed, this, [this] { addWatch(); });
    form->addWidget(m_wtEntry, 1);
    auto *add = new QPushButton(QStringLiteral("Add"));
    connect(add, &QPushButton::clicked, this, [this] { addWatch(); });
    form->addWidget(add);
    layout->addLayout(form);

    m_wtList = new QListWidget;
    setMono(m_wtList);
    layout->addWidget(m_wtList, 1);

    auto *foot = new QHBoxLayout;
    foot->addStretch();
    auto *mem = new QPushButton(QStringLiteral("Memory"));
    mem->setToolTip(QStringLiteral("Show the selected watch in the Memory tab"));
    connect(mem, &QPushButton::clicked, this, [this] {
        uint16_t first, last;
        if (m_wtList->currentRow() >= 0 && adamwatch_range(m_dbg, m_wtList->currentRow(), &first, &last)) {
            showTab(TAB_MEMORY);
            showInMemory(first, last);
        }
    });
    auto *brk = new QPushButton(QStringLiteral("Break on Change"));
    brk->setToolTip(QStringLiteral("A write breakpoint on it with the condition value != old"));
    connect(brk, &QPushButton::clicked, this, [this] {
        uint16_t first, last;
        if (m_wtList->currentRow() < 0 || !adamwatch_range(m_dbg, m_wtList->currentRow(), &first, &last)) return;
        char err[200] = "";
        if (adamdebug_breakpoint_add(m_dbg, ADAMDEBUG_BP_WRITE, first, last, "value != old", err, sizeof err) >= 0)
            message(QStringLiteral("Breaking when it changes"));
        else
            message(err[0] ? stripControl(err) : QStringLiteral("The breakpoint could not be added"));
        syncLater();
    });
    auto *remove = new QPushButton(QStringLiteral("Remove"));
    connect(remove, &QPushButton::clicked, this, [this] {
        if (m_wtList->currentRow() >= 0) {
            adamwatch_remove(m_wtList->currentRow());
            syncLater();
        }
    });
    foot->addWidget(mem);
    foot->addWidget(brk);
    foot->addWidget(remove);
    layout->addLayout(foot);
    return page;
}

/* The machine and memory map above, the I/O below (controllers, VDP, CPU, Super
 * Game Module). */
QWidget *DebuggerWindow::buildCart()
{
    m_cartText = monoView();
    m_ioText = monoView();
    m_cartText->setLineWrapMode(QPlainTextEdit::WidgetWidth);      /* the device list is long */
    auto *split = new QSplitter(Qt::Vertical);
    split->addWidget(m_cartText);
    split->addWidget(m_ioText);
    split->setChildrenCollapsible(false);
    split->setSizes({ 330, 270 });
    return split;
}

/* Trace (0) and History (1): a Record switch, a filter, a note and the text.
 * Trace follows the machine live; History is what ran before the stop. */
QWidget *DebuggerWindow::buildTrace(int which)
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *bar = new QHBoxLayout;

    m_hRec[which] = new QPushButton(QStringLiteral("Record"));
    m_hRec[which]->setCheckable(true);
    m_hRec[which]->setStyleSheet(QStringLiteral("QPushButton:checked { background: %1; color: %2; }")
                                     .arg(adamDebugAccentFill(m_hRec[which]).name(), adamDebugAccentFillInk(m_hRec[which]).name()));
    m_hRec[which]->setToolTip(QStringLiteral("Record every instruction the CPU runs (the machine runs a little slower)"));
    connect(m_hRec[which], &QPushButton::toggled, this, [this, which](bool enabled) {
        if (m_hSync) return;
        if (which == 0) adamdebug_trace_enable(m_dbg, enabled);
        else adamdebug_history_enable(m_dbg, enabled);
        syncLater();
    });
    m_hFilter[which] = new QComboBox;
    m_hFilter[which]->addItems({ QStringLiteral("All instructions"), QStringLiteral("Branches only"),
                                 QStringLiteral("In range") });
    m_hRange[which] = new QLineEdit;
    m_hRange[which]->setPlaceholderText(QStringLiteral("range: lo-hi or a label"));
    auto changed = [this] {
        m_hSeen[0] = m_hSeen[1] = ~0ull;
        syncLater();
    };
    connect(m_hFilter[which], &QComboBox::currentIndexChanged, this, changed);
    connect(m_hRange[which], &QLineEdit::returnPressed, this, changed);
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this, which] {
        if (which == 0) adamdebug_trace_clear(m_dbg);
        else adamdebug_history_clear(m_dbg);
        m_hSeen[0] = m_hSeen[1] = ~0ull;
        syncLater();
    });
    m_hNote[which] = new QLabel;
    m_hNote[which]->setStyleSheet(kGray);
    m_hNote[which]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bar->addWidget(m_hRec[which]);
    bar->addWidget(m_hFilter[which]);
    bar->addWidget(m_hRange[which], 1);
    bar->addWidget(clear);
    bar->addWidget(m_hNote[which], 1);
    layout->addLayout(bar);
    m_hText[which] = monoView();
    layout->addWidget(m_hText[which], 1);
    return page;
}

QWidget *DebuggerWindow::buildVideo()
{
    m_video = new VideoTab(m_dbg);
    return m_video;
}

QWidget *DebuggerWindow::buildSound()
{
    m_soundText = monoView();
    return m_soundText;
}

QWidget *DebuggerWindow::buildSymbols()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *bar = new QHBoxLayout;
    bar->addWidget(new QLabel(QStringLiteral("Filter")));
    m_symFilter = new QLineEdit;
    connect(m_symFilter, &QLineEdit::textChanged, this, [this] { refreshSymbols(false); });
    bar->addWidget(m_symFilter, 1);
    m_symCount = new QLabel;
    bar->addWidget(m_symCount);
    layout->addLayout(bar);
    m_symList = new QListWidget;
    setMono(m_symList);
    m_symList->setToolTip(QStringLiteral("Enter or double-click shows it in the Memory tab"));
    connect(m_symList, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        const unsigned addr = item->data(Qt::UserRole).toUInt();
        showTab(TAB_MEMORY);
        showInMemory(uint16_t(addr), uint16_t(addr));
    });
    layout->addWidget(m_symList, 1);
    auto *foot = new QHBoxLayout;
    foot->addStretch();
    auto *memory = new QPushButton(QStringLiteral("Memory"));
    memory->setToolTip(QStringLiteral("Show the selected symbol in the Memory tab"));
    connect(memory, &QPushButton::clicked, this, [this] {
        if (QListWidgetItem *item = m_symList->currentItem()) emit m_symList->itemActivated(item);
    });
    auto *breakAt = new QPushButton(QStringLiteral("Break at"));
    breakAt->setToolTip(QStringLiteral("Toggle an execute breakpoint at the selected symbol"));
    connect(breakAt, &QPushButton::clicked, this, [this] { breakAtSymbol(); });
    foot->addWidget(memory);
    foot->addWidget(breakAt);
    layout->addLayout(foot);
    return page;
}

/* The packets left (a note and Clear over them), the cartridge's link and
 * counts right. */
QWidget *DebuggerWindow::buildFujinet()
{
    auto *split = new QSplitter(Qt::Horizontal);
    auto *left = new QWidget;
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->addWidget(titleLabel(QStringLiteral("AdamNet packets over BoIP")));
    auto *row = new QHBoxLayout;
    m_fujiNote = new QLabel;
    m_fujiNote->setStyleSheet(kGray);
    m_fujiNote->setWordWrap(true);
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        adamdebug_fujinet_trace_clear(m_dbg);
        refreshFujinet(true);
    });
    row->addWidget(m_fujiNote, 1);
    row->addWidget(clear);
    leftLayout->addLayout(row);
    m_fujiText = monoView();
    leftLayout->addWidget(m_fujiText, 1);
    left->setMinimumWidth(560);

    auto *right = new QWidget;
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->addWidget(titleLabel(QStringLiteral("BoIP link")));
    m_fujiState = monoView();
    rightLayout->addWidget(m_fujiState, 1);
    right->setMinimumWidth(320);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setChildrenCollapsible(false);
    return split;
}

/* ---- refresh -------------------------------------------------------------- */

void DebuggerWindow::refreshStatus(const adamdebug_snapshot &snap)
{
    const bool stopped = snap.paused != 0;
    const QString text = stopped ? QStringLiteral("Stopped: %1").arg(stripControl(snap.text)) : QStringLiteral("Running");
    if (m_status->text() != text) {
        m_status->setText(text);
        m_status->setToolTip(text);
        m_status->update();
    }
    m_runBtn->setText(stopped ? QStringLiteral("Run (F5)") : QStringLiteral("Stop (F5)"));
    /* the action on offer wears the accent: Run while stopped */
    m_runBtn->setStyleSheet(stopped ? QStringLiteral("background: %1; color: %2;")
                                          .arg(adamDebugAccentFill(m_runBtn).name(), adamDebugAccentFillInk(m_runBtn).name())
                                    : QString());
}

/* The refresh never overwrites a value being typed. */
void DebuggerWindow::setEntry(QLineEdit *edit, const QString &text)
{
    if (edit->hasFocus()) return;
    if (edit->text() != text) edit->setText(text);
}

void DebuggerWindow::refreshRegs()
{
    adamdebug_cpu cpu;
    adamdebug_cpu_get(m_dbg, &cpu);
    const unsigned values[15] = { unsigned(cpu.af), unsigned(cpu.bc), unsigned(cpu.de), unsigned(cpu.hl),
                                  unsigned(cpu.ix), unsigned(cpu.iy), unsigned(cpu.sp), unsigned(cpu.pc),
                                  unsigned(cpu.af2), unsigned(cpu.bc2), unsigned(cpu.de2), unsigned(cpu.hl2),
                                  unsigned(cpu.i), unsigned(cpu.r), unsigned(cpu.im) };
    for (int idx = 0; idx < 15; ++idx)
        setEntry(m_reg[idx], kRegs[idx].digits ? hexText(values[idx], kRegs[idx].digits) : QString::number(values[idx]));
    for (int idx = 0; idx < 6; ++idx) m_flag[idx]->setChecked((cpu.af & kFlags[idx].mask) != 0);
    m_iff[0]->setChecked(cpu.iff1 != 0);
    m_iff[1]->setChecked(cpu.iff2 != 0);
    m_haltNote->setText(cpu.halted ? QStringLiteral("HALT") : QString());
}

/* The dump from m_memTop, each row labelled once with the region it is in (the
 * map as of the last refreshSlow); a highlighted range (Go to, a watch, a
 * search hit) in the accent. */
void DebuggerWindow::refreshMem()
{
    static uint8_t mem[MEM_ROWS * 16];
    memset(mem, 0, sizeof mem);
    adamdebug_read(m_dbg, (uint16_t)m_memTop, mem, (int)sizeof mem);

    QString text;
    text.reserve(MEM_ROWS * 90);
    text += QStringLiteral("       ");
    for (int col = 0; col < 16; ++col) text += hexText((unsigned)col, 1) + QStringLiteral("  ");
    char region[64];
    QString lastRegion;
    for (int row = 0; row < MEM_ROWS; ++row) {
        const uint8_t *bytes = mem + row * 16;
        const unsigned addr = (unsigned)(m_memTop + row * 16);
        text += QStringLiteral("\n$%1: ").arg(hexText(addr, 4));
        for (int col = 0; col < 16; ++col) text += hexText(bytes[col], 2) + QLatin1Char(' ');
        text += QLatin1Char(' ');
        for (int col = 0; col < 16; ++col) text += QLatin1Char(bytes[col] >= 0x20 && bytes[col] < 0x7F ? (char)bytes[col] : '.');
        if (!m_regions.empty() && adammem_region_name(m_regions.data(), (int)m_regions.size(), (uint16_t)addr, region, sizeof region) > 0) {
            const QString name = QString::fromUtf8(region);
            if (name != lastRegion) text += QStringLiteral("  ; ") + name;
            lastRegion = name;
        }
    }
    setText(m_mem, text);

    /* the bytes Go to found stay marked across refreshes */
    QList<QTextEdit::ExtraSelection> sel;
    if (m_memHl) {
        for (int row = 0; row < MEM_ROWS; ++row) {
            const QTextBlock block = m_mem->document()->findBlockByNumber(1 + row);
            if (!block.isValid()) break;
            for (int col = 0; col < 16; ++col) {
                const int addr = m_memTop + row * 16 + col;
                if (addr < m_memHlLo || addr > m_memHlHi) continue;
                QTextEdit::ExtraSelection mark;
                mark.cursor = QTextCursor(m_mem->document());
                mark.cursor.setPosition(block.position() + 7 + col * 3);
                mark.cursor.setPosition(block.position() + 7 + col * 3 + 2, QTextCursor::KeepAnchor);
                mark.format = accentFormat(m_mem);
                sel << mark;
            }
        }
    }
    m_mem->setExtraSelections(sel);
}

/* The disassembly lines that fit the view. */
int DebuggerWindow::visibleRows() const
{
    const int line = m_disasm->fontMetrics().lineSpacing();
    const int height = m_disasm->viewport()->height() - 2 * (int)m_disasm->document()->documentMargin();
    if (line <= 0 || height <= 0) return 32;
    return qBound(4, height / line, DISASM_WINDOW);
}

void DebuggerWindow::refreshDisasm()
{
    static adamdebug_line lines[DISASM_WINDOW];
    const int rows = visibleRows();
    if (m_followPc->isChecked()) {
        adamdebug_cpu cpu;
        adamdebug_cpu_get(m_dbg, &cpu);
        /* the PC a third of the way down */
        m_disasmTop = (uint16_t)adamdebug_row_address(m_dbg, (uint16_t)cpu.pc, -(rows / 3));
    }
    int pcLine = -1;
    const int count = adamdebug_disassemble(m_dbg, m_disasmTop, lines, rows, &pcLine);
    /* the label column is as wide as the longest label shown, within reason */
    int labelWidth = 0;
    for (int row = 0; row < count; ++row) labelWidth = qMax(labelWidth, (int)strlen(lines[row].label));
    labelWidth = qMin(labelWidth, 24);
    m_lineAddr.clear();
    QString text;
    char buf[256];
    for (int row = 0; row < count; ++row) {
        m_lineAddr.push_back(lines[row].address);
        char label[48];
        snprintf(label, sizeof label, "%.*s", labelWidth, lines[row].label);
        /* the gutter: a breakpoint dot, then the PC's arrow */
        snprintf(buf, sizeof buf, "%c %04X  %-11.11s %-*s %-22s", lines[row].is_pc ? '>' : ' ', lines[row].address,
                 lines[row].bytes, labelWidth, label, lines[row].disasm);
        if (row) text += QLatin1Char('\n');
        text += lines[row].has_breakpoint ? QStringLiteral("●") : QStringLiteral(" ");
        text += QString::fromUtf8(buf);
        if (lines[row].comment[0]) text += QStringLiteral("; ") + QString::fromUtf8(lines[row].comment);
    }
    if (count == 0) text = QStringLiteral("(no disassembly: no machine is running)");
    m_disasm->setPlainText(stripControl(text.toUtf8().constData()));
    m_disasm->verticalScrollBar()->setValue(0);

    QList<QTextEdit::ExtraSelection> sel;
    for (int row = 0; row < count; ++row) {
        /* the gutter: a breakpoint red, a disabled one grey */
        if (lines[row].has_breakpoint) {
            QTextEdit::ExtraSelection mark;
            mark.cursor = QTextCursor(m_disasm->document()->findBlockByNumber(row));
            mark.cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
            if (lines[row].breakpoint_enabled) {
                mark.format.setForeground(QColor(0xe0, 0x1b, 0x24));
                mark.format.setFontWeight(QFont::Bold);
            } else {
                mark.format.setForeground(QColor(0x9a, 0x99, 0x96));
            }
            sel << mark;
        }
        /* the selected line underlined */
        if (lines[row].address == m_selected && row != pcLine) {
            QTextEdit::ExtraSelection mark;
            mark.cursor = QTextCursor(m_disasm->document()->findBlockByNumber(row));
            mark.cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            mark.format.setFontUnderline(true);
            mark.format.setFontWeight(QFont::Bold);
            sel << mark;
        }
    }
    /* the PC's line in the accent colour, last so its ink wins */
    if (pcLine >= 0) {
        QTextCharFormat pcFormat = accentFormat(m_disasm);
        if (lines[pcLine].address == m_selected) pcFormat.setFontUnderline(true);
        sel << lineSelection(m_disasm, pcLine, pcFormat);
    }
    m_disasm->setExtraSelections(sel);
}

void DebuggerWindow::refreshBps()
{
    static adamdebug_breakpoint bps[BP_MAX];
    const int count = adamdebug_breakpoint_list(m_dbg, bps, BP_MAX);

    /* what stopped the machine, in the core's words */
    adamdebug_snapshot snap;
    adamdebug_stop_snapshot(m_dbg, &snap);
    m_bpStop->setText(snap.paused ? QStringLiteral("Stopped: %1").arg(stripControl(snap.text)) : QStringLiteral("Running."));

    /* rebuilt only when it changed (each step refreshes), and never while a
     * row's own button is being handled */
    QString sig;
    for (int idx = 0; idx < count; ++idx)
        sig += QStringLiteral("%1 %2 %3 %4 %5 %6 %7\n").arg(bps[idx].id).arg(bps[idx].enabled).arg(bps[idx].hits)
                   .arg(bps[idx].type).arg(bps[idx].start).arg(bps[idx].end).arg(QString::fromUtf8(bps[idx].condition));
    if (sig == m_bpSig && m_bpTable->rowCount() == count) return;
    m_bpSig = sig;

    QFont mono = m_bpTable->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    m_bpFilling = true;
    m_bpTable->setRowCount(count);
    for (int idx = 0; idx < count; ++idx) {
        auto *on = new QTableWidgetItem;
        on->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        on->setCheckState(bps[idx].enabled ? Qt::Checked : Qt::Unchecked);
        on->setData(Qt::UserRole, bps[idx].id);
        on->setToolTip(QStringLiteral("Enabled"));
        m_bpTable->setItem(idx, 0, on);
        QStringList type;
        if (bps[idx].type == ADAMDEBUG_BP_ACCESS) {
            type << QStringLiteral("access");
        } else {
            if (bps[idx].type & ADAMDEBUG_BP_EXEC) type << QStringLiteral("exec");
            if (bps[idx].type & ADAMDEBUG_BP_READ) type << QStringLiteral("read");
            if (bps[idx].type & ADAMDEBUG_BP_WRITE) type << QStringLiteral("write");
            if (bps[idx].type & ADAMDEBUG_BP_IN) type << QStringLiteral("in");
            if (bps[idx].type & ADAMDEBUG_BP_OUT) type << QStringLiteral("out");
        }
        auto *typeItem = new QTableWidgetItem(type.join(QLatin1Char('/')));
        char desc[256];
        adambp_describe(m_dbg, &bps[idx], desc, sizeof desc);
        typeItem->setToolTip(stripControl(desc));
        m_bpTable->setItem(idx, 1, typeItem);
        /* a port range for in/out is two digits */
        const int digits = (bps[idx].type & (ADAMDEBUG_BP_IN | ADAMDEBUG_BP_OUT)) ? 2 : 4;
        QString range = QStringLiteral("$") + hexText(bps[idx].start, digits);
        if (bps[idx].end != bps[idx].start) range += QStringLiteral("-$") + hexText(bps[idx].end, digits);
        m_bpTable->setItem(idx, 2, new QTableWidgetItem(range));
        auto *condItem = new QTableWidgetItem(stripControl(bps[idx].condition));
        condItem->setData(Qt::UserRole, bps[idx].id);
        condItem->setToolTip(QStringLiteral("Double-click to change the condition (empty: stop every time)"));
        m_bpTable->setItem(idx, 3, condItem);
        m_bpTable->setItem(idx, 4, new QTableWidgetItem(QString::number(bps[idx].hits)));
        for (int col = 1; col < 5; ++col) {
            m_bpTable->item(idx, col)->setFont(mono);
            if (col != 3) m_bpTable->item(idx, col)->setFlags(Qt::ItemIsEnabled);
        }
        auto *removeBtn = new QToolButton;
        removeBtn->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete"), QIcon::fromTheme(QStringLiteral("user-trash"))));
        if (removeBtn->icon().isNull()) removeBtn->setText(QStringLiteral("Remove"));
        removeBtn->setAutoRaise(true);
        removeBtn->setToolTip(QStringLiteral("Remove"));
        const int bpid = bps[idx].id;
        connect(removeBtn, &QToolButton::clicked, this, [this, bpid] {
            adamdebug_breakpoint_remove(m_dbg, bpid);
            syncLater();
        });
        m_bpTable->setCellWidget(idx, 5, removeBtn);
    }
    m_bpFilling = false;
    m_bpEmpty->setVisible(count == 0);
}

void DebuggerWindow::refreshWatches()
{
    const int count = adamwatch_count();
    const int keep = m_wtList->currentRow();
    QStringList rows;
    for (int idx = 0; idx < count; ++idx) {
        char line[256];
        adamwatch_format(m_dbg, idx, line, sizeof line);
        rows << stripControl(line);
    }
    /* unchanged: leave the list alone (a click in it must not be lost) */
    bool same = m_wtList->count() == count;
    for (int idx = 0; same && idx < count; ++idx) same = m_wtList->item(idx)->text() == rows[idx];
    if (same) return;
    m_wtList->clear();
    m_wtList->addItems(rows);
    if (keep >= 0 && keep < count) m_wtList->setCurrentRow(keep);
}

/* The machine, its memory map and its I/O, from the engine's getters (valid
 * while stopped; a running machine shows what the machine last published). The
 * cartridge text is the engine's summary; the FujiNet cartridge's own link and
 * mailbox state is on the FujiNet tab. */
void DebuggerWindow::refreshCart()
{
    static char text[8192];
    adamdebug_cart cart;
    adamdebug_region map[16];
    QString top;

    adamdebug_snapshot snap;
    adamdebug_stop_snapshot(m_dbg, &snap);
    m_cartSerial = snap.serial;
    adamdebug_cart_info(m_dbg, &cart);
    if (!cart.valid) {
        setText(m_cartText, QStringLiteral("No machine running."));
        setText(m_ioText, QStringLiteral("No machine running."));
        return;
    }
    text[0] = '\0';
    adamdebug_cart_summary(m_dbg, text, int(sizeof text));
    top += QStringLiteral("Machine\n  %1\n").arg(QString::fromUtf8(text));
    top += QStringLiteral("\nMemory map  $7F=%1  $3F=%2\n").arg(hexText(cart.mem_ctrl, 2), hexText(cart.net_ctrl, 2));
    const int regions = adamdebug_memory_map(m_dbg, map, 16);
    if (!regions) top += QStringLiteral("  Stop the machine to read its memory map.\n");
    for (int idx = 0; idx < regions; ++idx)
        top += QStringLiteral("  %1-%2  %3%4  %5\n")
                   .arg(hexText(map[idx].low, 4), hexText(map[idx].high, 4))
                   .arg(map[idx].readable ? QLatin1Char('r') : QLatin1Char('-'))
                   .arg(map[idx].writable ? QLatin1Char('w') : QLatin1Char('-'))
                   .arg(QString::fromUtf8(map[idx].name));
    setText(m_cartText, stripControl(top.toUtf8().constData()));

    text[0] = '\0';
    adamdebug_io_state(m_dbg, text, int(sizeof text));
    /* the engine's own memory map block repeats the list above: left out here */
    QStringList kept;
    bool inMap = false;
    for (const QString &line : stripControl(text).split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1String("Memory map"))) inMap = true;
        else if (inMap && !line.startsWith(QLatin1Char(' '))) inMap = false;
        if (!inMap) kept << line;
    }
    setText(m_ioText, text[0] ? kept.join(QLatin1Char('\n')) : QStringLiteral("No machine running."));
}

/* Trace (which 0: the newest instructions, live) and History (1: what ran
 * before the stop). Both read the one ring; the filter and the note are the
 * tab's own. */
void DebuggerWindow::refreshTrace(int which, bool force)
{
    static adamdebug_trace buf[HIST_FETCH];
    const bool live = which == 0;
    const bool stopped = adamdebug_is_stopped(m_dbg) != 0;
    const bool traceOn = adamdebug_trace_enabled(m_dbg) != 0, histOn = adamdebug_history_enabled(m_dbg) != 0;
    const bool recording = live ? traceOn : histOn;
    const uint64_t total = live ? adamdebug_trace_total(m_dbg) : adamdebug_history_count(m_dbg);
    uint16_t first = 0, last = 0xFFFF;
    int filter = ADAMDEBUG_TRACE_ALL;

    m_hSync = true;      /* the two Record buttons follow the engine */
    m_hRec[0]->setChecked(traceOn);
    m_hRec[1]->setChecked(histOn);
    m_hSync = false;

    if (!force && total == m_hSeen[which] && (live || stopped)) return;
    m_hSeen[which] = total;
    if (!live && !stopped) {
        m_hNote[which]->setText(QStringLiteral("Stop the machine to read its history."));
        return;
    }
    switch (m_hFilter[which]->currentIndex()) {
    case 1: filter = ADAMDEBUG_TRACE_BRANCHES; break;
    case 2:
        if (adamsym_parse_range(m_dbg, m_hRange[which]->text().toUtf8().constData(), &first, &last)) filter = ADAMDEBUG_TRACE_RANGE;
        break;
    default: break;
    }
    QList<int> hit;
    int fetched = 0;
    if (live) {
        fetched = adamdebug_trace_read(m_dbg, buf, HIST_FETCH);                /* newest first */
        for (int idx = 0; idx < fetched && hit.size() < HIST_SHOWN; ++idx)
            if (adamdebug_trace_match(&buf[idx], filter, first, last)) hit.append(idx);
    } else {
        fetched = adamdebug_history_copy(m_dbg, buf, HIST_FETCH);              /* oldest first */
        for (int idx = fetched - 1; idx >= 0 && hit.size() < HIST_SHOWN; --idx)   /* the newest matches, scanning back */
            if (adamdebug_history_match(&buf[idx], filter, first, last)) hit.append(idx);
    }
    QString text;
    if (!recording && fetched == 0)
        text = QStringLiteral("%1 is off. Press Record, then let the machine run.").arg(live ? QStringLiteral("Trace") : QStringLiteral("History"));
    for (int pos = hit.size() - 1; pos >= 0; --pos) {      /* oldest first */
        char line[320];
        if (live) adamdebug_trace_format(&buf[hit[pos]], line, sizeof line);
        else adamdebug_history_format(&buf[hit[pos]], line, sizeof line);
        text += stripControl(line);
        text += QLatin1Char('\n');
    }
    setText(m_hText[which], text, true);
    m_hNote[which]->setText(QStringLiteral("%1%2 instructions recorded, last %3 shown")
                                .arg(recording ? QStringLiteral("Recording. ") : QString()).arg(total).arg(hit.size()));
}

void DebuggerWindow::refreshVideo(bool force)
{
    if (m_video) m_video->refresh(force || m_videoForce);
    m_videoForce = false;
}

/* The SN76489, and the AY-3-8910 of a Super Game Module, from the engine's
 * emulator-thread mirrors (stopped only). */
void DebuggerWindow::refreshSound()
{
    static adamdebug_sound snd;
    if (!adamdebug_is_stopped(m_dbg)) {
        setText(m_soundText, QStringLiteral("Stop the machine to read the sound chips."));
        return;
    }
    QString text;
    if (!adamdebug_sound_get(m_dbg, &snd)) {
        setText(m_soundText, QStringLiteral("No machine running."));
        return;
    }
    if (snd.sn.present) {
        const adamdebug_sn &chip = snd.sn;
        text += QStringLiteral("SN76489 sound generator (write-only, ports $E0-$FF)\n");
        for (int voice = 0; voice < 3; ++voice)
            text += QStringLiteral("  Tone %1  period %2  attenuation %3 (level %4)  %5\n")
                        .arg(voice + 1).arg(chip.period[voice], 4).arg(chip.atten[voice], 2).arg(chip.level[voice], 2)
                        .arg(chip.level[voice] > 0 ? noteName(chip.freq[voice]) : QStringLiteral("-"));
        text += QStringLiteral("  Noise   %1, %2  attenuation %3 (level %4)  %5\n")
                    .arg(chip.noise_white ? QStringLiteral("white") : QStringLiteral("periodic"),
                         chip.noise_rate == 3 ? QStringLiteral("rate follows tone 3")
                                              : QStringLiteral("rate %1").arg(chip.noise_rate))
                    .arg(chip.atten[3], 2).arg(chip.level[3], 2)
                    .arg(chip.level[3] > 0 ? QStringLiteral("%1 Hz").arg(chip.noise_hz, 0, 'f', 1) : QStringLiteral("-"));
        text += QStringLiteral("  Latched register %1; attenuation is 2 dB a step, 15 is silent\n").arg(chip.latched_reg);
    }
    if (snd.ay.present) {
        const adamdebug_ay &chip = snd.ay;
        text += QStringLiteral("\nAY-3-8910 of the Super Game Module ($50 selects a register, $51 writes it, $52 reads it)\n");
        for (int voice = 0; voice < 3; ++voice)
            text += QStringLiteral("  %1  period %2  tone %3  noise %4  level %5%6  %7\n")
                        .arg(QLatin1Char(char('A' + voice)))
                        .arg(chip.period[voice], 4)
                        .arg(onOff(chip.tone_on[voice]), onOff(chip.noise_on[voice]))
                        .arg(chip.level[voice], 2)
                        .arg(chip.envelope[voice] ? QStringLiteral(" (envelope)") : QString())
                        .arg(chip.tone_on[voice] && chip.period[voice] > 0 ? noteName(chip.freq[voice]) : QStringLiteral("-"));
        text += QStringLiteral("  Noise period %1   Envelope period %2, shape %3\n")
                    .arg(chip.noise_period).arg(chip.env_period).arg(hexText(unsigned(chip.env_shape), 1));
        text += QStringLiteral("  Registers:");
        for (int reg = 0; reg < 16; ++reg) text += QStringLiteral(" R%1=%2").arg(hexText(unsigned(reg), 1), hexText(chip.reg[reg], 2));
        text += QStringLiteral("\n  (the mixer, R7, is active-low: a set bit turns that voice off)\n");
    } else {
        text += QStringLiteral("\nNot on this machine: AY-3-8910 (no Super Game Module)\n");
    }
    setText(m_soundText, text);
}

/* Only rebuilt when the list or the filter changed: a real game .sym can be
 * a few hundred entries. force: whatever the tab shows, on first open too. */
void DebuggerWindow::refreshSymbols(bool force)
{
    const unsigned generation = adamdebug_symbols_generation(m_dbg);
    const QString filter = m_symFilter->text();
    if (!force && generation == m_symGen && filter == m_symShownFilter) return;
    m_symGen = generation;
    m_symShownFilter = filter;
    const int total = adamdebug_symbols_count(m_dbg);
    std::vector<int> found((size_t)qMax(total, 1));
    const int shown = adamdebug_symbols_filter(m_dbg, filter.toUtf8().constData(), found.data(), (int)found.size());
    m_symList->clear();
    for (int idx = 0; idx < shown; ++idx) {
        uint16_t addr;
        char name[96];
        if (!adamdebug_symbols_get(m_dbg, found[(size_t)idx], &addr, name, sizeof name)) continue;
        auto *item = new QListWidgetItem(QStringLiteral("%1  %2").arg(hexText(addr, 4), QString::fromUtf8(name)));
        item->setData(Qt::UserRole, addr);
        m_symList->addItem(item);
    }
    m_symCount->setText(QStringLiteral("%1 symbol%2%3").arg(shown).arg(shown == 1 ? QString() : QStringLiteral("s"))
                            .arg(filter.isEmpty() ? QString() : QStringLiteral(" (filtered)")));
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
void DebuggerWindow::refreshFujinet(bool force)
{
    static adam_fuji_packet packets[TRACE_MAX];
    static char state[4096];
    const int count = adamdebug_fujinet_trace(m_dbg, packets, TRACE_MAX);
    if (!adamsession_fujinet_running(m_session) && !adamsession_fujinet_external(m_session))
        m_fujiNote->setText(QStringLiteral("FujiNet is not running (FujiNet ▸ Console Log)."));
    else
        m_fujiNote->setText(QStringLiteral("%1 FujiNet on BoIP %2, %3.")
                                .arg(adamsession_fujinet_external(m_session) ? QStringLiteral("External")
                                                                             : QStringLiteral("Built-in"))
                                .arg(QString::fromUtf8(adamsession_boip_endpoint(m_session)))
                                .arg(adamsession_fujinet_link(m_session) ? QStringLiteral("connected")
                                                                         : QStringLiteral("not connected")));
    state[0] = '\0';
    adamdebug_fujinet_state(m_dbg, state, int(sizeof state));
    setText(m_fujiState, stripControl(state));

    if (!force && count == m_fujiCount && (count == 0 || packets[count - 1].serial == m_fujiLast)) return;
    const bool follow = force || m_fujiCount < 0 || atEnd(m_fujiText);
    m_fujiCount = count;
    m_fujiLast = count > 0 ? packets[count - 1].serial : 0;
    QString text;
    for (int idx = 0; idx < count; ++idx) {
        if (idx) text += QLatin1Char('\n');
        text += QString::fromUtf8(packets[idx].text);
        if (packets[idx].repeat > 1) text += QStringLiteral(" ×%1").arg(packets[idx].repeat);
        text += QStringLiteral("\n     ");
        for (int byte = 0; byte < packets[idx].nbytes && byte < int(sizeof packets[idx].data); byte++) text += hexText(packets[idx].data[byte], 2) + QLatin1Char(' ');
        if (packets[idx].nbytes > int(sizeof packets[idx].data)) text += QStringLiteral("…");
    }
    if (count == 0) text = QStringLiteral("(no AdamNet packets since the trace was cleared)");
    setText(m_fujiText, text, follow);
}

/* One tab; live is the twice-a-second refresh while the machine runs, which
 * leaves out what only changes at a stop. */
void DebuggerWindow::refreshTab(int tab, bool live)
{
    switch (tab) {
    case TAB_DISASM: if (!live) refreshDisasm(); break;
    case TAB_MEMORY: if (!live) refreshRegions(); refreshMem(); break;
    case TAB_BREAKS: refreshBps(); break;
    case TAB_WATCHES: refreshWatches(); break;
    case TAB_CART: if (!live) refreshCart(); break;
    case TAB_TRACE: refreshTrace(0, !live); break;
    case TAB_HISTORY: if (!live) refreshTrace(1, true); break;
    case TAB_VIDEO: refreshVideo(!live); break;
    case TAB_SOUND: if (!live) refreshSound(); break;
    case TAB_SYMBOLS: refreshSymbols(false); break;     /* it fills when its generation first differs */
    case TAB_FUJINET: refreshFujinet(false); break;
    default: break;
    }
}

/* The memory map labels the Memory rows: read for the stop that is current. */
void DebuggerWindow::refreshRegions()
{
    adamdebug_snapshot snap;
    adamdebug_stop_snapshot(m_dbg, &snap);
    if (!m_regions.empty() && m_regionSerial == snap.serial && snap.paused) return;
    adamdebug_region map[16];
    const int count = adamdebug_memory_map(m_dbg, map, 16);
    m_regions.assign(map, map + qMax(0, count));
    m_regionSerial = snap.serial;
}

/* Every tab that is on screen (the one showing, and any peeled off). */
void DebuggerWindow::refreshAll()
{
    adamdebug_snapshot snap;
    adamdebug_stop_snapshot(m_dbg, &snap);
    refreshStatus(snap);
    refreshRegs();
    for (int tab = 0; tab < TAB_COUNT; ++tab)
        if (tabVisible(tab)) refreshTab(tab, false);
}

/* After anything that may have moved the engine: the console notes a new
 * stop, then the tabs showing follow. */
void DebuggerWindow::resync()
{
    adamdebug_snapshot snap;
    adamdebug_stop_snapshot(m_dbg, &snap);
    /* a step is its own answer; anything else that stopped the machine is news */
    if (snap.paused && snap.serial != m_seenSerial && snap.stop.reason != ADAMDEBUG_STOP_STEP && snap.text[0])
        consoleAppend(QStringLiteral("— %1\n").arg(stripControl(snap.text)));
    m_wasStopped = snap.paused != 0;
    m_seenSerial = snap.serial;
    m_seenGen = adamdebug_generation(m_dbg);
    m_runningTicks = 0;
    refreshAll();
}

/* From inside a breakpoint row's own signal: the table rebuilds after. */
void DebuggerWindow::syncLater()
{
    QTimer::singleShot(0, this, [this] { resync(); });
}

/* A step takes microseconds: a moment's wait for the stop keeps the window
 * from flashing "Running" in between. */
void DebuggerWindow::settleAndSync()
{
    for (int tries = 0; tries < 40 && !adamdebug_is_stopped(m_dbg); ++tries) QThread::msleep(1);
    resync();
}

/* The window follows the engine through the stop snapshot (the stop callback
 * fires on the emulator thread), so this polls it: a refresh when it moved,
 * and the live values while the machine runs. */
void DebuggerWindow::tick()
{
    if (!isVisible()) return;
    adamdebug_snapshot snap;
    adamdebug_stop_snapshot(m_dbg, &snap);
    const unsigned gen = adamdebug_generation(m_dbg);
    const bool stopped = snap.paused != 0;
    if (snap.serial != m_seenSerial || gen != m_seenGen || stopped != m_wasStopped) {
        resync();
        return;
    }
    if (stopped) return;
    ++m_runningTicks;
    /* a running machine publishes a frame, and the ring a record, all the time */
    if (m_runningTicks % 2 == 0 && tabVisible(TAB_VIDEO)) refreshVideo(false);
    if (m_runningTicks % 4 == 0 && tabVisible(TAB_TRACE)) refreshTrace(0, false);
    if (m_runningTicks >= 5) {
        m_runningTicks = 0;
        refreshStatus(snap);
        refreshRegs();
        for (int tab = 0; tab < TAB_COUNT; ++tab)
            if (tabVisible(tab) && tab != TAB_VIDEO && tab != TAB_TRACE) refreshTab(tab, true);
    }
}

/* ---- the console ---------------------------------------------------------- */

/* Onto the transcript, which follows its end while the reader is there. */
void DebuggerWindow::consoleAppend(const QString &text)
{
    const bool follow = atEnd(m_conOut);
    QTextCursor cur(m_conOut->document());
    cur.movePosition(QTextCursor::End);
    cur.insertText(text);
    if (follow) m_conOut->verticalScrollBar()->setValue(m_conOut->verticalScrollBar()->maximum());
}

void DebuggerWindow::runConsole()
{
    static char out[65536];
    const QString cmd = m_conIn->text().trimmed();
    if (cmd.isEmpty()) return;
    if (m_history.isEmpty() || m_history.last() != cmd) m_history << cmd;
    m_historyPos = (int)m_history.size();
    /* a command brings the reader back down */
    m_conOut->verticalScrollBar()->setValue(m_conOut->verticalScrollBar()->maximum());
    consoleAppend(QStringLiteral("> %1\n").arg(cmd));
    m_conIn->clear();

    out[0] = '\0';
    adamdebug_command(m_dbg, cmd.toUtf8().constData(), out, sizeof out);
    QString text = stripControl(out);
    if (!text.isEmpty()) consoleAppend(text.endsWith(QLatin1Char('\n')) ? text : text + QLatin1Char('\n'));
    settleAndSync();
}

/* Tab: the engine completes the word at the end of the line. One match
 * replaces it; several are listed and the line extends to what they share. */
void DebuggerWindow::completeConsole()
{
    char comps[4096];
    const QString text = m_conIn->text();
    comps[0] = '\0';
    const int count = adamdebug_completions(m_dbg, text.toUtf8().constData(), comps, sizeof comps);
    if (count <= 0) return;
    const QStringList words = stripControl(comps).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (words.isEmpty()) return;
    const int space = text.lastIndexOf(QLatin1Char(' '));
    const QString head = text.left(space + 1);
    if (words.size() == 1) {
        m_conIn->setText(head + words[0] + QLatin1Char(' '));
        return;
    }
    QString common = words[0];
    for (const QString &word : words)
        while (!word.startsWith(common)) common.chop(1);
    consoleAppend(words.join(QStringLiteral("  ")) + QLatin1Char('\n'));
    if (common.size() > text.size() - space - 1) m_conIn->setText(head + common);
}

/* Messages land in the console and, since it may not be the tab showing,
 * the status bar. */
void DebuggerWindow::say(const QString &msg)
{
    consoleAppend(msg + QLatin1Char('\n'));
    message(msg);
}

void DebuggerWindow::message(const QString &msg)
{
    statusBar()->showMessage(msg, 6000);
}

/* ---- the rest of the handlers ----------------------------------------------- */

void DebuggerWindow::toggleRun()
{
    if (adamdebug_is_stopped(m_dbg)) {
        adamdebug_resume(m_dbg);
        resync();
    } else {
        adamdebug_stop(m_dbg);
        settleAndSync();
    }
}

void DebuggerWindow::stepAnd(void (*func)(adamdebug *))
{
    func(m_dbg);
    settleAndSync();
}

void DebuggerWindow::runToCursor()
{
    if (m_selected < 0) {
        message(QStringLiteral("Select a line in the Disassembly to run to"));
        return;
    }
    adamdebug_run_to(m_dbg, (uint16_t)m_selected);
    settleAndSync();
}

void DebuggerWindow::toggleBreakpoint(int addr)
{
    adamdebug_breakpoint_toggle(m_dbg, (uint16_t)addr);
    resync();
}

void DebuggerWindow::toggleSelected()
{
    if (m_selected < 0) {
        message(QStringLiteral("Select a line in the Disassembly first"));
        return;
    }
    toggleBreakpoint(m_selected);
}

/* A number the way the prompt takes it: a bare token of hex digits is hex (as
 * the register and value entries always were), anything else goes to the
 * engine's expression evaluator ($8000, 49152, a label, hl + 1 ...). */
bool DebuggerWindow::numberOf(const QString &text, long *out, QString *why)
{
    static const QRegularExpression hexOnly(QStringLiteral("^[0-9A-Fa-f]{1,8}$"));
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        if (why) *why = QStringLiteral("Type a value");
        return false;
    }
    if (hexOnly.match(trimmed).hasMatch()) {
        *out = trimmed.toLong(nullptr, 16);
        return true;
    }
    char err[160] = "";
    if (adamdebug_eval(m_dbg, trimmed.toUtf8().constData(), out, err, int(sizeof err)) == 0) return true;
    if (why) *why = stripControl(err);
    return false;
}

/* A label, or a number as numberOf reads it. */
bool DebuggerWindow::addressOf(const QString &text, long *out)
{
    const int lbl = adamdebug_label_address(m_dbg, text.trimmed().toUtf8().constData());
    if (lbl >= 0) { *out = lbl; return true; }
    return numberOf(text, out, nullptr);
}

/* One register or flag through the engine's grammar: it says if it will not
 * take the value (running, IM above 2), and may add a note (a PC written while
 * the CPU is in HALT leaves it halted). */
void DebuggerWindow::applyRegister(const char *name, long value, const QString &okMessage)
{
    char err[200] = "";
    if (adamdebug_register_set(m_dbg, name, value, err, sizeof err))
        message(err[0] ? okMessage + QStringLiteral(": ") + stripControl(err) : okMessage);
    else
        message(err[0] ? stripControl(err) : QStringLiteral("The register could not be written"));
}

/* Edits go in only while the machine is stopped. */
void DebuggerWindow::setReg(int idx)
{
    long val = 0;
    QString why;
    QLineEdit *edit = m_reg[idx];
    if (!adamdebug_is_stopped(m_dbg)) {
        message(QStringLiteral("Stop the machine first"));
    } else if (!numberOf(edit->text(), &val, &why)) {
        message(why);
    } else {
        /* a CPU in HALT stays halted when its PC is written: the engine's note says so */
        applyRegister(kRegs[idx].engine, val,
                      idx == REG_PC ? QStringLiteral("PC set") : QStringLiteral("%1 set").arg(QString::fromUtf8(kRegs[idx].name)));
    }
    edit->clearFocus();
    resync();
}

void DebuggerWindow::setFlag(int idx, bool enabled)
{
    if (!adamdebug_is_stopped(m_dbg)) message(QStringLiteral("Stop the machine first"));
    else applyRegister(kFlags[idx].engine, enabled ? 1 : 0, QStringLiteral("Flag set"));
    resync();
}

void DebuggerWindow::setIff(int which, bool enabled)
{
    if (!adamdebug_is_stopped(m_dbg)) message(QStringLiteral("Stop the machine first"));
    else applyRegister(which ? "iff2" : "iff1", enabled ? 1 : 0, QStringLiteral("IFF%1 set").arg(which + 1));
    resync();
}

/* Shows a range in the Memory tab, highlighted, on a row boundary and a
 * little above the middle. */
void DebuggerWindow::showInMemory(uint16_t first, uint16_t last)
{
    m_memHl = true;
    m_memHlLo = first;
    m_memHlHi = last;
    m_memTop = qBound(0, (int)((first > 0x100 ? first - 0x100 : 0) & ~0xF), 0x10000 - MEM_ROWS * 16);
    refreshMem();
    const int line = 1 + (first - m_memTop) / 16;
    const int shown = m_mem->viewport()->height() / qMax(1, m_mem->fontMetrics().lineSpacing());
    m_mem->verticalScrollBar()->setValue(qMax(0, line - shown * 3 / 10));
}

/* Go to: an address, a label, an expression or lo-hi. */
void DebuggerWindow::memGoto()
{
    uint16_t first, last;
    if (adamsym_parse_range(m_dbg, m_memGoto->text().toUtf8().constData(), &first, &last)) {
        showInMemory(first, last);
    } else {
        m_memHl = false;
        refreshMem();
        message(QStringLiteral("Not an address ($8000, a label or first-last)"));
    }
}

void DebuggerWindow::memPoke()
{
    long addr, val;
    QString why;
    if (!adamdebug_is_stopped(m_dbg)) {
        message(QStringLiteral("Stop the machine first"));
        return;
    }
    if (!addressOf(m_memAddr->text(), &addr) || addr < 0 || addr > 0xFFFF) {
        message(QStringLiteral("Not an address"));
        return;
    }
    if (!numberOf(m_memVal->text(), &val, &why) || val < 0 || val > 0xFF) {
        message(why.isEmpty() ? QStringLiteral("The value is a byte ($00-$FF)") : why);
        return;
    }
    const uint8_t byte = uint8_t(val);
    adamdebug_write(m_dbg, (uint16_t)addr, byte);
    message(QStringLiteral("Wrote $%1 to $%2").arg(hexText((unsigned)val, 2), hexText((unsigned)addr, 4)));
    resync();
}

/* Find Next / Previous: from the highlighted bytes, or the top of the dump. */
void DebuggerWindow::memSearch(bool backward)
{
    uint8_t pat[ADAMSEARCH_MAX], mask;
    int count = 0, flags = 0;
    uint16_t found;
    if (!adamsearch_parse(m_srchKind->currentIndex(), m_srchEntry->text().toUtf8().constData(), pat, &count, &mask, &flags)) {
        m_srchNote->setText(QStringLiteral("Type hex bytes (3E 01) or some text."));
        return;
    }
    if (!adamdebug_is_stopped(m_dbg)) {
        m_srchNote->setText(QStringLiteral("Stop the machine to search its memory."));
        return;
    }
    const uint16_t from = m_memHl ? (backward ? m_memHlLo : uint16_t(m_memHlLo + 1)) : (uint16_t)m_memTop;
    const int hit = adamdebug_search_next(m_dbg, pat, count, mask, flags, from, backward ? 1 : 0, &found);
    if (hit > 0) {
        m_srchNote->setText(QStringLiteral("Found at $%1").arg(hexText(found, 4)));
        showInMemory(found, uint16_t(found + count - 1));
    } else {
        m_srchNote->setText(hit == 0 ? QStringLiteral("Not found.") : QStringLiteral("Stop the machine to search its memory."));
    }
}

void DebuggerWindow::jumpTo()
{
    long address;
    if (!addressOf(m_jump->text(), &address) || address < 0 || address > 0xFFFF) {
        message(QStringLiteral("No such address or label"));
        return;
    }
    m_followPc->setChecked(false);
    m_selected = (int)address;
    m_disasmTop = (uint16_t)adamdebug_row_address(m_dbg, (uint16_t)address, -(visibleRows() / 3));
    refreshDisasm();
}

/* The type takes an address or label; in and out a port. The end is
 * optional; the condition is an expression the engine checks. */
void DebuggerWindow::addBreakpoint()
{
    static const int types[6] = { ADAMDEBUG_BP_EXEC, ADAMDEBUG_BP_READ, ADAMDEBUG_BP_WRITE, ADAMDEBUG_BP_ACCESS,
                                  ADAMDEBUG_BP_IN, ADAMDEBUG_BP_OUT };
    const int sel = qBound(0, m_bpType->currentIndex(), 5);
    const bool port = sel >= 4;
    const QString start = m_bpStart->text().trimmed(), end = m_bpEnd->text().trimmed();
    if (start.isEmpty()) return;
    long first, last;
    const long limit = port ? 0xFF : 0xFFFF;
    if (!addressOf(start, &first) || first < 0 || first > limit) {
        m_bpMsg->setText(port ? QStringLiteral("A port is $00-$FF") : QStringLiteral("Not an address ($8000, or a label)"));
        return;
    }
    last = first;
    if (!end.isEmpty() && (!addressOf(end, &last) || last < first || last > limit)) {
        m_bpMsg->setText(QStringLiteral("The end is not an address at or after the start"));
        return;
    }
    char err[256] = "";
    const QByteArray cond = m_bpCond->text().trimmed().toUtf8();
    if (adamdebug_breakpoint_add(m_dbg, types[sel], (uint16_t)first, (uint16_t)last, cond.constData(), err, sizeof err) < 0) {
        m_bpMsg->setText(err[0] ? stripControl(err) : QStringLiteral("The breakpoint could not be added"));
        return;
    }
    m_bpMsg->clear();
    m_bpStart->clear();
    m_bpEnd->clear();
    m_bpCond->clear();
    resync();
}

void DebuggerWindow::addWatch()
{
    if (adamwatch_add(m_wtEntry->text().toUtf8().constData()) < 0) {
        message(QStringLiteral("Type an address, label, expression or lo-hi (the list holds %1)").arg(ADAMWATCH_MAX));
        return;
    }
    m_wtEntry->clear();
    resync();
}

void DebuggerWindow::breakAtSymbol()
{
    QListWidgetItem *item = m_symList->currentItem();
    if (!item) {
        message(QStringLiteral("Select a symbol first"));
        return;
    }
    const uint16_t addr = uint16_t(item->data(Qt::UserRole).toUInt());
    const int now = adamdebug_breakpoint_toggle(m_dbg, addr);
    message(now ? QStringLiteral("Breakpoint at $%1").arg(hexText(addr, 4)) : QStringLiteral("Breakpoint at $%1 removed").arg(hexText(addr, 4)));
    resync();
}

void DebuggerWindow::loadSymbols(bool fromFile)
{
    QString path;
    if (fromFile) {
        const QString cart = QString::fromUtf8(adamsession_get_str(m_session, "cart_path", ""));
        path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Load Symbols"), cart.isEmpty() ? QString() : QFileInfo(cart).absolutePath(),
            QStringLiteral("Symbol files (*.sym *.map *.noi *.txt);;All files (*)"));
        if (path.isEmpty()) return;
    }
    char msg[512] = "";
    const QByteArray bytes = path.toLocal8Bit();
    adamdebug_load_symbols(m_dbg, fromFile ? bytes.constData() : nullptr, msg, sizeof msg);
    say(msg[0] ? stripControl(msg) : QStringLiteral("No symbols found"));
    refreshSymbols(true);
    resync();
}

void DebuggerWindow::save(const char *kind, const QString &title, const QString &file)
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save %1").arg(title), file);
    if (path.isEmpty()) return;
    char msg[512] = "";
    adamdebug_save(m_dbg, kind, path.toLocal8Bit().constData(), msg, sizeof msg);
    say(msg[0] ? stripControl(msg) : QStringLiteral("Saved %1").arg(path));
}

/* ---- tabs peeled off into windows of their own --------------------------------- */

/* A page's place in the tab widget is not its identity once tabs can be
 * moved into windows of their own: each carries its original index. */
int DebuggerWindow::currentTab() const
{
    QWidget *page = m_tabs->currentWidget();
    return page ? page->property("tab").toInt() : -1;
}

/* On screen: the tab widget's page, or a window of its own. */
bool DebuggerWindow::tabVisible(int tab) const
{
    return currentTab() == tab || m_popped[tab] != nullptr;
}

/* Brings a tab forward, wherever it lives. */
void DebuggerWindow::showTab(int tab)
{
    if (m_popped[tab]) {
        m_popped[tab]->activateWindow();
        m_popped[tab]->raise();
    } else {
        m_tabs->setCurrentWidget(m_pages[tab]);
    }
}

void DebuggerWindow::popOut(int tab)
{
    if (tab < 0 || tab >= TAB_COUNT || m_popped[tab] || m_tabs->count() <= 1) return;   /* one tab always stays */
    QWidget *page = m_pages[tab];
    m_tabs->removeTab(m_tabs->indexOf(page));

    auto *win = new PopWindow;
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->setWindowTitle(QStringLiteral("Debugger – %1").arg(QString::fromUtf8(kTabs[tab].label)));
    win->setProperty("adamHelpContext", QStringLiteral("debugger/%1").arg(QLatin1String(kTabs[tab].name)));
    auto *layout = new QVBoxLayout(win);
    auto *bar = new QHBoxLayout;
    auto *dockBtn = new QPushButton(QStringLiteral("Dock"));
    dockBtn->setToolTip(QStringLiteral("Put this tab back in the debugger"));
    bar->addWidget(dockBtn);
    bar->addStretch();
    layout->addLayout(bar);
    layout->addWidget(page, 1);
    page->show();
    win->resize(760, 520);
    m_popped[tab] = win;
    win->onClose = [this, tab] {
        if (!m_closing) dock(tab);
    };
    win->onKey = [this](QKeyEvent *evt) { return handleKey(evt); };
    connect(dockBtn, &QPushButton::clicked, win, &QWidget::close);
    new QShortcut(QKeySequence(Qt::Key_F1), win, [win] { adamHelpFor(win); });
    win->show();
    resync();
}

void DebuggerWindow::dock(int tab)
{
    QWidget *win = m_popped[tab];
    if (!win) return;
    QWidget *page = m_pages[tab];
    m_popped[tab] = nullptr;
    int pos = 0;
    for (int before = 0; before < tab; before++)   /* back where it was in the order */
        if (!m_popped[before]) pos++;
    page->setParent(nullptr);
    m_tabs->insertTab(pos, page, tabLabel(tab));
    m_tabs->setCurrentIndex(pos);
    resync();
}

/* ---- events --------------------------------------------------------------- */

bool DebuggerWindow::eventFilter(QObject *obj, QEvent *evt)
{
    /* Tab completes; Up and Down walk the history */
    if (obj == m_conIn && evt->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(evt);
        if (keyEvent->key() == Qt::Key_Tab) {
            completeConsole();
            return true;
        }
        if (keyEvent->key() == Qt::Key_Up || keyEvent->key() == Qt::Key_Down) {
            if (keyEvent->key() == Qt::Key_Up && m_historyPos > 0) --m_historyPos;
            else if (keyEvent->key() == Qt::Key_Down && m_historyPos < m_history.size()) ++m_historyPos;
            m_conIn->setText(m_historyPos < m_history.size() ? m_history.at(m_historyPos) : QString());
            return true;
        }
    }
    if (m_disasm && obj == m_disasm->viewport()) {
        /* A click selects a line (Run to Cursor); a click on its left edge,
         * or a double-click, toggles its breakpoint. A label has no line of
         * its own, so there is nothing else to hit. */
        if (evt->type() == QEvent::MouseButtonPress || evt->type() == QEvent::MouseButtonDblClick) {
            auto *mouseEvent = static_cast<QMouseEvent *>(evt);
            if (mouseEvent->button() != Qt::LeftButton) return true;
            const QTextCursor cur = m_disasm->cursorForPosition(mouseEvent->position().toPoint());
            const int line = cur.blockNumber();
            if (line < 0 || line >= (int)m_lineAddr.size()) return true;
            const bool gutter = cur.positionInBlock() <= 2;
            const bool dbl = evt->type() == QEvent::MouseButtonDblClick;
            m_selected = m_lineAddr[(size_t)line];
            if (gutter ? !dbl : dbl) toggleBreakpoint(m_lineAddr[(size_t)line]);
            else refreshDisasm();
            return true;
        }
        if (evt->type() == QEvent::MouseButtonRelease || evt->type() == QEvent::MouseMove) return true;
        if (evt->type() == QEvent::Wheel) {
            m_wheel += static_cast<QWheelEvent *>(evt)->angleDelta().y();
            const int rows = -m_wheel / 40;     /* a notch is three rows */
            if (rows != 0) {
                m_wheel += rows * 40;
                m_followPc->setChecked(false);
                m_disasmTop = (uint16_t)adamdebug_row_address(m_dbg, m_disasmTop, rows);
                refreshDisasm();
            }
            return true;
        }
        if (evt->type() == QEvent::Resize && isVisible()) refreshDisasm();
    }
    return QMainWindow::eventFilter(obj, evt);
}

/* These keys are the debugger's here, taken as plain key presses ahead of
 * any application-wide shortcut. */
bool DebuggerWindow::event(QEvent *evt)
{
    if (evt->type() == QEvent::ShortcutOverride && isDebuggerKey(static_cast<QKeyEvent *>(evt))) {
        evt->accept();
        return true;
    }
    return QMainWindow::event(evt);
}

/* The family's keys, from this window or one of its peeled-off tabs. */
bool DebuggerWindow::handleKey(QKeyEvent *evt)
{
    if (!isDebuggerKey(evt)) return false;
    const bool shift = evt->modifiers() & Qt::ShiftModifier;
    /* held, the steps repeat; the others do not */
    if (evt->isAutoRepeat() && (evt->key() == Qt::Key_F5 || evt->key() == Qt::Key_F9 || evt->key() == Qt::Key_F12)) return true;
    switch (evt->key()) {
    case Qt::Key_F5: toggleRun(); break;
    case Qt::Key_F7: stepAnd(shift ? frameOne : adamdebug_step); break;
    case Qt::Key_F8: stepAnd(shift ? adamdebug_step_out : adamdebug_step_over); break;
    case Qt::Key_F9: toggleSelected(); break;
    default: hide(); break;     /* F12 */
    }
    return true;
}

void DebuggerWindow::keyPressEvent(QKeyEvent *evt)
{
    if (!handleKey(evt)) QMainWindow::keyPressEvent(evt);
}

/* Shown: the engine attaches, which stops the machine, as on every sibling.
 * The tabs have no size until the window is up, so the first refresh waits
 * for the event loop. A restore from the taskbar is not a show. */
void DebuggerWindow::showEvent(QShowEvent *evt)
{
    QMainWindow::showEvent(evt);
    m_timer.start(100);
    if (evt->spontaneous()) return;
    adamdebug_attach(m_dbg);
    m_wasStopped = false;
    m_seenSerial = ~0ull;
    for (QWidget *win : m_popped)
        if (win) win->show();
    QTimer::singleShot(0, this, [this] {
        if (!isVisible()) return;
        firstShow();
        if (currentTab() == TAB_CONSOLE) m_conIn->setFocus();
        settleAndSync();
    });
}

/* The scripted-capture variables that touch the machine (read at construction),
 * applied once the window is up: ADAM_DEBUGGER_WATCH adds a watch,
 * ADAM_DEBUGGER_WATCH_BREAK a write-when-changed breakpoint on it,
 * ADAM_DEBUGGER_MEM_GOTO shows it in Memory, ADAM_DEBUGGER_HISTORY records
 * from the start. (ADAM_DEBUGGER_TAB and ADAM_DEBUGGER_POPOUT are read at construction.) */
void DebuggerWindow::firstShow()
{
    if (m_shownOnce) return;
    m_shownOnce = true;
    uint16_t first, last;
    if (!m_envWatch.isEmpty()) adamwatch_add(m_envWatch.constData());
    if (!m_envWatchBreak.isEmpty() && adamsym_parse_range(m_dbg, m_envWatchBreak.constData(), &first, &last)) {
        char err[200] = "";
        adamdebug_breakpoint_add(m_dbg, ADAMDEBUG_BP_WRITE, first, last, "value != old", err, sizeof err);
    }
    if (!m_envMemGoto.isEmpty() && adamsym_parse_range(m_dbg, m_envMemGoto.constData(), &first, &last)) {
        showInMemory(first, last);
    }
    if (m_envHistory) adamdebug_history_enable(m_dbg, 1);   /* record from the start */
}

/* Hidden by F12, the close button or the main window: the engine goes away
 * (it takes the breakpoints out of the core) and the machine runs on. The
 * close button is a plain close, which only hides the window (it is not
 * deleted, so it comes back as it was); ignoring the close instead would
 * make QApplication's quit, which closes every window first, give up
 * whenever it reached this one. A minimised window's spontaneous hide is not
 * a detach. The peeled-off tabs go with it. */
void DebuggerWindow::hideEvent(QHideEvent *evt)
{
    m_timer.stop();
    if (!evt->spontaneous()) {
        for (QWidget *win : m_popped)
            if (win) win->hide();
        if (!m_closing) adamdebug_detach(m_dbg);
    }
    QMainWindow::hideEvent(evt);
}

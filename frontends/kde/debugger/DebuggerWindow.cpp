/*
 * Debugger window (Qt6 Widgets) over the Z80 / VDP debugger engine, via
 * core/include/adamdebug.h. Mirrors the GNOME one tab for tab, and follows
 * the SMS, MSX and Atari 7800 KDE debuggers (the family's newest).
 *
 * Tabs, in this order (ADAM_DEBUGGER_TAB picks the one a window opens on, by
 * index or by name -- 0 console, 1 cpu, 2 disassembly, 3 vdp, 4 breakpoints,
 * 5 memmap, 6 fujinet -- or by the start of a label): the Console (the
 * engine's command line, with a history and Tab completion), CPU & Memory
 * (registers, flags, a hex dump), the Disassembly (click a line to select it
 * for Run to Cursor; click its left edge, double-click it or press F9 for its
 * breakpoint), the VDP (the nametable, pattern, sprite and palette pictures,
 * the decoded state and a VRAM hex editor), Breakpoints (execute, read,
 * write, port IN and OUT, with a condition), Memory Map & I/O and FujiNet
 * (the AdamNet packets on the BoIP link). The toolbar carries the stepping
 * controls, symbol loading and Save; the family's keys apply (F5 run/stop,
 * F7 step, F8 step over, Shift+F8 step out, Shift+F7 frame, F9 breakpoint,
 * F12 close).
 *
 * Showing the window attaches (which stops the machine, as on every
 * sibling); hiding it detaches, which takes the breakpoints out of the core
 * and lets the machine run on.
 *
 * A short timer polls the engine's generation and refreshes the tab showing
 * when it moved; while the machine runs, the live tabs refresh twice a
 * second. The stop callback fires on the emulator thread, so it only counts
 * stops, which tells the console about one that ended between two polls.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
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
#include <QShowEvent>
#include <QSplitter>
#include <QStatusBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "../FujiNetWindows.h"

#define DISASM_WINDOW 64        /* lines fetched; as many show as fit */
#define MEM_ROWS 256            /* 4K of memory, 16 bytes a row */
#define BP_MAX 256
#define TRACE_MAX 512
#define CONSOLE_LINES 5000
#define VRAM_ROWS 16
#define POKE_MAX 64

namespace {

enum { TAB_CONSOLE, TAB_CPU, TAB_DISASM, TAB_VDP, TAB_BREAKS, TAB_IO, TAB_FUJINET, TAB_COUNT };

/* The tabs, in order: the names ADAM_DEBUGGER_TAB takes, and their labels
 * (which it takes too). */
const struct { const char *name, *label; } kTabs[TAB_COUNT] = {
    { "console", "Console" }, { "cpu", "CPU & Memory" }, { "disassembly", "Disassembly" },
    { "vdp", "VDP" }, { "breakpoints", "Breakpoints" }, { "memmap", "Memory Map & I/O" },
    { "fujinet", "FujiNet" },
};

/* The register entries, in the order shown, and the adamdebug_reg each sets.
 * Pairs show four digits, I and R two, IM bare. */
const struct { const char *name; int id, digits; } kRegs[15] = {
    { "AF", ADAM_REG_AF, 4 },   { "BC", ADAM_REG_BC, 4 },   { "DE", ADAM_REG_DE, 4 },   { "HL", ADAM_REG_HL, 4 },
    { "IX", ADAM_REG_IX, 4 },   { "IY", ADAM_REG_IY, 4 },   { "SP", ADAM_REG_SP, 4 },   { "PC", ADAM_REG_PC, 4 },
    { "AF'", ADAM_REG_AF2, 4 }, { "BC'", ADAM_REG_BC2, 4 }, { "DE'", ADAM_REG_DE2, 4 }, { "HL'", ADAM_REG_HL2, 4 },
    { "I", ADAM_REG_I, 2 },     { "R", ADAM_REG_R, 2 },     { "IM", ADAM_REG_IM, 0 },
};

const struct { const char *name; int id; } kFlags[6] = {
    { "S", ADAM_FLAG_S }, { "Z", ADAM_FLAG_Z }, { "H", ADAM_FLAG_H },
    { "PV", ADAM_FLAG_PV }, { "N", ADAM_FLAG_N }, { "C", ADAM_FLAG_C },
};

const QString kGray = QStringLiteral("color: gray;");

/* The PC's line and a marked memory row: the app's accent (a light colour,
 * so black ink). */
QTextCharFormat accentFormat()
{
    QTextCharFormat f;
    f.setBackground(adamAccentColor());
    f.setForeground(Qt::black);
    return f;
}

void setMono(QWidget *w)
{
    QFont f = w->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    w->setFont(f);
}

QPlainTextEdit *monoView()
{
    auto *v = new QPlainTextEdit;
    v->setReadOnly(true);
    setMono(v);
    v->setLineWrapMode(QPlainTextEdit::NoWrap);
    return v;
}

/* Engine text for a view: control bytes dropped. */
QString stripControl(const char *s)
{
    const QString in = QString::fromUtf8(s);
    QString out;
    out.reserve(in.size());
    for (const QChar c : in)
        if (c.unicode() >= 0x20 || c == QLatin1Char('\n') || c == QLatin1Char('\t')) out += c;
    return out;
}

QString hx(unsigned v, int digits)
{
    return QStringLiteral("%1").arg(v, digits, 16, QLatin1Char('0')).toUpper();
}

QLabel *titleLabel(const QString &text)
{
    auto *l = new QLabel(text);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

QLabel *hintLabel(const QString &text)
{
    auto *l = new QLabel(text);
    l->setStyleSheet(kGray);
    l->setWordWrap(true);
    return l;
}

QLineEdit *smallEntry(const QString &placeholder, int chars)
{
    auto *e = new QLineEdit;
    e->setPlaceholderText(placeholder);
    e->setFixedWidth(e->fontMetrics().averageCharWidth() * chars + 16);
    return e;
}

/* A register entry exactly wide enough for its hex digits. */
void setDigits(QLineEdit *e, int digits)
{
    e->setFixedWidth(e->fontMetrics().horizontalAdvance(QString(digits, QLatin1Char('0'))) + 16);
}

/* A toggle takes the app-wide checked style (the accent, a box of its own),
 * which is not the native button's: one size for both states and every text,
 * so it never jumps. */
void fixToggleSize(QPushButton *b, const QStringList &texts, int minHeight)
{
    b->ensurePolished();
    QSize s(0, minHeight);
    for (const bool on : { false, true })
        for (const QString &t : texts) {
            b->setChecked(on);
            b->setText(t);
            s = s.expandedTo(b->sizeHint());
        }
    b->setChecked(false);
    b->setText(texts.first());
    b->setFixedSize(s);
}

/* Whether a view's reader is at its end (a log then follows new lines). */
bool atEnd(QPlainTextEdit *v)
{
    return v->verticalScrollBar()->value() >= v->verticalScrollBar()->maximum() - 1;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
void setText(QPlainTextEdit *v, const QString &text, bool follow = false)
{
    if (v->toPlainText() == text) {
        if (follow) v->verticalScrollBar()->setValue(v->verticalScrollBar()->maximum());
        return;
    }
    const int top = v->verticalScrollBar()->value(), left = v->horizontalScrollBar()->value();
    v->setPlainText(text);
    v->verticalScrollBar()->setValue(follow ? v->verticalScrollBar()->maximum() : top);
    v->horizontalScrollBar()->setValue(left);
}

/* A whole line of a view in a format: the PC's fill, a marked row. */
QTextEdit::ExtraSelection lineSelection(QPlainTextEdit *v, int line, const QTextCharFormat &fmt)
{
    QTextEdit::ExtraSelection s;
    s.cursor = QTextCursor(v->document()->findBlockByNumber(line));
    s.format = fmt;
    s.format.setProperty(QTextFormat::FullWidthSelection, true);
    return s;
}

/* A stretch of one line of a view in a format. */
QTextEdit::ExtraSelection spanSelection(QPlainTextEdit *v, int line, int from, int length, const QTextCharFormat &fmt)
{
    QTextEdit::ExtraSelection s;
    const QTextBlock block = v->document()->findBlockByNumber(line);
    s.cursor = QTextCursor(block);
    s.cursor.setPosition(block.position() + from);
    s.cursor.setPosition(block.position() + from + length, QTextCursor::KeepAnchor);
    s.format = fmt;
    return s;
}

/* A status line that elides instead of widening the window. */
class ElidedLabel : public QLabel {
public:
    ElidedLabel() { setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); }
    QSize minimumSizeHint() const override { return QSize(40, QLabel::minimumSizeHint().height()); }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QRect r = contentsRect();
        p.setPen(palette().color(foregroundRole()));
        p.drawText(r, (alignment() & Qt::AlignHorizontal_Mask) | Qt::AlignVCenter,
                   fontMetrics().elidedText(text(), Qt::ElideRight, r.width()));
    }
};

/* ADAM_DEBUGGER_TAB: a page number, or a tab's name or label (any case), or
 * the start of one, or of a word of a label ("map" ...). */
int tabFromEnv()
{
    const QString tab = qEnvironmentVariable("ADAM_DEBUGGER_TAB").trimmed();
    if (tab.isEmpty()) return -1;
    bool isNum = false;
    const int n = tab.toInt(&isNum);
    if (isNum) return n >= 0 && n < TAB_COUNT ? n : -1;
    for (int pass = 0; pass < 3; ++pass)
        for (int i = 0; i < TAB_COUNT; ++i) {
            const QString name = QString::fromUtf8(kTabs[i].name), label = QString::fromUtf8(kTabs[i].label);
            if (pass == 0 && name.startsWith(tab, Qt::CaseInsensitive)) return i;
            if (pass == 1 && label.startsWith(tab, Qt::CaseInsensitive)) return i;
            if (pass == 2) {
                const QStringList words = label.split(QLatin1Char(' '));
                for (int w = 1; w < words.size(); ++w)
                    if (words[w].startsWith(tab, Qt::CaseInsensitive)) return i;
            }
        }
    return -1;
}

/* The family's keys: F5 run/stop, F7 step, Shift+F7 frame, F8 step over,
 * Shift+F8 step out, F9 breakpoint, F12 close. */
bool isDebuggerKey(const QKeyEvent *e)
{
    const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    switch (e->key()) {
    case Qt::Key_F5: case Qt::Key_F9: case Qt::Key_F12: return mods == Qt::NoModifier;
    case Qt::Key_F7: case Qt::Key_F8: return mods == Qt::NoModifier || mods == Qt::ShiftModifier;
    default: return false;
    }
}

QPointer<DebuggerWindow> s_win;

/* Every park of the machine, counted on the emulator thread (all the
 * callback does), so a stop that comes and goes between two polls -- a run
 * that hits a breakpoint at once -- still reaches the console. */
std::atomic<unsigned> g_stops{ 0 };

void countStop(void *, int, uint16_t)
{
    g_stops.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

void DebuggerWindow::showFor(QWidget *parent, adamsession *session)
{
    if (!s_win) s_win = new DebuggerWindow(session, parent);
    /* showEvent attaches, which stops the machine */
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
    adamdebug_set_stop_callback(m_dbg, countStop, nullptr);
    m_seenStops = g_stops.load(std::memory_order_relaxed);
    setWindowTitle(QStringLiteral("ADAM Debugger"));
    resize(1400, 820);

    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->addWidget(buildToolbar());
    m_tabs = new QTabWidget;
    m_tabs->setUsesScrollButtons(true);
    QWidget *pages[TAB_COUNT];
    pages[TAB_CONSOLE] = buildConsole();
    pages[TAB_CPU] = buildCpu();
    pages[TAB_DISASM] = buildDisasm();
    pages[TAB_VDP] = buildVdp();
    pages[TAB_BREAKS] = buildBreaks();
    pages[TAB_IO] = buildIo();
    pages[TAB_FUJINET] = buildFujinet();
    for (int i = 0; i < TAB_COUNT; ++i)
        m_tabs->addTab(pages[i], QString::fromUtf8(kTabs[i].label).replace(QLatin1Char('&'), QLatin1String("&&")));
    const int tab = tabFromEnv();
    if (tab >= 0) m_tabs->setCurrentIndex(tab);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int page) {
        refreshPage(page, false);
        if (page == TAB_CONSOLE) m_conIn->setFocus();
    });
    root->addWidget(m_tabs, 1);
    setCentralWidget(central);
    statusBar();

    connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
    m_timer.start(100);
}

/* Only at exit, with the main window: the session outlives its windows. */
DebuggerWindow::~DebuggerWindow()
{
    if (adamdebug_is_attached(m_dbg)) adamdebug_detach(m_dbg);
}

/* ---- building ------------------------------------------------------------- */

QWidget *DebuggerWindow::buildToolbar()
{
    auto *bar = new QWidget;
    auto *h = new QHBoxLayout(bar);
    h->setContentsMargins(0, 0, 0, 0);
    auto add = [&](const QString &label, const QString &tip, std::function<void()> fn) {
        auto *b = new QPushButton(label);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, fn);
        h->addWidget(b);
        return b;
    };
    /* Run is a checkable button, checked while the machine is stopped: the
     * app-wide stylesheet gives that the accent. */
    m_runBtn = add(QStringLiteral("Stop (F5)"), QStringLiteral("Run, or stop before the next instruction"),
                   [this] { toggleRun(); });
    m_runBtn->setCheckable(true);
    auto *step = add(QStringLiteral("Step (F7)"), QStringLiteral("One instruction"),
                     [this] { stepAnd([this] { adamdebug_step(m_dbg); }); });
    add(QStringLiteral("Step Over (F8)"), QStringLiteral("Over a CALL, RST, DJNZ, block instruction or HALT"),
        [this] { stepAnd([this] { adamdebug_step_over(m_dbg); }); });
    add(QStringLiteral("Step Out (Shift+F8)"), QStringLiteral("On to the RET"),
        [this] { stepAnd([this] { adamdebug_step_out(m_dbg); }); });
    add(QStringLiteral("Run to Cursor"), QStringLiteral("On to the line selected in the Disassembly"),
        [this] { runToCursor(); });
    add(QStringLiteral("Frame (Shift+F7)"), QStringLiteral("On to the end of the frame, the whole picture drawn"),
        [this] { stepAnd([this] { adamdebug_frame(m_dbg, 1); }); });
    fixToggleSize(m_runBtn, { QStringLiteral("Stop (F5)"), QStringLiteral("Run (F5)") }, step->sizeHint().height());

    m_status = new ElidedLabel;
    m_status->setAlignment(Qt::AlignRight);
    m_status->setStyleSheet(kGray);
    m_status->setContentsMargins(8, 0, 8, 0);
    h->addWidget(m_status, 1);

    auto *sym = new QPushButton(QStringLiteral("Load Symbols…"));
    sym->setFocusPolicy(Qt::NoFocus);
    sym->setToolTip(QStringLiteral("Labels from a .sym, .map or .noi file"));
    auto *symMenu = new QMenu(sym);
    symMenu->addAction(QStringLiteral("Symbol File…"), this, [this] { loadSymbols(true); });
    symMenu->addAction(QStringLiteral("Next to the Cartridge"), this, [this] { loadSymbols(false); });
    sym->setMenu(symMenu);
    h->addWidget(sym);

    auto *saveBtn = new QPushButton(QStringLiteral("Save…"));
    saveBtn->setFocusPolicy(Qt::NoFocus);
    auto *menu = new QMenu(saveBtn);
    menu->addAction(QStringLiteral("Disassembly ($0000-$FFFF as mapped)"), this,
                    [this] { save("dis", QStringLiteral("Disassembly"), QStringLiteral("disassembly.asm")); });
    menu->addAction(QStringLiteral("Memory (the 64K the CPU sees)"), this,
                    [this] { save("mem", QStringLiteral("Memory"), QStringLiteral("memory.bin")); });
    menu->addAction(QStringLiteral("VRAM"), this,
                    [this] { save("vram", QStringLiteral("VRAM"), QStringLiteral("vram.bin")); });
    menu->addAction(QStringLiteral("CPU and VDP registers"), this,
                    [this] { save("regs", QStringLiteral("Registers"), QStringLiteral("registers.txt")); });
    saveBtn->setMenu(menu);
    h->addWidget(saveBtn);
    return bar;
}

QWidget *DebuggerWindow::buildConsole()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_conOut = monoView();
    m_conOut->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_conOut->setMaximumBlockCount(CONSOLE_LINES);
    m_conOut->setPlainText(QStringLiteral("Debugger console. Type 'help' for every command. Numbers: $C000 or 0xC000 "
                                          "for hex, bare digits are decimal.\n"));
    m_conIn = new QLineEdit;
    setMono(m_conIn);
    m_conIn->setPlaceholderText(QStringLiteral("command (help, step, break, bpw, mem, poke, runto, disasm, regs, "
                                               "io ...) — Tab completes, Up/Down recall"));
    m_conIn->installEventFilter(this);
    connect(m_conIn, &QLineEdit::returnPressed, this, [this] { runConsole(); });
    v->addWidget(m_conOut, 1);
    v->addWidget(m_conIn);
    return w;
}

QWidget *DebuggerWindow::buildCpu()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);

    /* the main set on the first row, the shadow set and the rest on the
     * second; Enter sets one */
    auto *regs = new QGridLayout;
    regs->setHorizontalSpacing(8);
    for (int i = 0; i < 15; ++i) {
        const int row = i < 8 ? 0 : 1, col = 2 * (i < 8 ? i : i - 8);
        auto *l = new QLabel(QString::fromUtf8(kRegs[i].name));
        l->setStyleSheet(kGray);
        regs->addWidget(l, row, col, Qt::AlignRight);
        m_reg[i] = new QLineEdit;
        setMono(m_reg[i]);
        m_reg[i]->setMaxLength(12);
        setDigits(m_reg[i], kRegs[i].digits ? kRegs[i].digits : 2);
        m_reg[i]->setToolTip(QStringLiteral("Enter sets it (hex, or an expression; only while stopped)"));
        connect(m_reg[i], &QLineEdit::returnPressed, this, [this, i] { setReg(i); });
        regs->addWidget(m_reg[i], row, col + 1);
    }
    regs->setColumnStretch(16, 1);
    v->addLayout(regs);

    auto *flags = new QHBoxLayout;
    for (int i = 0; i < 6; ++i) {
        m_flag[i] = new QCheckBox(QString::fromUtf8(kFlags[i].name));
        connect(m_flag[i], &QCheckBox::clicked, this, [this, i](bool on) { setFlag(kFlags[i].id, on); });
        flags->addWidget(m_flag[i]);
    }
    flags->addSpacing(16);
    for (int i = 0; i < 2; ++i) {
        m_iff[i] = new QCheckBox(QStringLiteral("IFF%1").arg(i + 1));
        connect(m_iff[i], &QCheckBox::clicked, this,
                [this, i](bool on) { setFlag(i ? ADAM_REG_IFF2 : ADAM_REG_IFF1, on); });
        flags->addWidget(m_iff[i]);
    }
    flags->addStretch(1);
    v->addLayout(flags);

    m_beam = new QLabel;
    setMono(m_beam);
    m_beam->setStyleSheet(kGray);
    v->addWidget(m_beam);

    v->addWidget(titleLabel(QStringLiteral("Memory (as the CPU sees it through the bank map)")));
    m_mem = monoView();
    v->addWidget(m_mem, 1);

    /* the dump: Go to, a page back or on, and a write */
    auto *nav = new QHBoxLayout;
    nav->addWidget(new QLabel(QStringLiteral("Go to")));
    m_memGoto = smallEntry(QStringLiteral("$C000 or label"), 16);
    connect(m_memGoto, &QLineEdit::returnPressed, this, [this] { memGoto(); });
    nav->addWidget(m_memGoto);
    for (int dir = -1; dir <= 1; dir += 2) {
        auto *b = new QToolButton;
        b->setArrowType(dir < 0 ? Qt::UpArrow : Qt::DownArrow);
        b->setToolTip(dir < 0 ? QStringLiteral("Back 4K") : QStringLiteral("On 4K"));
        connect(b, &QToolButton::clicked, this, [this, dir] {
            m_memTop = qBound(0, m_memTop + dir * MEM_ROWS * 16, 0x10000 - MEM_ROWS * 16);
            refreshMem();
        });
        nav->addWidget(b);
    }
    nav->addSpacing(12);
    nav->addWidget(new QLabel(QStringLiteral("Write")));
    m_memAddr = smallEntry(QStringLiteral("$C000"), 10);
    m_memVal = smallEntry(QStringLiteral("$00"), 5);
    connect(m_memAddr, &QLineEdit::returnPressed, this, [this] { memWrite(); });
    connect(m_memVal, &QLineEdit::returnPressed, this, [this] { memWrite(); });
    nav->addWidget(m_memAddr);
    nav->addWidget(new QLabel(QStringLiteral("value")));
    nav->addWidget(m_memVal);
    nav->addStretch(1);
    v->addLayout(nav);
    return w;
}

QWidget *DebuggerWindow::buildDisasm()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
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
    v->addLayout(row);
    m_disasm = monoView();
    /* no scrollbar: the wheel scrolls by instructions, as many lines as fit */
    m_disasm->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_disasm->setFocusPolicy(Qt::NoFocus);
    m_disasm->viewport()->setCursor(Qt::ArrowCursor);
    m_disasm->viewport()->installEventFilter(this);
    v->addWidget(m_disasm, 1);
    return w;
}

QWidget *DebuggerWindow::buildVdp()
{
    auto *vdp = new QWidget;
    auto *grid = new QGridLayout(vdp);
    auto make_pic = [](QLabel **out, int w, int h) {
        auto *l = new QLabel;
        l->setMinimumSize(w, h);
        l->setScaledContents(true);
        *out = l;
        return l;
    };
    /* the nametable left, the patterns over the sprites in the middle, the
     * sprite list right */
    grid->addWidget(new QLabel(QStringLiteral("Nametable")), 0, 0);
    grid->addWidget(make_pic(&m_nt, 512, 384), 1, 0, 4, 1, Qt::AlignTop);
    m_patBank = new QComboBox;
    m_patBank->addItems({ QStringLiteral("Bank 0"), QStringLiteral("Bank 1"), QStringLiteral("Bank 2") });
    connect(m_patBank, &QComboBox::currentIndexChanged, this, [this](int) { refreshVdp(); });
    grid->addWidget(new QLabel(QStringLiteral("Patterns")), 0, 1);
    grid->addWidget(m_patBank, 1, 1);
    grid->addWidget(make_pic(&m_pat, 384, 96), 2, 1, Qt::AlignTop);
    grid->addWidget(new QLabel(QStringLiteral("Sprites")), 3, 1);
    grid->addWidget(make_pic(&m_spr, 256, 128), 4, 1, Qt::AlignTop);
    grid->addWidget(new QLabel(QStringLiteral("Sprite list")), 0, 2);
    m_spriteInfo = monoView();
    m_spriteInfo->setMinimumWidth(200);
    grid->addWidget(m_spriteInfo, 1, 2, 4, 1);
    grid->addWidget(new QLabel(QStringLiteral("Palette")), 5, 0);
    /* 1:1 with the rendered swatch grid so the index labels stay crisp,
     * pinned to the top-left of its cell */
    m_pal = new QLabel;
    m_pal->setFixedSize(ADAMVDP_PAL_W, ADAMVDP_PAL_H);
    grid->addWidget(m_pal, 6, 0, Qt::AlignTop | Qt::AlignLeft);

    /* decoded registers + the table addresses the beam is fetching from */
    grid->addWidget(new QLabel(QStringLiteral("Registers & tables")), 5, 1);
    m_vdpState = monoView();
    m_vdpState->setMinimumHeight(260);
    grid->addWidget(m_vdpState, 6, 1, 1, 2);

    /* the VRAM hex editor */
    grid->addWidget(new QLabel(QStringLiteral("VRAM (Enter writes; addr [bytes...]; addresses wrap at 16K)")), 7, 0, 1, 3);
    auto *bar = new QWidget;
    auto *barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(0, 0, 0, 0);
    m_vramEntry = new QLineEdit;
    m_vramEntry->setPlaceholderText(QStringLiteral("addr [bytes...] e.g. 1800: 41 42 43"));
    connect(m_vramEntry, &QLineEdit::returnPressed, this, [this] { vramSubmit(); });
    auto *prev = new QPushButton(QStringLiteral("◀"));
    auto *next = new QPushButton(QStringLiteral("▶"));
    prev->setFixedWidth(40);
    next->setFixedWidth(40);
    connect(prev, &QPushButton::clicked, this, [this] { vramPage(-1); });
    connect(next, &QPushButton::clicked, this, [this] { vramPage(1); });
    m_vramStatus = new QLabel;
    barLayout->addWidget(m_vramEntry, 1);
    barLayout->addWidget(prev);
    barLayout->addWidget(next);
    barLayout->addWidget(m_vramStatus);
    grid->addWidget(bar, 8, 0, 1, 3);
    m_vramView = monoView();
    m_vramView->setMinimumHeight(180);
    grid->addWidget(m_vramView, 9, 0, 1, 3);
    /* the slack goes to the pictures, or every label floats away from what
     * it labels */
    grid->setRowStretch(4, 1);

    /* the page can outgrow the window on a short screen: it scrolls */
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(vdp);
    return scroll;
}

QWidget *DebuggerWindow::buildBreaks()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);

    auto *top = new QHBoxLayout;
    top->addWidget(hintLabel(QStringLiteral("Execute stops before the instruction runs; the others after the "
                                            "access. Breakpoints are kept while the debugger is closed, but only "
                                            "act while it is open.")), 1);
    v->addLayout(top);

    m_bpTable = new QTableWidget(0, 6);
    m_bpTable->setHorizontalHeaderLabels({ QStringLiteral("On"), QStringLiteral("Type"), QStringLiteral("Range"),
                                           QStringLiteral("Condition"), QStringLiteral("Hits"), QString() });
    m_bpTable->verticalHeader()->setVisible(false);
    m_bpTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_bpTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_bpTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_bpTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_bpTable->setFocusPolicy(Qt::NoFocus);
    connect(m_bpTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_bpFilling || item->column() != 0) return;
        adamdebug_breakpoint_enable(m_dbg, item->data(Qt::UserRole).toInt(), item->checkState() == Qt::Checked);
        syncLater();
    });
    v->addWidget(m_bpTable, 1);
    m_bpEmpty = hintLabel(QStringLiteral("No breakpoints. Click the left edge of a disassembly line, or add one below."));
    v->addWidget(m_bpEmpty);

    auto *row = new QHBoxLayout;
    m_bpType = new QComboBox;
    m_bpType->addItems({ QStringLiteral("exec"), QStringLiteral("read"), QStringLiteral("write"),
                         QStringLiteral("in"), QStringLiteral("out") });
    m_bpType->setToolTip(QStringLiteral("in and out take a port ($00-$FF)"));
    m_bpStart = smallEntry(QStringLiteral("start ($8000 or label)"), 22);
    m_bpEnd = smallEntry(QStringLiteral("end (optional)"), 16);
    m_bpCond = new QLineEdit;
    m_bpCond->setPlaceholderText(QStringLiteral("condition (optional, e.g. a == 5 && value == $FF)"));
    auto *add = new QPushButton(QStringLiteral("Add"));
    auto *clear = new QPushButton(QStringLiteral("Clear All"));
    connect(add, &QPushButton::clicked, this, [this] { addBreakpoint(); });
    connect(clear, &QPushButton::clicked, this, [this] {
        adamdebug_breakpoint_clear(m_dbg);
        resync();
    });
    for (QLineEdit *e : { m_bpStart, m_bpEnd, m_bpCond })
        connect(e, &QLineEdit::returnPressed, this, [this] { addBreakpoint(); });
    row->addWidget(m_bpType);
    row->addWidget(m_bpStart);
    row->addWidget(m_bpEnd);
    row->addWidget(m_bpCond, 1);
    row->addWidget(add);
    row->addWidget(clear);
    v->addLayout(row);

    m_bpMsg = new QLabel;
    m_bpMsg->setWordWrap(true);
    m_bpMsg->setStyleSheet(QStringLiteral("color: #e01b24;"));
    v->addWidget(m_bpMsg);
    return w;
}

QWidget *DebuggerWindow::buildIo()
{
    m_io = monoView();
    return m_io;
}

/* The packets left (a note and Clear over them), the link and counts
 * right. */
QWidget *DebuggerWindow::buildFujinet()
{
    auto *split = new QSplitter(Qt::Horizontal);
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->addWidget(titleLabel(QStringLiteral("AdamNet packets over BoIP")));
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
    lv->addLayout(row);
    m_fujiText = monoView();
    lv->addWidget(m_fujiText, 1);
    left->setMinimumWidth(560);

    auto *right = new QWidget;
    auto *rv = new QVBoxLayout(right);
    rv->addWidget(titleLabel(QStringLiteral("BoIP link")));
    m_fujiState = monoView();
    rv->addWidget(m_fujiState, 1);
    right->setMinimumWidth(320);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setChildrenCollapsible(false);
    return split;
}

/* ---- refresh -------------------------------------------------------------- */

void DebuggerWindow::refreshStatus()
{
    char reason[160];
    int addr = -1;
    const bool stopped = adamdebug_is_stopped(m_dbg) != 0;
    reason[0] = '\0';
    adamdebug_stop_reason(m_dbg, reason, sizeof reason, &addr);
    QString text = QStringLiteral("Running");
    if (stopped) {
        const QString why = stripControl(reason);
        text = why.isEmpty() || why == QLatin1String("stopped") ? QStringLiteral("Stopped")
                                                                : QStringLiteral("Stopped: ") + why;
        /* where, by name when it has one */
        uint16_t off = 0;
        const char *sym = addr >= 0 ? adamdebug_symbol_at(m_dbg, (uint16_t)addr, &off) : nullptr;
        if (sym) text += QStringLiteral("  (%1%2)").arg(QString::fromUtf8(sym),
                                                        off ? QStringLiteral("+%1").arg(off) : QString());
    }
    if (m_status->text() != text) {
        m_status->setText(text);
        m_status->setToolTip(text);
        m_status->update();
    }
    const QString run = stopped ? QStringLiteral("Run (F5)") : QStringLiteral("Stop (F5)");
    if (m_runBtn->text() != run) m_runBtn->setText(run);
    if (m_runBtn->isChecked() != stopped) m_runBtn->setChecked(stopped);
}

/* The refresh never overwrites a value being typed. */
void DebuggerWindow::setEntry(QLineEdit *e, const QString &text)
{
    if (e->hasFocus()) return;
    if (e->text() != text) e->setText(text);
}

void DebuggerWindow::refreshCpu()
{
    adamdebug_cpu c;
    adamdebug_cpu_get(m_dbg, &c);
    for (int i = 0; i < 15; ++i) {
        int val = 0;
        switch (kRegs[i].id) {
        case ADAM_REG_AF: val = c.af; break;     case ADAM_REG_BC: val = c.bc; break;
        case ADAM_REG_DE: val = c.de; break;     case ADAM_REG_HL: val = c.hl; break;
        case ADAM_REG_IX: val = c.ix; break;     case ADAM_REG_IY: val = c.iy; break;
        case ADAM_REG_SP: val = c.sp; break;     case ADAM_REG_PC: val = c.pc; break;
        case ADAM_REG_AF2: val = c.af2; break;   case ADAM_REG_BC2: val = c.bc2; break;
        case ADAM_REG_DE2: val = c.de2; break;   case ADAM_REG_HL2: val = c.hl2; break;
        case ADAM_REG_I: val = c.i; break;       case ADAM_REG_R: val = c.r; break;
        default: val = c.im; break;
        }
        setEntry(m_reg[i], kRegs[i].digits ? hx((unsigned)val & (kRegs[i].digits == 4 ? 0xFFFF : 0xFF), kRegs[i].digits)
                                           : QString::number(val));
    }
    const int flags[6] = { c.sf, c.zf, c.hf, c.pf, c.nf, c.cf };
    for (int i = 0; i < 6; ++i) m_flag[i]->setChecked(flags[i] != 0);
    m_iff[0]->setChecked(c.iff1 != 0);
    m_iff[1]->setChecked(c.iff2 != 0);
    m_beam->setText(QStringLiteral("scanline %1   frame %2   %3 T-states   WZ %4%5")
                        .arg(c.line, 3).arg(c.frame).arg(c.cycles).arg(hx((unsigned)c.wz & 0xFFFF, 4),
                                                                      c.halted ? QStringLiteral("   HALT") : QString()));
}

void DebuggerWindow::refreshMem()
{
    static uint8_t mem[MEM_ROWS * 16];
    memset(mem, 0, sizeof mem);
    adamdebug_read(m_dbg, (uint16_t)m_memTop, mem, (int)sizeof mem);
    QString t;
    t.reserve(MEM_ROWS * 80);
    t += QStringLiteral("       ");
    for (int col = 0; col < 16; ++col) t += hx((unsigned)col, 1) + QStringLiteral("  ");
    for (int row = 0; row < MEM_ROWS; ++row) {
        const uint8_t *r = mem + row * 16;
        t += QStringLiteral("\n$%1: ").arg(hx((unsigned)(m_memTop + row * 16), 4));
        for (int col = 0; col < 16; ++col) t += hx(r[col], 2) + QLatin1Char(' ');
        t += QLatin1Char(' ');
        for (int col = 0; col < 16; ++col) t += QLatin1Char(r[col] >= 0x20 && r[col] < 0x7F ? (char)r[col] : '.');
    }
    setText(m_mem, t);

    /* the row Go to found stays marked across refreshes */
    QList<QTextEdit::ExtraSelection> sel;
    if (m_memMark >= m_memTop && m_memMark < m_memTop + MEM_ROWS * 16)
        sel << lineSelection(m_mem, 1 + (m_memMark - m_memTop) / 16, accentFormat());
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
        adamdebug_cpu c;
        adamdebug_cpu_get(m_dbg, &c);
        /* the PC a third of the way down */
        m_disasmTop = (uint16_t)adamdebug_row_address(m_dbg, (uint16_t)c.pc, -(rows / 3));
    }
    int pcLine = -1;
    const int n = adamdebug_disassemble(m_dbg, m_disasmTop, lines, rows, &pcLine);
    /* the label column is as wide as the longest label shown, within reason */
    int labelWidth = 0;
    for (int i = 0; i < n; ++i) labelWidth = qMax(labelWidth, (int)strlen(lines[i].label));
    labelWidth = qMin(labelWidth, 24);

    m_lineAddr.clear();
    QString text;
    char buf[128];
    struct Span { int line, from, length; };
    std::vector<Span> grays;
    for (int i = 0; i < n; ++i) {
        m_lineAddr.push_back(lines[i].address);
        /* the gutter: a breakpoint dot (hollow when disabled), then the
         * PC's arrow */
        QString ln = lines[i].has_breakpoint ? (lines[i].breakpoint_enabled ? QStringLiteral("●") : QStringLiteral("○"))
                                             : QStringLiteral(" ");
        snprintf(buf, sizeof buf, "%c%04X  %-11.11s ", lines[i].is_pc ? '>' : ' ', lines[i].address, lines[i].bytes);
        ln += QString::fromUtf8(buf);
        const int labelFrom = (int)ln.size();
        char label[48];
        snprintf(label, sizeof label, "%-*.*s ", labelWidth, labelWidth, lines[i].label);
        ln += QString::fromUtf8(label);
        if (lines[i].label[0]) grays.push_back({ i, labelFrom, (int)strlen(lines[i].label) < labelWidth
                                                                  ? (int)strlen(lines[i].label) : labelWidth });
        snprintf(buf, sizeof buf, "%-22s", lines[i].disasm);
        ln += QString::fromUtf8(buf);
        if (lines[i].comment[0]) {
            const QString comment = QStringLiteral("; ") + QString::fromUtf8(lines[i].comment);
            grays.push_back({ i, (int)ln.size(), (int)comment.size() });
            ln += comment;
        }
        if (i) text += QLatin1Char('\n');
        text += ln;
    }
    if (n == 0) text = QStringLiteral("(no disassembly: the debugger is not attached)");
    m_disasm->setPlainText(stripControl(text.toUtf8().constData()));
    m_disasm->verticalScrollBar()->setValue(0);

    QList<QTextEdit::ExtraSelection> sel;
    /* labels and comments in the system's grey */
    QTextCharFormat grey;
    grey.setForeground(m_disasm->palette().color(QPalette::Disabled, QPalette::Text));
    for (const Span &g : grays) sel << spanSelection(m_disasm, g.line, g.from, g.length, grey);
    for (int i = 0; i < n; ++i) {
        /* the gutter: a breakpoint red, a disabled one grey */
        if (lines[i].has_breakpoint) {
            QTextCharFormat f;
            if (lines[i].breakpoint_enabled) {
                f.setForeground(QColor(0xe0, 0x1b, 0x24));
                f.setFontWeight(QFont::Bold);
            } else {
                f.setForeground(QColor(0x9a, 0x99, 0x96));
            }
            sel << spanSelection(m_disasm, i, 0, 1, f);
        }
        /* the selected line underlined */
        if (lines[i].address == m_selected && i != pcLine) {
            QTextCharFormat f;
            f.setFontUnderline(true);
            f.setFontWeight(QFont::Bold);
            QTextEdit::ExtraSelection s;
            s.cursor = QTextCursor(m_disasm->document()->findBlockByNumber(i));
            s.cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            s.format = f;
            sel << s;
        }
    }
    /* the PC's line in the accent colour, last so its ink wins */
    if (pcLine >= 0) {
        QTextCharFormat f = accentFormat();
        if (lines[pcLine].address == m_selected) f.setFontUnderline(true);
        sel << lineSelection(m_disasm, pcLine, f);
    }
    m_disasm->setExtraSelections(sel);
}

void DebuggerWindow::refreshVdp()
{
    static adamvdp_snapshot snap;
    static uint8_t nt[256 * 192 * 4], pat[256 * 64 * 4], spr[128 * 64 * 4], pal[ADAMVDP_PAL_W * ADAMVDP_PAL_H * 4];
    adamvdp_sprite info[32];

    adamdebug_vdp_snapshot(m_dbg, &snap);

    adamvdp_render_nametable(&snap, nt);
    m_nt->setPixmap(QPixmap::fromImage(QImage(nt, 256, 192, 256 * 4, QImage::Format_RGBA8888)));
    adamvdp_render_patterns(&snap, m_patBank->currentIndex(), pat);
    m_pat->setPixmap(QPixmap::fromImage(QImage(pat, 256, 64, 256 * 4, QImage::Format_RGBA8888)));
    adamvdp_render_sprites(&snap, spr, info);
    m_spr->setPixmap(QPixmap::fromImage(QImage(spr, 128, 64, 128 * 4, QImage::Format_RGBA8888)));
    adamvdp_render_palette(&snap, pal);
    m_pal->setPixmap(QPixmap::fromImage(
        QImage(pal, ADAMVDP_PAL_W, ADAMVDP_PAL_H, ADAMVDP_PAL_W * 4, QImage::Format_RGBA8888)));

    QString text = QStringLiteral("##  Y    X  PAT CLR EC\n");
    for (int i = 0; i < 32; i++)
        text += QStringLiteral("%1 %2 %3  %4  %5  %6\n").arg(i, 2).arg(info[i].y, 4).arg(info[i].x, 4)
                    .arg(info[i].pattern, 3, 16).arg(info[i].color, 3).arg(info[i].early_clock);
    setText(m_spriteInfo, text);

    static char state[4096];
    adamvdp_format_state(&snap, state, int(sizeof state));
    setText(m_vdpState, QString::fromLatin1(state));
    adamvdp_format_hex(&snap, m_vramBase, VRAM_ROWS, state, int(sizeof state));
    setText(m_vramView, QString::fromLatin1(state));
}

void DebuggerWindow::refreshBps()
{
    static adamdebug_breakpoint bps[BP_MAX];
    const int n = adamdebug_breakpoint_list(m_dbg, bps, BP_MAX);

    /* rebuilt only when it changed (each step refreshes), and never while a
     * row's own button is being handled */
    QString sig;
    for (int i = 0; i < n; ++i)
        sig += QStringLiteral("%1 %2 %3 %4 %5 %6 %7\n").arg(bps[i].id).arg(bps[i].enabled).arg(bps[i].hits)
                   .arg(bps[i].type).arg(bps[i].start).arg(bps[i].end).arg(QString::fromUtf8(bps[i].condition));
    if (sig == m_bpSig && m_bpTable->rowCount() == n) return;
    m_bpSig = sig;

    QFont mono = m_bpTable->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    m_bpFilling = true;
    m_bpTable->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        auto *on = new QTableWidgetItem;
        on->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        on->setCheckState(bps[i].enabled ? Qt::Checked : Qt::Unchecked);
        on->setData(Qt::UserRole, bps[i].id);
        on->setToolTip(QStringLiteral("Enabled"));
        m_bpTable->setItem(i, 0, on);
        QStringList type;
        if (bps[i].type & ADAMDEBUG_BP_EXEC) type << QStringLiteral("exec");
        if (bps[i].type & ADAMDEBUG_BP_READ) type << QStringLiteral("read");
        if (bps[i].type & ADAMDEBUG_BP_WRITE) type << QStringLiteral("write");
        if (bps[i].type & ADAMDEBUG_BP_IN) type << QStringLiteral("in");
        if (bps[i].type & ADAMDEBUG_BP_OUT) type << QStringLiteral("out");
        m_bpTable->setItem(i, 1, new QTableWidgetItem(type.join(QLatin1Char('/'))));
        /* a port range for in/out is two digits */
        const int digits = (bps[i].type & (ADAMDEBUG_BP_IN | ADAMDEBUG_BP_OUT)) ? 2 : 4;
        QString range = QStringLiteral("$") + hx(bps[i].start, digits);
        if (bps[i].end != bps[i].start) range += QStringLiteral("-$") + hx(bps[i].end, digits);
        m_bpTable->setItem(i, 2, new QTableWidgetItem(range));
        m_bpTable->setItem(i, 3, new QTableWidgetItem(stripControl(bps[i].condition)));
        m_bpTable->setItem(i, 4, new QTableWidgetItem(QString::number(bps[i].hits)));
        for (int col = 1; col < 5; ++col) m_bpTable->item(i, col)->setFont(mono);
        auto *rm = new QToolButton;
        rm->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete"), QIcon::fromTheme(QStringLiteral("user-trash"))));
        if (rm->icon().isNull()) rm->setText(QStringLiteral("Remove"));
        rm->setAutoRaise(true);
        rm->setToolTip(QStringLiteral("Remove"));
        const int id = bps[i].id;
        connect(rm, &QToolButton::clicked, this, [this, id] {
            adamdebug_breakpoint_remove(m_dbg, id);
            syncLater();
        });
        m_bpTable->setCellWidget(i, 5, rm);
    }
    m_bpFilling = false;
    m_bpEmpty->setVisible(n == 0);
}

void DebuggerWindow::refreshIo()
{
    static char text[8192];
    text[0] = '\0';
    adamdebug_io_state(m_dbg, text, sizeof text);
    setText(m_io, stripControl(text));
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
void DebuggerWindow::refreshFujinet(bool force)
{
    static adam_fuji_packet p[TRACE_MAX];
    static char state[4096];
    const int n = adamdebug_fujinet_trace(m_dbg, p, TRACE_MAX);
    if (!adamsession_fujinet_running(m_session))
        m_fujiNote->setText(QStringLiteral("FujiNet is not running (FujiNet ▸ Console Log)."));
    else
        m_fujiNote->setText(QStringLiteral("FujiNet is on BoIP port %1, %2.")
                                .arg(ADAMSESSION_BOIP_PORT)
                                .arg(adamsession_fujinet_link(m_session) ? QStringLiteral("connected")
                                                                         : QStringLiteral("not connected")));
    state[0] = '\0';
    adamdebug_fujinet_state(m_dbg, state, int(sizeof state));
    setText(m_fujiState, stripControl(state));

    if (!force && n == m_fujiCount && (n == 0 || p[n - 1].serial == m_fujiLast)) return;
    const bool follow = force || m_fujiCount < 0 || atEnd(m_fujiText);
    m_fujiCount = n;
    m_fujiLast = n > 0 ? p[n - 1].serial : 0;
    QString t;
    for (int i = 0; i < n; ++i) {
        if (i) t += QLatin1Char('\n');
        t += QString::fromUtf8(p[i].text);
        t += QStringLiteral("\n     ");
        for (int k = 0; k < p[i].nbytes && k < int(sizeof p[i].data); k++) t += hx(p[i].data[k], 2) + QLatin1Char(' ');
        if (p[i].nbytes > int(sizeof p[i].data)) t += QStringLiteral("…");
    }
    if (n == 0) t = QStringLiteral("(no AdamNet packets since the trace was cleared)");
    setText(m_fujiText, t, follow);
}

/* One tab; live is the twice-a-second refresh while the machine runs, which
 * leaves out what only changes at a stop. */
void DebuggerWindow::refreshPage(int page, bool live)
{
    switch (page) {
    case TAB_CPU: refreshCpu(); refreshMem(); break;
    case TAB_DISASM: if (!live) refreshDisasm(); break;
    case TAB_VDP: refreshVdp(); break;
    case TAB_BREAKS: refreshBps(); break;
    case TAB_IO: refreshIo(); break;
    case TAB_FUJINET: refreshFujinet(false); break;
    default: break;
    }
}

void DebuggerWindow::refreshAll()
{
    refreshStatus();
    refreshPage(m_tabs->currentIndex(), false);
}

/* After anything that may have moved the engine: the console notes a new
 * stop, then the tab showing follows. */
void DebuggerWindow::resync()
{
    const bool stopped = adamdebug_is_stopped(m_dbg) != 0;
    const unsigned stops = g_stops.load(std::memory_order_relaxed);
    if (stopped && stops != m_seenStops) {
        char reason[160];
        int addr;
        reason[0] = '\0';
        adamdebug_stop_reason(m_dbg, reason, sizeof reason, &addr);
        /* a step already says where it is */
        if (reason[0] && strncmp(reason, "step", 4) != 0)
            consoleAppend(QStringLiteral("— %1\n").arg(stripControl(reason)));
    }
    m_seenStops = stops;
    m_wasStopped = stopped;
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
    for (int i = 0; i < 40 && !adamdebug_is_stopped(m_dbg); ++i) QThread::msleep(1);
    resync();
}

/* The engine reports a stop through the generation, so this polls it: a
 * refresh when it moved, and the live values twice a second while the
 * machine runs. */
void DebuggerWindow::tick()
{
    if (!isVisible()) return;
    const unsigned gen = adamdebug_generation(m_dbg);
    const bool stopped = adamdebug_is_stopped(m_dbg) != 0;
    if (gen != m_seenGen || stopped != m_wasStopped) {
        resync();
    } else if (!stopped && ++m_runningTicks >= 5) {
        m_runningTicks = 0;
        refreshStatus();
        refreshPage(m_tabs->currentIndex(), true);
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
    const int n = adamdebug_completions(m_dbg, text.toUtf8().constData(), comps, sizeof comps);
    if (n <= 0) return;
    const QStringList words = stripControl(comps).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (words.isEmpty()) return;
    const int sp = text.lastIndexOf(QLatin1Char(' '));
    const QString head = text.left(sp + 1);
    if (words.size() == 1) {
        m_conIn->setText(head + words[0] + QLatin1Char(' '));
        return;
    }
    QString common = words[0];
    for (const QString &w : words)
        while (!w.startsWith(common)) common.chop(1);
    consoleAppend(words.join(QStringLiteral("  ")) + QLatin1Char('\n'));
    if (common.size() > text.size() - sp - 1) m_conIn->setText(head + common);
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

void DebuggerWindow::stepAnd(const std::function<void()> &fn)
{
    if (!adamdebug_is_stopped(m_dbg)) {
        message(QStringLiteral("Stop the machine first"));
        return;
    }
    fn();
    settleAndSync();
}

void DebuggerWindow::runToCursor()
{
    if (m_selected < 0) {
        message(QStringLiteral("Select a line in the Disassembly to run to"));
        return;
    }
    stepAnd([this] { adamdebug_run_to(m_dbg, (uint16_t)m_selected); });
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

/* A number typed into an entry: a label first, then bare digits as hex (as in
 * the sibling debuggers), then whatever the engine evaluates ($C000, 0xC000,
 * C000h, hl+4, label+2; the console itself reads bare digits as decimal).
 * When it is none of those, the reason goes to *why, or to the status bar
 * without. */
bool DebuggerWindow::numberOf(const QString &text, long *out, QString *why)
{
    static const QRegularExpression hexOnly(QStringLiteral("^[0-9A-Fa-f]+$"));
    const QString t = text.trimmed();
    const QByteArray utf8 = t.toUtf8();
    long v = 0;
    QString reason;
    const int label = adamdebug_label_address(m_dbg, utf8.constData());
    if (label >= 0) {
        v = label;
    } else if (hexOnly.match(t).hasMatch()) {
        bool ok;
        v = t.toLong(&ok, 16);
        if (!ok) reason = QStringLiteral("Not a number");
    } else {
        char err[160] = "";
        if (adamdebug_eval(m_dbg, utf8.constData(), &v, err, sizeof err) != 0)
            reason = err[0] ? stripControl(err) : QStringLiteral("Not a number ($C000, or a label)");
    }
    if (reason.isEmpty()) {
        *out = v;
        return true;
    }
    if (why) *why = reason;
    else message(reason);
    return false;
}

/* The same, as an address: 0 to $FFFF. */
bool DebuggerWindow::addressOf(const QString &text, long *out, QString *why)
{
    long v;
    if (!numberOf(text, &v, why)) return false;
    if (v < 0 || v > 0xFFFF) {
        const QString reason = QStringLiteral("Out of range ($0000-$FFFF)");
        if (why) *why = reason;
        else message(reason);
        return false;
    }
    *out = v;
    return true;
}

/* Edits go in only while the machine is stopped. A register entry shows hex
 * digits, so bare digits there are hex; anything else is an expression. */
void DebuggerWindow::setReg(int i)
{
    QLineEdit *e = m_reg[i];
    long v = 0;
    QString why;
    /* IM is shown in decimal; the rest in hex, which is what bare digits are */
    bool ok;
    if (kRegs[i].digits) {
        ok = numberOf(e->text(), &v, &why);
    } else {
        char err[160] = "";
        ok = adamdebug_eval(m_dbg, e->text().trimmed().toUtf8().constData(), &v, err, sizeof err) == 0;
        if (!ok) why = err[0] ? stripControl(err) : QStringLiteral("Not a number");
    }
    if (!adamdebug_is_stopped(m_dbg)) message(QStringLiteral("Stop the machine first"));
    else if (!ok) message(why);
    else adamdebug_cpu_set(m_dbg, kRegs[i].id, (int)v);
    e->clearFocus();
    resync();
}

void DebuggerWindow::setFlag(int id, bool on)
{
    if (!adamdebug_is_stopped(m_dbg)) message(QStringLiteral("Stop the machine first"));
    else adamdebug_cpu_set(m_dbg, id, on ? 1 : 0);
    resync();
}

/* Go to: the dump from just above an address (a number or a label), its row
 * marked and scrolled to. */
void DebuggerWindow::memGoto()
{
    long a;
    if (!addressOf(m_memGoto->text(), &a)) {
        m_memMark = -1;
        refreshMem();
        return;
    }
    m_memMark = (int)a;
    m_memTop = qBound(0, (int)((a > 0x100 ? a - 0x100 : 0) & ~0xFL), 0x10000 - MEM_ROWS * 16);
    refreshMem();
    const int line = 1 + (m_memMark - m_memTop) / 16;
    const int shown = m_mem->viewport()->height() / qMax(1, m_mem->fontMetrics().lineSpacing());
    m_mem->verticalScrollBar()->setValue(qMax(0, line - shown * 3 / 10));
}

void DebuggerWindow::memWrite()
{
    long a, v;
    if (!addressOf(m_memAddr->text(), &a)) return;
    if (!numberOf(m_memVal->text(), &v)) return;
    if (v < 0 || v > 0xFF) {
        message(QStringLiteral("The value is a byte ($00-$FF)"));
        return;
    }
    adamdebug_write(m_dbg, (uint16_t)a, (uint8_t)v);
    resync();
}

void DebuggerWindow::jumpTo()
{
    long a;
    if (!addressOf(m_jump->text(), &a)) return;
    m_followPc->setChecked(false);
    m_selected = (int)a;
    m_disasmTop = (uint16_t)adamdebug_row_address(m_dbg, (uint16_t)a, -(visibleRows() / 3));
    refreshDisasm();
}

/* The type takes an address or label; in and out a port. The end is
 * optional; the condition is an expression the engine checks. */
void DebuggerWindow::addBreakpoint()
{
    static const int types[5] = { ADAMDEBUG_BP_EXEC, ADAMDEBUG_BP_READ, ADAMDEBUG_BP_WRITE,
                                  ADAMDEBUG_BP_IN, ADAMDEBUG_BP_OUT };
    const int sel = qBound(0, m_bpType->currentIndex(), 4);
    const QString start = m_bpStart->text().trimmed(), end = m_bpEnd->text().trimmed();
    if (start.isEmpty()) return;
    long a, b = 0;
    QString why;
    if (!addressOf(start, &a, &why) || (!end.isEmpty() && !addressOf(end, &b, &why))) {
        m_bpMsg->setText(why);
        return;
    }
    if (end.isEmpty()) b = a;
    if (b < a) {
        m_bpMsg->setText(QStringLiteral("The end is before the start"));
        return;
    }
    char err[256] = "";
    const QByteArray cond = m_bpCond->text().trimmed().toUtf8();
    if (adamdebug_breakpoint_add(m_dbg, types[sel], (uint16_t)a, (uint16_t)b, cond.constData(), err, sizeof err) < 0) {
        m_bpMsg->setText(err[0] ? stripControl(err) : QStringLiteral("The breakpoint could not be added"));
        return;
    }
    m_bpMsg->clear();
    m_bpStart->clear();
    m_bpEnd->clear();
    m_bpCond->clear();
    resync();
}

/* One entry does both jobs: an address alone scrolls the dump there, an
 * address followed by hex bytes writes them first ("1800: 41 42 43"). */
void DebuggerWindow::vramSubmit()
{
    uint8_t bytes[POKE_MAX];
    uint16_t addr = 0;
    const int n = adamvdp_parse_poke(m_vramEntry->text().toUtf8().constData(), &addr, bytes, POKE_MAX);
    if (n < 0) {
        m_vramStatus->setText(QStringLiteral("expected: addr [bytes...], e.g. 1800: 41 42"));
        return;
    }
    if (n > 0) {
        adamdebug_vdp_write(m_dbg, addr, bytes, n);
        m_vramStatus->setText(QStringLiteral("wrote %1 byte%2 at $%3").arg(n).arg(n == 1 ? "" : "s").arg(hx(addr, 4)));
        m_vramEntry->clear();
    } else {
        m_vramStatus->setText(QStringLiteral("$%1").arg(hx(addr, 4)));
    }
    m_vramBase = (uint16_t)(addr & 0x3FF0);
    refreshVdp();
}

void DebuggerWindow::vramPage(int delta)
{
    m_vramBase = (uint16_t)((m_vramBase + delta * VRAM_ROWS * 16) & 0x3FFF);
    refreshVdp();
}

/* A symbol file the user picks, or (fromFile false) the <cart>.sym/.map/.noi
 * beside the opened cartridge. */
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
    const QByteArray p = path.toLocal8Bit();
    adamdebug_load_symbols(m_dbg, fromFile ? p.constData() : nullptr, msg, sizeof msg);
    say(msg[0] ? stripControl(msg) : QStringLiteral("No symbols found"));
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

/* ---- events --------------------------------------------------------------- */

bool DebuggerWindow::eventFilter(QObject *obj, QEvent *e)
{
    /* Tab completes; Up and Down walk the history */
    if (obj == m_conIn && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Tab) {
            completeConsole();
            return true;
        }
        if (ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down) {
            if (ke->key() == Qt::Key_Up && m_historyPos > 0) --m_historyPos;
            else if (ke->key() == Qt::Key_Down && m_historyPos < m_history.size()) ++m_historyPos;
            m_conIn->setText(m_historyPos < m_history.size() ? m_history.at(m_historyPos) : QString());
            return true;
        }
    }
    if (m_disasm && obj == m_disasm->viewport()) {
        /* A click selects a line (Run to Cursor); a click on its left edge,
         * or a double-click, toggles its breakpoint. A label has no line of
         * its own, so there is nothing else to hit. */
        if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonDblClick) {
            auto *me = static_cast<QMouseEvent *>(e);
            if (me->button() != Qt::LeftButton) return true;
            const QTextCursor cur = m_disasm->cursorForPosition(me->position().toPoint());
            const int line = cur.blockNumber();
            if (line < 0 || line >= (int)m_lineAddr.size()) return true;
            const bool gutter = cur.positionInBlock() <= 2;
            const bool dbl = e->type() == QEvent::MouseButtonDblClick;
            m_selected = m_lineAddr[(size_t)line];
            if (gutter ? !dbl : dbl) toggleBreakpoint(m_lineAddr[(size_t)line]);
            else refreshDisasm();
            return true;
        }
        if (e->type() == QEvent::MouseButtonRelease || e->type() == QEvent::MouseMove) return true;
        if (e->type() == QEvent::Wheel) {
            m_wheel += static_cast<QWheelEvent *>(e)->angleDelta().y();
            const int rows = -m_wheel / 40;     /* a notch is three rows */
            if (rows != 0) {
                m_wheel += rows * 40;
                m_followPc->setChecked(false);
                m_disasmTop = (uint16_t)adamdebug_row_address(m_dbg, m_disasmTop, rows);
                refreshDisasm();
            }
            return true;
        }
        if (e->type() == QEvent::Resize && isVisible()) refreshDisasm();
    }
    return QMainWindow::eventFilter(obj, e);
}

/* These keys are the debugger's here, taken as plain key presses ahead of
 * any application-wide shortcut. */
bool DebuggerWindow::event(QEvent *e)
{
    if (e->type() == QEvent::ShortcutOverride && isDebuggerKey(static_cast<QKeyEvent *>(e))) {
        e->accept();
        return true;
    }
    return QMainWindow::event(e);
}

void DebuggerWindow::keyPressEvent(QKeyEvent *e)
{
    if (!isDebuggerKey(e)) {
        QMainWindow::keyPressEvent(e);
        return;
    }
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    /* held, the steps repeat; the others do not */
    if (e->isAutoRepeat() && (e->key() == Qt::Key_F5 || e->key() == Qt::Key_F9 || e->key() == Qt::Key_F12)) return;
    switch (e->key()) {
    case Qt::Key_F5: toggleRun(); break;
    case Qt::Key_F7: stepAnd(shift ? std::function<void()>([this] { adamdebug_frame(m_dbg, 1); })
                                   : std::function<void()>([this] { adamdebug_step(m_dbg); })); break;
    case Qt::Key_F8: stepAnd(shift ? std::function<void()>([this] { adamdebug_step_out(m_dbg); })
                                   : std::function<void()>([this] { adamdebug_step_over(m_dbg); })); break;
    case Qt::Key_F9: toggleSelected(); break;
    default: hide(); break;     /* F12 */
    }
}

/* Shown: the engine attaches, which stops the machine, as on every sibling.
 * The tabs have no size until the window is up, so the first refresh waits
 * for the event loop. A restore from the taskbar is not a show. */
void DebuggerWindow::showEvent(QShowEvent *e)
{
    QMainWindow::showEvent(e);
    if (e->spontaneous()) return;
    adamdebug_attach(m_dbg);
    m_wasStopped = false;
    QTimer::singleShot(0, this, [this] {
        if (!isVisible()) return;
        if (m_tabs->currentIndex() == TAB_CONSOLE) m_conIn->setFocus();
        settleAndSync();
    });
}

/* Hidden by F12, the close button or the main window: the engine goes away
 * and the machine runs on. The close button is a plain close, which only
 * hides the window (it is not deleted, so it comes back as it was);
 * ignoring the close instead would make QApplication's quit, which closes
 * every window first, give up whenever it reached this one. A minimised
 * window's spontaneous hide is not a detach. */
void DebuggerWindow::hideEvent(QHideEvent *e)
{
    if (!e->spontaneous()) adamdebug_detach(m_dbg);
    QMainWindow::hideEvent(e);
}

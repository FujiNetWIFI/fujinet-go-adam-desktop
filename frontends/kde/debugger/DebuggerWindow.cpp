/*
 * Debugger window (Qt6): disassembly with click-to-toggle breakpoints,
 * editable registers, memory view, breakpoints, instruction trace, and the
 * VDP visualizers, over the shared adamdebug engine. The engine's stop
 * callback fires on the emulator thread and is marshaled here through a
 * queued signal.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QGridLayout>
#include <QGroupBox>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QShortcut>
#include <QTextBlock>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

namespace {
constexpr int kDisasmLines = 40;
constexpr int kMemRows = 16;
const char *const kRegNames[8] = {"AF", "BC", "DE", "HL",
                                  "IX", "IY", "SP", "PC"};

quint16 regGet(const adamcore_z80_regs &r, int i)
{
    switch (i) {
    case 0: return quint16((r.a << 8) | r.f);
    case 1: return quint16((r.b << 8) | r.c);
    case 2: return quint16((r.d << 8) | r.e);
    case 3: return quint16((r.h << 8) | r.l);
    case 4: return r.ix;
    case 5: return r.iy;
    case 6: return r.sp;
    default: return r.pc;
    }
}

void regSet(adamcore_z80_regs &r, int i, quint16 v)
{
    switch (i) {
    case 0: r.a = quint8(v >> 8); r.f = quint8(v); break;
    case 1: r.b = quint8(v >> 8); r.c = quint8(v); break;
    case 2: r.d = quint8(v >> 8); r.e = quint8(v); break;
    case 3: r.h = quint8(v >> 8); r.l = quint8(v); break;
    case 4: r.ix = v; break;
    case 5: r.iy = v; break;
    case 6: r.sp = v; break;
    default: r.pc = v; break;
    }
}

QPlainTextEdit *monoView()
{
    auto *v = new QPlainTextEdit;
    v->setReadOnly(true);
    QFont f = v->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    v->setFont(f);
    v->setLineWrapMode(QPlainTextEdit::NoWrap);
    return v;
}

QPointer<DebuggerWindow> g_singleton;

void stop_trampoline(void *ud, adamdebug_stop_reason reason, uint16_t pc)
{
    auto *w = static_cast<DebuggerWindow *>(ud);
    /* Emulator thread -> UI thread via queued signal. */
    emit w->stopped(int(reason), pc);
}

} // namespace

void DebuggerWindow::showFor(QWidget *parent, adamsession *session)
{
    if (g_singleton) {
        g_singleton->raise();
        g_singleton->activateWindow();
        return;
    }
    g_singleton = new DebuggerWindow(parent, session);
    g_singleton->show();
}

DebuggerWindow::DebuggerWindow(QWidget *parent, adamsession *session)
    : QMainWindow(parent), m_session(session)
{
    m_dbg = adamsession_debugger(session);
    setWindowFlag(Qt::Window);
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("ADAM Debugger"));
    resize(1180, 820);
    buildUi();

    connect(this, &DebuggerWindow::stopped, this, &DebuggerWindow::onStopped,
            Qt::QueuedConnection);
    adamdebug_set_stop_callback(m_dbg, stop_trampoline, this);

    m_tick = new QTimer(this);
    connect(m_tick, &QTimer::timeout, this, [this] {
        if (!adamdebug_is_paused(m_dbg)) {
            refreshVdp();
            refreshRegs();
        }
    });
    m_tick->start(100);
    refreshAll();
}

DebuggerWindow::~DebuggerWindow()
{
    adamdebug_set_stop_callback(m_dbg, nullptr, nullptr);
}

void DebuggerWindow::buildUi()
{
    auto *tb = addToolBar(QStringLiteral("Control"));
    tb->setMovable(false);

    m_pauseBtn = new QPushButton(QStringLiteral("Pause (F5)"));
    connect(m_pauseBtn, &QPushButton::clicked, this,
            &DebuggerWindow::pauseContinue);
    tb->addWidget(m_pauseBtn);

    auto *stepIn = new QPushButton(QStringLiteral("Step Into (F7)"));
    connect(stepIn, &QPushButton::clicked, this,
            [this] { adamdebug_step_into(m_dbg); });
    tb->addWidget(stepIn);
    auto *stepOver = new QPushButton(QStringLiteral("Step Over (F8)"));
    connect(stepOver, &QPushButton::clicked, this,
            [this] { adamdebug_step_over(m_dbg); });
    tb->addWidget(stepOver);
    auto *stepOut = new QPushButton(QStringLiteral("Step Out (Shift+F8)"));
    connect(stepOut, &QPushButton::clicked, this,
            [this] { adamdebug_step_out(m_dbg); });
    tb->addWidget(stepOut);

    auto *gotoEdit = new QLineEdit;
    gotoEdit->setPlaceholderText(QStringLiteral("Go to addr / symbol"));
    gotoEdit->setMaximumWidth(180);
    connect(gotoEdit, &QLineEdit::returnPressed, this, [this, gotoEdit] {
        quint16 addr;
        if (parseAddr(gotoEdit->text(), &addr)) {
            m_followPc = false;
            m_disasmBase = addr;
            refreshDisasm();
        }
    });
    tb->addWidget(gotoEdit);

    auto *trace = new QCheckBox(QStringLiteral("Trace"));
    connect(trace, &QCheckBox::toggled, this, [this](bool on) {
        adamdebug_trace_enable(m_dbg, on ? 1 : 0);
    });
    tb->addWidget(trace);

    m_status = new QLabel(QStringLiteral("Running"));
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_status->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(m_status);

    new QShortcut(QKeySequence(Qt::Key_F5), this,
                  [this] { pauseContinue(); });
    new QShortcut(QKeySequence(Qt::Key_F7), this,
                  [this] { adamdebug_step_into(m_dbg); });
    new QShortcut(QKeySequence(Qt::Key_F8), this,
                  [this] { adamdebug_step_over(m_dbg); });
    new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F8), this,
                  [this] { adamdebug_step_out(m_dbg); });

    /* Central: disassembly. Click a line to toggle its breakpoint. */
    m_disasm = monoView();
    connect(m_disasm, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        const QString line = m_disasm->textCursor().block().text();
        bool ok = false;
        const quint16 addr =
            quint16(line.mid(2, 4).toUInt(&ok, 16));
        if (ok && m_disasm->hasFocus()) {
            adamdebug_bp_toggle(m_dbg, addr);
            refreshDisasm();
            refreshBps();
        }
    });
    setCentralWidget(m_disasm);

    /* Right dock: registers + breakpoints + memory. */
    auto *side = new QWidget;
    auto *sideLayout = new QVBoxLayout(side);

    auto *regBox =
        new QGroupBox(QStringLiteral("Registers (Enter applies while paused)"));
    auto *regGrid = new QGridLayout(regBox);
    for (int i = 0; i < 8; i++) {
        regGrid->addWidget(new QLabel(QLatin1String(kRegNames[i])),
                           i / 4, (i % 4) * 2);
        m_regEdit[i] = new QLineEdit;
        m_regEdit[i]->setMaximumWidth(64);
        const int idx = i;
        connect(m_regEdit[i], &QLineEdit::returnPressed, this, [this, idx] {
            if (!adamdebug_is_paused(m_dbg))
                return;
            bool ok = false;
            const quint16 v = quint16(m_regEdit[idx]->text().toUInt(&ok, 16));
            if (!ok)
                return;
            adamcore_z80_regs r;
            adamdebug_get_regs(m_dbg, &r);
            regSet(r, idx, v);
            adamdebug_set_regs(m_dbg, &r);
            refreshRegs();
            refreshDisasm();
        });
        regGrid->addWidget(m_regEdit[i], i / 4, (i % 4) * 2 + 1);
    }
    m_flags = new QLabel;
    regGrid->addWidget(m_flags, 2, 0, 1, 8);
    sideLayout->addWidget(regBox);

    auto *bpBox = new QGroupBox(QStringLiteral("Breakpoints"));
    auto *bpLayout = new QVBoxLayout(bpBox);
    auto *bpRow = new QWidget;
    auto *bpRowLayout = new QGridLayout(bpRow);
    bpRowLayout->setContentsMargins(0, 0, 0, 0);
    m_bpEntry = new QLineEdit;
    m_bpEntry->setPlaceholderText(QStringLiteral("Add: addr or symbol"));
    connect(m_bpEntry, &QLineEdit::returnPressed, this, [this] {
        quint16 addr;
        if (parseAddr(m_bpEntry->text(), &addr)) {
            adamdebug_bp_set(m_dbg, addr);
            m_bpEntry->clear();
            refreshBps();
            refreshDisasm();
        }
    });
    auto *bpClear = new QPushButton(QStringLiteral("Clear all"));
    connect(bpClear, &QPushButton::clicked, this, [this] {
        adamdebug_bp_clear_all(m_dbg);
        refreshBps();
        refreshDisasm();
    });
    bpRowLayout->addWidget(m_bpEntry, 0, 0);
    bpRowLayout->addWidget(bpClear, 0, 1);
    bpLayout->addWidget(bpRow);
    m_bpView = monoView();
    m_bpView->setMaximumHeight(110);
    bpLayout->addWidget(m_bpView);
    sideLayout->addWidget(bpBox);

    auto *memBox = new QGroupBox(QStringLiteral("Memory"));
    auto *memLayout = new QVBoxLayout(memBox);
    m_memAddr = new QLineEdit;
    m_memAddr->setPlaceholderText(QStringLiteral("Memory addr / symbol"));
    connect(m_memAddr, &QLineEdit::returnPressed, this, [this] {
        quint16 addr;
        if (parseAddr(m_memAddr->text(), &addr)) {
            m_memBase = addr;
            refreshMem();
        }
    });
    memLayout->addWidget(m_memAddr);
    m_memView = monoView();
    memLayout->addWidget(m_memView);
    sideLayout->addWidget(memBox, 1);

    auto *sideDock = new QDockWidget(QStringLiteral("CPU State"), this);
    sideDock->setWidget(side);
    sideDock->setFeatures(QDockWidget::DockWidgetMovable |
                          QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::RightDockWidgetArea, sideDock);

    /* Bottom docks: VDP + trace (tabbed). */
    auto *vdp = new QWidget;
    auto *vdpGrid = new QGridLayout(vdp);
    auto make_pic = [](QLabel **out, int w, int h) {
        auto *l = new QLabel;
        l->setMinimumSize(w, h);
        l->setScaledContents(true);
        *out = l;
        return l;
    };
    vdpGrid->addWidget(new QLabel(QStringLiteral("Nametable")), 0, 0);
    vdpGrid->addWidget(make_pic(&m_nt, 512, 384), 1, 0, 3, 1);
    m_patBank = new QComboBox;
    m_patBank->addItems({QStringLiteral("Bank 0"), QStringLiteral("Bank 1"),
                         QStringLiteral("Bank 2")});
    vdpGrid->addWidget(new QLabel(QStringLiteral("Patterns")), 0, 1);
    vdpGrid->addWidget(m_patBank, 1, 1);
    vdpGrid->addWidget(make_pic(&m_pat, 512, 128), 2, 1);
    vdpGrid->addWidget(new QLabel(QStringLiteral("Sprites")), 0, 2);
    vdpGrid->addWidget(make_pic(&m_spr, 256, 128), 2, 2);
    m_spriteInfo = monoView();
    m_spriteInfo->setMinimumWidth(300);
    vdpGrid->addWidget(m_spriteInfo, 1, 3, 3, 1);
    vdpGrid->addWidget(new QLabel(QStringLiteral("Palette")), 4, 0);
    vdpGrid->addWidget(make_pic(&m_pal, 256, 16), 5, 0);

    auto *vdpDock = new QDockWidget(QStringLiteral("VDP"), this);
    vdpDock->setWidget(vdp);
    addDockWidget(Qt::BottomDockWidgetArea, vdpDock);

    m_traceView = monoView();
    auto *traceDock = new QDockWidget(QStringLiteral("Trace"), this);
    traceDock->setWidget(m_traceView);
    addDockWidget(Qt::BottomDockWidgetArea, traceDock);
    tabifyDockWidget(vdpDock, traceDock);
    vdpDock->raise();
}

bool DebuggerWindow::parseAddr(const QString &text, quint16 *out)
{
    QString t = text.trimmed();
    if (t.startsWith(QLatin1Char('$')))
        t = t.mid(1);
    bool ok = false;
    const uint v = t.toUInt(&ok, 16);
    if (ok && v <= 0xFFFF) {
        *out = quint16(v);
        return true;
    }
    uint16_t addr;
    if (adamdebug_symbol_find(m_dbg, text.trimmed().toUtf8().constData(),
                              &addr)) {
        *out = addr;
        return true;
    }
    return false;
}

void DebuggerWindow::pauseContinue()
{
    if (adamdebug_is_paused(m_dbg)) {
        adamdebug_resume(m_dbg);
        m_status->setText(QStringLiteral("Running"));
        refreshAll();
    } else {
        adamdebug_pause(m_dbg);
    }
}

void DebuggerWindow::onStopped(int reason, quint16 pc)
{
    static const char *const names[] = {"paused", "breakpoint", "step",
                                        "run-to"};
    uint16_t off = 0;
    const char *sym = adamdebug_symbol_at(m_dbg, pc, &off);
    QString text = QStringLiteral("Stopped (%1) at %2")
                       .arg(QLatin1String(names[reason]))
                       .arg(pc, 4, 16, QLatin1Char('0'));
    if (sym)
        text += QStringLiteral("  %1+%2")
                    .arg(QLatin1String(sym))
                    .arg(off, 0, 16);
    m_status->setText(text);
    m_followPc = true;
    refreshAll();
}

void DebuggerWindow::refreshAll()
{
    refreshDisasm();
    refreshRegs();
    refreshMem();
    refreshBps();
    refreshTrace();
    refreshVdp();
    m_pauseBtn->setText(adamdebug_is_paused(m_dbg)
                            ? QStringLiteral("Continue (F5)")
                            : QStringLiteral("Pause (F5)"));
}

void DebuggerWindow::refreshDisasm()
{
    adamdasm_line lines[kDisasmLines];
    adamcore_z80_regs r;
    adamdebug_get_regs(m_dbg, &r);
    if (m_followPc)
        m_disasmBase = r.pc;

    const int n =
        adamdebug_disassemble(m_dbg, m_disasmBase, kDisasmLines, lines);
    QString text;
    for (int i = 0; i < n; i++) {
        if (lines[i].symbol)
            text += QStringLiteral("%1%2:\n")
                        .arg(QString(), 17)
                        .arg(QLatin1String(lines[i].symbol));
        QString bytes;
        for (int b = 0; b < lines[i].len; b++)
            bytes += QStringLiteral("%1 ").arg(lines[i].bytes[b], 2, 16,
                                               QLatin1Char('0')).toUpper();
        text += QStringLiteral("%1%2%3  %4 %5\n")
                    .arg(adamdebug_bp_is_set(m_dbg, lines[i].addr)
                             ? QLatin1Char('*')
                             : QLatin1Char(' '))
                    .arg(lines[i].addr == r.pc ? QLatin1Char('>')
                                               : QLatin1Char(' '))
                    .arg(lines[i].addr, 4, 16, QLatin1Char('0'))
                    .arg(bytes, -13)
                    .arg(QLatin1String(lines[i].text));
    }
    /* setPlainText would emit cursorPositionChanged (the bp toggler); the
     * view doesn't have focus during programmatic updates, which the
     * toggler checks, so this is safe. */
    m_disasm->setPlainText(text.toUpper());
}

void DebuggerWindow::refreshRegs()
{
    adamcore_z80_regs r;
    adamdebug_get_regs(m_dbg, &r);
    for (int i = 0; i < 8; i++)
        if (!m_regEdit[i]->hasFocus())
            m_regEdit[i]->setText(QStringLiteral("%1").arg(
                regGet(r, i), 4, 16, QLatin1Char('0')).toUpper());
    m_flags->setText(
        QStringLiteral("F: %1%2%3%4%5%6  IFF1:%7 IM:%8  cyc:%9")
            .arg(QLatin1Char((r.f & 0x80) ? 'S' : '-'))
            .arg(QLatin1Char((r.f & 0x40) ? 'Z' : '-'))
            .arg(QLatin1Char((r.f & 0x10) ? 'H' : '-'))
            .arg(QLatin1Char((r.f & 0x04) ? 'P' : '-'))
            .arg(QLatin1Char((r.f & 0x02) ? 'N' : '-'))
            .arg(QLatin1Char((r.f & 0x01) ? 'C' : '-'))
            .arg(r.iff1)
            .arg(r.im)
            .arg(r.cycles));
}

void DebuggerWindow::refreshMem()
{
    uint8_t data[kMemRows * 16];
    adamdebug_read_mem(m_dbg, m_memBase, data, sizeof(data));
    QString text;
    for (int row = 0; row < kMemRows; row++) {
        text += QStringLiteral("%1  ").arg(quint16(m_memBase + row * 16), 4,
                                           16, QLatin1Char('0')).toUpper();
        QString ascii;
        for (int col = 0; col < 16; col++) {
            const uint8_t c = data[row * 16 + col];
            text += QStringLiteral("%1 ").arg(c, 2, 16, QLatin1Char('0'))
                        .toUpper();
            ascii += (c >= 0x20 && c <= 0x7E) ? QLatin1Char(char(c))
                                              : QLatin1Char('.');
        }
        text += QLatin1Char(' ') + ascii + QLatin1Char('\n');
    }
    m_memView->setPlainText(text);
}

void DebuggerWindow::refreshBps()
{
    uint16_t bps[128];
    const int n = adamdebug_bp_list(m_dbg, bps, 128);
    QString text = n ? QString() : QStringLiteral("(no breakpoints)");
    for (int i = 0; i < n; i++) {
        uint16_t off = 0;
        const char *sym = adamdebug_symbol_at(m_dbg, bps[i], &off);
        text += QStringLiteral("%1").arg(bps[i], 4, 16, QLatin1Char('0'))
                    .toUpper();
        if (sym)
            text += QStringLiteral("  %1+%2").arg(QLatin1String(sym))
                        .arg(off, 0, 16);
        text += QLatin1Char('\n');
    }
    m_bpView->setPlainText(text);
}

void DebuggerWindow::refreshTrace()
{
    static adamtrace_entry entries[256];
    const int n = adamdebug_trace_read(m_dbg, entries, 256);
    QString text = n ? QStringLiteral("  PC    AF   BC   DE   HL   SP\n")
                     : QStringLiteral("(trace empty; enable tracing and run)");
    for (int i = 0; i < n; i++)
        text += QStringLiteral("%1  %2 %3 %4 %5 %6\n")
                    .arg(entries[i].pc, 4, 16, QLatin1Char('0'))
                    .arg(entries[i].af, 4, 16, QLatin1Char('0'))
                    .arg(entries[i].bc, 4, 16, QLatin1Char('0'))
                    .arg(entries[i].de, 4, 16, QLatin1Char('0'))
                    .arg(entries[i].hl, 4, 16, QLatin1Char('0'))
                    .arg(entries[i].sp, 4, 16, QLatin1Char('0'))
                    .toUpper();
    m_traceView->setPlainText(text);
}

void DebuggerWindow::refreshVdp()
{
    static adamvdp_snapshot snap;
    static uint8_t nt[256 * 192 * 4], pat[256 * 64 * 4], spr[128 * 64 * 4],
        pal[16 * 4];
    adamvdp_sprite info[32];

    adamdebug_vdp_snapshot(m_dbg, &snap);

    adamvdp_render_nametable(&snap, nt);
    m_nt->setPixmap(QPixmap::fromImage(
        QImage(nt, 256, 192, 256 * 4, QImage::Format_RGBA8888)));

    adamvdp_render_patterns(&snap, m_patBank->currentIndex(), pat);
    m_pat->setPixmap(QPixmap::fromImage(
        QImage(pat, 256, 64, 256 * 4, QImage::Format_RGBA8888)));

    adamvdp_render_sprites(&snap, spr, info);
    m_spr->setPixmap(QPixmap::fromImage(
        QImage(spr, 128, 64, 128 * 4, QImage::Format_RGBA8888)));

    adamvdp_render_palette(&snap, pal);
    m_pal->setPixmap(QPixmap::fromImage(
        QImage(pal, 16, 1, 16 * 4, QImage::Format_RGBA8888)));

    QString text = QStringLiteral("##  Y    X  PAT CLR EC   R0-R7: ");
    for (int i = 0; i < 8; i++)
        text += QStringLiteral("%1 ").arg(snap.regs[i], 2, 16,
                                          QLatin1Char('0')).toUpper();
    text += QStringLiteral(" ST:%1\n").arg(snap.status, 2, 16,
                                           QLatin1Char('0')).toUpper();
    for (int i = 0; i < 32; i++)
        text += QStringLiteral("%1 %2 %3  %4  %5  %6\n")
                    .arg(i, 2)
                    .arg(info[i].y, 4)
                    .arg(info[i].x, 4)
                    .arg(info[i].pattern, 3, 16)
                    .arg(info[i].color, 3)
                    .arg(info[i].early_clock);
    m_spriteInfo->setPlainText(text);
}

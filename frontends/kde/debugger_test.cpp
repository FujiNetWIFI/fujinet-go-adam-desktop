/*
 * debugger_test -- the Debugger window (debugger/DebuggerWindow.cpp) driven
 * with synthetic events on the offscreen platform against a running
 * ColecoVision session whose cartridge is a hand-assembled loop (no ROMs
 * beyond the embedded ones; the real ADAM ROMs are not shippable): the twelve
 * tabs and their order, the ADAM_DEBUGGER_TAB names and the older aliases, the
 * Help contexts, the minimum size, attach/detach with show and hide, F12, the
 * stepping keys and buttons, the console (transcript, Tab completion,
 * history), the disassembly (the PC's line, click / double-click / F9 for a
 * breakpoint, selection and Run to Cursor, the wheel, the jump box), the
 * register strip, Memory (Go to, Poke, search), Breakpoints (every type, an
 * expression condition), Watches, Trace and History, Video, Sound, Symbols,
 * Cart & I/O, FujiNet, pop-out and dock, the env hooks, F12 and the View menu
 * from the main window and a session restart. A small program of NOP-padded
 * instructions is written into the console RAM and stepped through the window.
 *
 * DBG_TEST_SHOTS=<dir> also grabs a PNG of each tab (DBG_TEST_LIGHT=1 /
 * DBG_TEST_DARK=1: in that scheme).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QOpenGLContext>
#include <QTemporaryDir>
#include <QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStatusBar>
#include <QStyleHints>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QToolButton>

#include "FujiNetWindows.h"
#include "adamdebug.h"
#include "adammemtools.h"
#include "adamsession.h"
#include "DisplayWidget.h"
#include "MainWindow.h"
#include "debugger/Accent.h"
#include "debugger/DebuggerWindow.h"
#include "debugger/VideoTab.h"

namespace {

/* A ColecoVision image whose program loops forever:
 *   8024  LD SP,$6400
 *   8027  LD HL,$6000
 *   802A  XOR A
 *   802B  LD B,3
 *   802D  CALL $8050     ; INC C / RET
 *   8030  DJNZ $802D
 *   8032  INC A
 *   8033  LD (HL),A      ; write $6000
 *   8034  OUT ($FF),A
 *   8036  IN A,($FC)
 *   8038  LD A,(HL)      ; read $6000
 *   8039  JP $802B */
void makeCart(const QString &path)
{
    static const uint8_t prog[] = {
        0x31, 0x00, 0x64, 0x21, 0x00, 0x60, 0xAF, 0x06, 0x03,
        0xCD, 0x50, 0x80, 0x10, 0xFB, 0x3C, 0x77, 0xD3, 0xFF,
        0xDB, 0xFC, 0x7E, 0xC3, 0x2B, 0x80,
    };
    uint8_t img[128] = {};
    img[0] = 0x55;
    img[1] = 0xAA;
    img[0x0A] = 0x24;
    img[0x0B] = 0x80;
    for (int i = 0x0C; i < 0x21; i++) img[i] = 0xC9;
    img[0x21] = 0xED;
    img[0x22] = 0x45;
    memcpy(img + 0x24, prog, sizeof prog);
    img[0x50] = 0x0C;
    img[0x51] = 0xC9;
    QFile f(path);
    QVERIFY2(f.open(QIODevice::WriteOnly), "cart file");
    f.write(reinterpret_cast<const char *>(img), sizeof img);
}

int pcOf(adamdebug *d)
{
    adamdebug_cpu c;
    adamdebug_cpu_get(d, &c);
    return c.pc;
}

/* In the 1K console RAM at $6100 (the cart's own loop uses $6000 and the stack
 * below $6400), NOP-padded so a backward disassembly guess is stable:
 * NOP x3, LD HL,$1234, NOP, LD ($6200),HL, NOP x3. */
const uint8_t kProg[] = { 0x00, 0x00, 0x00, 0x21, 0x34, 0x12, 0x00, 0x22, 0x00, 0x62, 0x00, 0x00, 0x00 };
const int kStart = 0x6100;

QStringList expectedTabs()
{
    return { QStringLiteral("Console"), QStringLiteral("Disassembly"), QStringLiteral("Memory"),
             QStringLiteral("Breakpoints"), QStringLiteral("Watches"), QStringLiteral("Cart & I/O"),
             QStringLiteral("Trace"), QStringLiteral("History"), QStringLiteral("Video"),
             QStringLiteral("Sound"), QStringLiteral("Symbols"), QStringLiteral("FujiNet") };
}

} // namespace

class DebuggerTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    adamsession *m_s = nullptr;
    adamdebug *m_d = nullptr;
    QMainWindow *m_win = nullptr;
    QTabWidget *m_tabs = nullptr;
    QString m_shots;
    QByteArray m_cartPath;
    adamsession_start_opts m_opts = {};

    static QMainWindow *findWindow()
    {
        for (QWidget *top : QApplication::topLevelWidgets())
            if (top->objectName() == QLatin1String("debugger")) return qobject_cast<QMainWindow *>(top);
        return nullptr;
    }
    /* the window is kept when closed: hide it (which detaches the engine) and
     * delete it, so showFor() builds a fresh one that reads the env hooks */
    void discardWindow()
    {
        if (m_win) {
            m_win->hide();
            m_win->deleteLater();
        }
        m_win = nullptr;
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void openWindow()
    {
        DebuggerWindow::showFor(nullptr, m_s);
        m_win = findWindow();
        QVERIFY(m_win);
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
        m_tabs = m_win->findChild<QTabWidget *>();
        QVERIFY(m_tabs);
        QTRY_VERIFY_WITH_TIMEOUT(adamdebug_is_attached(m_d), 2000);
        waitStopped();
    }
    QString tabText(int idx) const { return m_tabs->tabText(idx).replace(QStringLiteral("&&"), QStringLiteral("&")); }

    QPushButton *button(const QString &textStart)
    {
        for (QPushButton *b : m_win->findChildren<QPushButton *>())
            if (b->text().startsWith(textStart)) return b;
        return nullptr;
    }
    QLineEdit *entry(const QString &placeholderStart)
    {
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->placeholderText().startsWith(placeholderStart)) return e;
        return nullptr;
    }
    QPushButton *addButton(int tab)   /* the tab's own Add */
    {
        for (QPushButton *btn : m_tabs->widget(tab)->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Add")) return btn;
        return nullptr;
    }
    QPlainTextEdit *disasmView()
    {
        for (QPlainTextEdit *v : m_win->findChildren<QPlainTextEdit *>())
            if (v->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff) return v;
        return nullptr;
    }
    QPlainTextEdit *consoleView() { return m_tabs->widget(0)->findChild<QPlainTextEdit *>(); }
    QString consoleText()
    {
        auto *view = consoleView();
        return view ? view->toPlainText() : QString();
    }
    QLabel *statusLabel()
    {
        for (QLabel *l : m_win->findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("Stopped")) || l->text() == QLatin1String("Running")) return l;
        return nullptr;
    }
    QCheckBox *check(const QString &text)
    {
        for (QCheckBox *c : m_win->findChildren<QCheckBox *>())
            if (c->text() == text) return c;
        return nullptr;
    }
    QList<QLineEdit *> regFields()
    {
        QList<QLineEdit *> out;
        for (QLineEdit *edit : m_win->findChildren<QLineEdit *>())
            if (edit->toolTip().startsWith(QLatin1String("Enter sets it"))) out << edit;
        return out;
    }
    QTableWidget *bpTable() { return m_tabs->widget(3)->findChild<QTableWidget *>(); }
    adamdebug_cpu cpuNow()
    {
        adamdebug_cpu c;
        adamdebug_cpu_get(m_d, &c);
        return c;
    }

    void waitStopped()
    {
        QTRY_VERIFY_WITH_TIMEOUT(adamdebug_is_stopped(m_d), 4000);
        /* the window follows within its poll */
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel() && statusLabel()->text().startsWith(QLatin1String("Stopped")), 3000);
    }
    void shot(const QString &name)
    {
        if (!m_shots.isEmpty()) m_win->grab().save(m_shots + QLatin1Char('/') + name + QStringLiteral(".png"));
    }

    /* The block of the disassembly that shows an address, or -1. */
    int lineOf(QPlainTextEdit *v, int addr)
    {
        const QString want = QStringLiteral("%1").arg(addr, 4, 16, QLatin1Char('0')).toUpper();
        int n = 0;
        for (QTextBlock b = v->document()->begin(); b.isValid(); b = b.next(), ++n)
            if (b.text().mid(3, 4) == want) return n;
        return -1;
    }
    QPoint lineAt(QPlainTextEdit *v, int line, int column)
    {
        QTextCursor c(v->document()->findBlockByNumber(line));
        c.setPosition(c.block().position() + column);
        const QRect r = v->cursorRect(c);
        return QPoint(r.left() + 1, r.center().y());
    }
    void clickLine(int addr, int column, bool dbl = false)
    {
        QPlainTextEdit *v = disasmView();
        const int line = lineOf(v, addr);
        QVERIFY2(line >= 0, qPrintable(QStringLiteral("line for %1").arg(addr, 0, 16)));
        const QPoint p = lineAt(v, line, column);
        if (dbl) QTest::mouseDClick(v->viewport(), Qt::LeftButton, {}, p);
        else QTest::mouseClick(v->viewport(), Qt::LeftButton, {}, p);
    }
    void typeInto(QLineEdit *e, const QString &text)
    {
        e->clear();
        QTest::keyClicks(e, text);
        QTest::keyClick(e, Qt::Key_Return);
    }
    /* The program in console RAM, started: interrupts off, no HALT, the
     * stack in RAM. */
    bool loadProgram()
    {
        uint8_t back[sizeof kProg];
        char err[200] = "";
        for (size_t at = 0; at < sizeof kProg; ++at) adamdebug_write(m_d, uint16_t(kStart + at), kProg[at]);
        if (adamdebug_read(m_d, kStart, back, int(sizeof kProg)) != int(sizeof kProg) || memcmp(back, kProg, sizeof kProg))
            return false;
        adamcore_z80_regs regs;
        memset(&regs, 0, sizeof regs);
        const adamdebug_cpu cpu = cpuNow();
        regs.a = uint8_t(cpu.af >> 8);
        regs.f = uint8_t(cpu.af);
        regs.b = uint8_t(cpu.bc >> 8);
        regs.c = uint8_t(cpu.bc);
        regs.d = uint8_t(cpu.de >> 8);
        regs.e = uint8_t(cpu.de);
        regs.h = uint8_t(cpu.hl >> 8);
        regs.l = uint8_t(cpu.hl);
        regs.ix = uint16_t(cpu.ix);
        regs.iy = uint16_t(cpu.iy);
        regs.sp = 0x6380;
        regs.im = uint8_t(cpu.im);
        regs.iff1 = regs.iff2 = 0;
        regs.halted = 0;
        regs.pc = kStart;
        return adamdebug_regs_set(m_d, &regs, err, sizeof err) && pcOf(m_d) == kStart;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qputenv("SDL_AUDIODRIVER", "dummy");
        if (qEnvironmentVariableIsSet("DBG_TEST_SHOTS")) m_shots = qEnvironmentVariable("DBG_TEST_SHOTS");
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        if (qEnvironmentVariableIsSet("DBG_TEST_LIGHT"))
            QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
        if (qEnvironmentVariableIsSet("DBG_TEST_DARK"))
            QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
#endif
        adamsession_paths paths = {};
        const QByteArray dir = m_dir.path().toUtf8();
        paths.config_dir = dir.constData();
        paths.data_dir = dir.constData();
        paths.fujinet_lib = "";
        m_s = adamsession_new(&paths);
        QVERIFY(m_s);
        m_d = adamsession_debugger(m_s);
        QVERIFY(m_d);

        const QString cart = m_dir.path() + QStringLiteral("/loop.rom");
        makeCart(cart);
        m_cartPath = cart.toUtf8();
        adamsession_default_opts(m_s, &m_opts);
        m_opts.machine = 1;
        m_opts.cart_path = m_cartPath.constData();
        QCOMPARE(adamsession_start(m_s, &m_opts), 0);

        /* the window opens on the tab ADAM_DEBUGGER_TAB names (the older "vdp" is the Video tab) */
        qputenv("ADAM_DEBUGGER_TAB", "vdp");
        DebuggerWindow::showFor(nullptr, m_s);
        qunsetenv("ADAM_DEBUGGER_TAB");
        m_win = findWindow();
        QVERIFY(m_win);
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
        m_tabs = m_win->findChild<QTabWidget *>();
        QVERIFY(m_tabs);
        QCOMPARE(m_tabs->currentIndex(), 8);
        waitStopped();
    }

    void cleanupTestCase()
    {
        delete m_win;       /* detaches */
        adamsession_stop(m_s);
        adamsession_free(m_s);
    }

    void init()
    {
        if (!m_win->isVisible()) DebuggerWindow::toggleFor(nullptr, m_s);
        QTRY_VERIFY_WITH_TIMEOUT(adamdebug_is_attached(m_d), 2000);
        if (!adamdebug_is_stopped(m_d)) adamdebug_stop(m_d);
        waitStopped();
        adamdebug_breakpoint_clear(m_d);
        adamdebug_cpu_set(m_d, ADAM_REG_PC, 0x802B);
        adamdebug_cpu_set(m_d, ADAM_REG_B, 3);
        adamdebug_cpu_set(m_d, ADAM_REG_HL, 0x6000);     /* the loop's own pointer and stack */
        adamdebug_cpu_set(m_d, ADAM_REG_SP, 0x6400);
    }

    void tabsInOrder()
    {
        QStringList names;
        for (int idx = 0; idx < m_tabs->count(); ++idx) names << tabText(idx);
        QCOMPARE(names, expectedTabs());
        QCOMPARE(m_tabs->currentIndex(), 8);   /* "vdp" is the Video tab */
        QVERIFY(m_win->findChild<VideoTab *>() != nullptr);
        QVERIFY(adamdebug_is_attached(m_d));
        QVERIFY(adamdebug_is_stopped(m_d));
        /* the register strip is under every tab, not in one */
        const QList<QLineEdit *> regs = regFields();
        QCOMPARE(regs.size(), 15);
        for (QLineEdit *edit : regs) QVERIFY(!m_tabs->isAncestorOf(edit));
        /* Run is the accent while stopped, whichever text it wears */
        QPushButton *run = button(QStringLiteral("Run (F5)"));
        QVERIFY(run);
        QVERIFY(run->styleSheet().contains(adamDebugAccentFill(run).name()));
        QTest::mouseClick(run, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!adamdebug_is_stopped(m_d), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(run->text() == QLatin1String("Stop (F5)"), 2000);
        QVERIFY(run->styleSheet().isEmpty());
        QCOMPARE(statusLabel()->text(), QStringLiteral("Running"));
        QTest::mouseClick(run, Qt::LeftButton);
        waitStopped();
        QVERIFY(run->styleSheet().contains(adamDebugAccentFill(run).name()));
        QVERIFY(run->styleSheet().contains(adamDebugAccentFillInk(run).name()));
    }

    void tabsByName()
    {
        /* each name the env hook takes, in a window of its own */
        discardWindow();
        struct { const char *name; int index; } cases[] = {
            { "cpu", 1 }, { "registers", 1 }, { "io", 5 }, { "vram", 8 }, { "CART", 5 }, { "3", 3 },
            { "Sound", 9 }, { "console", 0 }, { "hist", 7 }, { "sym", 10 }, { "fuji", 11 }, { "disassembly", 1 } };
        for (const auto &one : cases) {
            qputenv("ADAM_DEBUGGER_TAB", one.name);
            DebuggerWindow::showFor(nullptr, m_s);
            qunsetenv("ADAM_DEBUGGER_TAB");
            QMainWindow *win = findWindow();
            QVERIFY(win);
            QVERIFY2(win->findChild<QTabWidget *>()->currentIndex() == one.index, one.name);
            m_win = win;
            discardWindow();
        }
        openWindow();
    }

    void helpContexts()
    {
        static const char *const kContexts[] = { "console", "disassembly", "memory", "breakpoints", "watches", "cart",
                                                 "trace", "history", "video", "sound", "symbols", "fujinet" };
        for (int idx = 0; idx < 12; ++idx) {
            m_tabs->setCurrentIndex(idx);
            QCOMPARE(m_win->property("adamHelpContext").toString(),
                     QStringLiteral("debugger/%1").arg(QLatin1String(kContexts[idx])));
        }
    }

    void fitsASmallScreen()
    {
        QVERIFY2(m_win->minimumSizeHint().height() <= 800,
                 qPrintable(QStringLiteral("minimum height %1").arg(m_win->minimumSizeHint().height())));
        QVERIFY2(m_win->minimumSizeHint().width() <= 950,
                 qPrintable(QStringLiteral("minimum width %1").arg(m_win->minimumSizeHint().width())));
    }

    /* the built-in EOS and OS7 names fill the list the first time the tab opens */
    void symbolsFillOnFirstOpen()
    {
        m_tabs->setCurrentIndex(10);
        QListWidget *list = m_tabs->widget(10)->findChild<QListWidget *>();
        QVERIFY(list);
        QTRY_VERIFY_WITH_TIMEOUT(list->count() > 0, 2000);
        QLineEdit *filter = m_tabs->widget(10)->findChild<QLineEdit *>();
        QVERIFY(filter);
        const int all = list->count();
        filter->setText(QStringLiteral("WRITE_VRAM"));
        QVERIFY(list->count() > 0 && list->count() < all);
        QVERIFY(list->item(0)->text().contains(QStringLiteral("WRITE_VRAM")));
        /* Break at toggles an execute breakpoint on the selected symbol */
        list->setCurrentRow(0);
        const unsigned addr = list->item(0)->data(Qt::UserRole).toUInt();
        QPushButton *breakAt = nullptr;
        for (QPushButton *btn : m_tabs->widget(10)->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Break at")) breakAt = btn;
        QVERIFY(breakAt);
        breakAt->click();
        QVERIFY(adamdebug_breakpoint_check(m_d, uint16_t(addr)));
        breakAt->click();
        QVERIFY(!adamdebug_breakpoint_check(m_d, uint16_t(addr)));
        filter->clear();
        QCOMPARE(list->count(), all);
    }

    void popOutAndDock()
    {
        m_tabs->setCurrentIndex(9);
        QPushButton *pop = button(QStringLiteral("Pop Out"));
        QVERIFY(pop);
        pop->click();
        QCOMPARE(m_tabs->count(), 11);
        QWidget *window = nullptr;
        for (QWidget *top : QApplication::topLevelWidgets())
            if (top->windowTitle().endsWith(QLatin1String("Sound"))) window = top;
        QVERIFY(window);
        for (QPushButton *btn : window->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Dock")) btn->click();
        QCOMPARE(m_tabs->count(), 12);
        QCOMPARE(tabText(9), QStringLiteral("Sound"));
    }

    void stepKeys()
    {
        QCOMPARE(pcOf(m_d), 0x802B);
        QTest::keyClick(m_win, Qt::Key_F7);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x802D, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(statusLabel()->text().left(13), QStringLiteral("Stopped: step"), 2000);
        /* F8 steps over the CALL */
        QTest::keyClick(m_win, Qt::Key_F8);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x8030, 2000);
        /* the buttons do the same */
        adamdebug_write(m_d, 0x63FE, 0x30);          /* a return address, as a CALL left it */
        adamdebug_write(m_d, 0x63FF, 0x80);
        adamdebug_cpu_set(m_d, ADAM_REG_SP, 0x63FE);
        adamdebug_cpu_set(m_d, ADAM_REG_PC, 0x8050);
        QTest::mouseClick(button(QStringLiteral("Step Out")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x8030, 2000);
        /* Frame: Shift+F7 lands on a frame boundary */
        const adamdebug_cpu before = cpuNow();
        QTest::keyClick(m_win, Qt::Key_F7, Qt::ShiftModifier);
        QTRY_VERIFY_WITH_TIMEOUT(adamdebug_is_stopped(m_d) && cpuNow().frame > before.frame, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("frame")), 2000);
        /* stepping while running is refused with a word, not silently */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!adamdebug_is_stopped(m_d), 2000);
        QTest::keyClick(m_win, Qt::Key_F7);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Stop the machine")) ||
                statusLabel()->text() == QLatin1String("Running"));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
    }

    void consoleCommands()
    {
        m_tabs->setCurrentIndex(0);
        QLineEdit *in = entry(QStringLiteral("command"));
        QPlainTextEdit *out = consoleView();
        QVERIFY(in && out);
        typeInto(in, QStringLiteral("regs"));
        QVERIFY(out->toPlainText().contains(QStringLiteral("> regs")));
        QVERIFY(out->toPlainText().contains(QStringLiteral("flags")));
        QVERIFY(in->text().isEmpty());
        typeInto(in, QStringLiteral("print $10 + 2"));
        QTRY_VERIFY(consoleText().contains(QStringLiteral("$0012")));
        /* a command that moves the machine moves the window */
        typeInto(in, QStringLiteral("set pc $8027"));
        QCOMPARE(pcOf(m_d), 0x8027);
        typeInto(in, QStringLiteral("break $802D"));
        QVERIFY(adamdebug_breakpoint_check(m_d, 0x802D));
        typeInto(in, QStringLiteral("run"));
        QTRY_VERIFY_WITH_TIMEOUT(out->toPlainText().contains(QStringLiteral("— breakpoint: exec $802D")), 3000);
        waitStopped();
        QCOMPARE(pcOf(m_d), 0x802D);
        QVERIFY(statusLabel()->text().contains(QStringLiteral("breakpoint: exec $802D")));
        /* Tab completes a command, lists several, and Up recalls */
        in->clear();
        QTest::keyClicks(in, QStringLiteral("reg"));
        QTest::keyClick(in, Qt::Key_Tab);
        QCOMPARE(in->text(), QStringLiteral("regs "));
        in->clear();
        QTest::keyClicks(in, QStringLiteral("b"));
        QTest::keyClick(in, Qt::Key_Tab);
        QVERIFY(out->toPlainText().contains(QStringLiteral("break")));
        QVERIFY(out->toPlainText().contains(QStringLiteral("bpw")));
        QVERIFY(in->text().startsWith(QLatin1Char('b')));
        in->clear();
        QTest::keyClick(in, Qt::Key_Up);
        QCOMPARE(in->text(), QStringLiteral("run"));
        QTest::keyClick(in, Qt::Key_Up);
        QCOMPARE(in->text(), QStringLiteral("break $802D"));
        QTest::keyClick(in, Qt::Key_Down);
        QCOMPARE(in->text(), QStringLiteral("run"));
        in->clear();
        shot(QStringLiteral("console"));
    }

    /* Symbols > Next to the cartridge: <cart>.sym beside loop.rom */
    void symbolsNextToTheCartridge()
    {
        m_tabs->setCurrentIndex(0);
        QAction *next = nullptr;
        for (QAction *a : m_win->findChildren<QAction *>())
            if (a->text() == QLatin1String("Next to the cartridge")) next = a;
        QVERIFY(next);
        next->trigger();                    /* no file yet: said, not silent */
        QVERIFY(consoleView()->toPlainText().trimmed().split(QLatin1Char('\n')).size() >= 2);
        QCOMPARE(adamdebug_label_address(m_d, "afterwards"), -1);
        QFile sym(m_dir.path() + QStringLiteral("/loop.sym"));
        QVERIFY(sym.open(QIODevice::WriteOnly));
        sym.write("8030 afterwards\n");
        sym.close();
        next->trigger();
        QCOMPARE(adamdebug_label_address(m_d, "afterwards"), 0x8030);
        QVERIFY(consoleView()->toPlainText().contains(QStringLiteral("1")));
        /* the Symbols tab shows it */
        m_tabs->setCurrentIndex(10);
        QListWidget *list = m_tabs->widget(10)->findChild<QListWidget *>();
        QTRY_VERIFY_WITH_TIMEOUT(!list->findItems(QStringLiteral("8030  afterwards"), Qt::MatchContains).isEmpty(), 2000);
        /* Clear loaded symbols (in the toolbar's menu) drops it again */
        QAction *clear = nullptr;
        for (QAction *a : m_win->findChildren<QAction *>())
            if (a->text() == QLatin1String("Clear loaded symbols")) clear = a;
        QVERIFY(clear);
        clear->trigger();
        QCOMPARE(adamdebug_label_address(m_d, "afterwards"), -1);
        QTRY_VERIFY_WITH_TIMEOUT(list->findItems(QStringLiteral("8030  afterwards"), Qt::MatchContains).isEmpty(), 2000);
    }

    void disassemblyHighlights()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QVERIFY(v);
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x802B) >= 0, 2000);
        const int pcLine = lineOf(v, 0x802B);
        QCOMPARE(v->document()->findBlockByNumber(pcLine).text().left(2), QStringLiteral(" >"));
        /* follow-PC keeps it about a third of the way down */
        const int rows = v->document()->blockCount();
        QVERIFY(pcLine >= rows / 3 - 2 && pcLine <= rows / 3 + 2);
        /* the PC's line: the accent fill (the light variant on a dark theme), applied last */
        auto sels = v->extraSelections();
        QVERIFY(!sels.isEmpty());
        const auto &pc = sels.last();
        QCOMPARE(pc.cursor.blockNumber(), pcLine);
        QVERIFY(pc.format.boolProperty(QTextFormat::FullWidthSelection));
        QCOMPARE(pc.format.background().color(), adamDebugAccentFill(v));
        QCOMPARE(pc.format.foreground().color(), adamDebugAccentFillInk(v));
        if (!adamDebugDarkTheme(v)) {
            QCOMPARE(pc.format.background().color(), adamAccentColor());
            QCOMPARE(pc.format.foreground().color(), QColor(Qt::black));
        }

        /* a breakpoint's dot: filled and red, hollow and grey when disabled */
        adamdebug_breakpoint_toggle(m_d, 0x802D);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, 0x802D)).text().startsWith(
                                     QStringLiteral("●")), 2000);
        auto dotColor = [&](int line) {
            QColor c;
            for (const auto &s : v->extraSelections())
                if (s.cursor.blockNumber() == line && s.cursor.hasSelection() &&
                    s.cursor.selectionStart() == s.cursor.block().position() &&
                    s.cursor.selectionEnd() == s.cursor.block().position() + 1)
                    c = s.format.foreground().color();
            return c;
        };
        QCOMPARE(dotColor(lineOf(v, 0x802D)), QColor(0xe0, 0x1b, 0x24));
        adamdebug_breakpoint b;
        QCOMPARE(adamdebug_breakpoint_list(m_d, &b, 1), 1);
        adamdebug_breakpoint_enable(m_d, b.id, 0);
        QTRY_COMPARE_WITH_TIMEOUT(dotColor(lineOf(v, 0x802D)), QColor(0x9a, 0x99, 0x96), 2000);
        shot(QStringLiteral("disassembly"));
    }

    void clickSelectsAndGutterToggles()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x8030) >= 0, 2000);
        /* nothing selected: Run to Cursor and F9 say so */
        QTest::mouseClick(button(QStringLiteral("Run to Cursor")), Qt::LeftButton);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Select a line")));
        /* a click on the text selects (underlined, bold) and sets no breakpoint */
        clickLine(0x8030, 30);
        QVERIFY(!adamdebug_breakpoint_check(m_d, 0x8030));
        {
            bool underlined = false;
            for (const auto &s : v->extraSelections())
                if (s.cursor.blockNumber() == lineOf(v, 0x8030) && s.format.fontUnderline() &&
                    s.format.fontWeight() == QFont::Bold)
                    underlined = true;
            QVERIFY(underlined);
        }
        /* moving to another line moves the selection; no caret-toggled breakpoints */
        clickLine(0x8032, 30);
        QVERIFY(!adamdebug_breakpoint_check(m_d, 0x8030));
        QVERIFY(!adamdebug_breakpoint_check(m_d, 0x8032));
        /* the left edge toggles, a double-click toggles, F9 toggles the selection */
        clickLine(0x8033, 0);
        QVERIFY(adamdebug_breakpoint_check(m_d, 0x8033));
        clickLine(0x8033, 0);
        QVERIFY(!adamdebug_breakpoint_check(m_d, 0x8033));
        clickLine(0x8034, 30, true);
        QVERIFY(adamdebug_breakpoint_check(m_d, 0x8034));
        adamdebug_breakpoint_toggle(m_d, 0x8034);
        clickLine(0x8036, 30);
        QTest::keyClick(m_win, Qt::Key_F9);
        QVERIFY(adamdebug_breakpoint_check(m_d, 0x8036));
        QTest::keyClick(m_win, Qt::Key_F9);
        QVERIFY(!adamdebug_breakpoint_check(m_d, 0x8036));
        adamdebug_breakpoint b[4];
        QCOMPARE(adamdebug_breakpoint_list(m_d, b, 4), 0);
    }

    void runToCursor()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x8033) >= 0, 2000);
        clickLine(0x8033, 30);
        QTest::mouseClick(button(QStringLiteral("Run to Cursor")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x8033, 3000);
        /* the selection survives the stop, drawn under the PC's fill */
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x8033) >= 0, 2000);
    }

    void wheelJumpAndFollow()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QCheckBox *follow = check(QStringLiteral("Follow PC"));
        QVERIFY(follow && follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x802B) >= 0, 2000);
        const QPoint p = v->viewport()->rect().center();
        QWheelEvent wheel(p, v->viewport()->mapToGlobal(p), QPoint(), QPoint(0, -120), Qt::NoButton, {},
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(v->viewport(), &wheel);
        QVERIFY(!follow->isChecked());
        QVERIFY(lineOf(v, 0x802B) < 0 || lineOf(v, 0x802B) < v->document()->blockCount() / 3 - 1);
        /* the jump box: bare digits are hex here, a label wins first, then expressions */
        QLineEdit *jump = entry(QStringLiteral("$address"));
        QVERIFY(jump);
        typeInto(jump, QStringLiteral("8050"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x8050) >= 0, 2000);
        adamdebug_set_label(m_d, 0x8030, "back");
        typeInto(jump, QStringLiteral("back"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x8030) >= 0, 2000);
        QVERIFY(v->toPlainText().contains(QStringLiteral("back")));
        typeInto(jump, QStringLiteral("$8050+1"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x8051) >= 0, 2000);
        typeInto(jump, QStringLiteral("nosuchlabel"));
        QVERIFY(!m_win->statusBar()->currentMessage().isEmpty());
        /* Follow PC again returns to the PC */
        QTest::mouseClick(follow, Qt::LeftButton, {}, QPoint(4, follow->height() / 2));
        QVERIFY(follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x802B) >= 0, 2000);
        adamdebug_set_label(m_d, 0x8030, nullptr);
    }

    void registerStrip()
    {
        /* under every tab: the Disassembly and the Breakpoints show it as well */
        for (int tab : { 0, 1, 3, 11 }) {
            m_tabs->setCurrentIndex(tab);
            QVERIFY(regFields().first()->isVisible());
        }
        m_tabs->setCurrentIndex(2);
        QList<QLineEdit *> regs = regFields();
        QCOMPARE(regs.size(), 15);                  /* AF..HL', I, R, IM; IFF1/2 are the boxes */
        QTRY_COMPARE_WITH_TIMEOUT(regs[7]->text(), QStringLiteral("802B"), 2000);   /* PC */
        QCOMPARE(regs[14]->text(), QString::number(cpuNow().im));
        typeInto(regs[7], QStringLiteral("8030"));
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x8030, 2000);
        typeInto(regs[8], QStringLiteral("1234"));  /* AF' */
        QCOMPARE(cpuNow().af2, 0x1234);
        /* an expression, the register taken from another */
        typeInto(regs[3], QStringLiteral("abcd"));
        QCOMPARE(cpuNow().hl, 0xABCD);
        typeInto(regs[2], QStringLiteral("hl + 1"));
        QCOMPARE(cpuNow().de, 0xABCE);
        typeInto(regs[4], QStringLiteral("$6100"));
        QCOMPARE(cpuNow().ix, 0x6100);
        /* IM above 2 is refused by the engine and leaves it alone */
        const int imBefore = cpuNow().im;
        typeInto(regs[14], QStringLiteral("7"));
        QCOMPARE(cpuNow().im, imBefore);
        /* the flags */
        QCheckBox *carry = check(QStringLiteral("C"));
        QVERIFY(carry);
        adamdebug_cpu_set(m_d, ADAM_FLAG_C, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!carry->isChecked(), 2000);
        QTest::mouseClick(carry, Qt::LeftButton, {}, QPoint(6, carry->height() / 2));
        QCOMPARE(cpuNow().cf, 1);
        QCheckBox *iff1 = check(QStringLiteral("IFF1"));
        QVERIFY(iff1);
        QTest::mouseClick(iff1, Qt::LeftButton, {}, QPoint(6, iff1->height() / 2));
        QCOMPARE(cpuNow().iff1, 1);
        QTest::mouseClick(iff1, Qt::LeftButton, {}, QPoint(6, iff1->height() / 2));
        QCOMPARE(cpuNow().iff1, 0);
        adamdebug_cpu_set(m_d, ADAM_FLAG_C, 0);
        /* edits need a stopped machine */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!adamdebug_is_stopped(m_d), 2000);
        typeInto(regs[7], QStringLiteral("8024"));
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Stop the machine")));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        shot(QStringLiteral("registers"));
    }

    void memoryPokeAndSearch()
    {
        m_tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QLineEdit *addr = entry(QStringLiteral("address or label"));
        QLineEdit *val = entry(QStringLiteral("value"));
        QVERIFY(addr && val);
        addr->setText(QStringLiteral("6010"));       /* hex */
        typeInto(val, QStringLiteral("5a"));
        uint8_t byte = 0;
        adamdebug_read(m_d, 0x6010, &byte, 1);
        QCOMPARE((int)byte, 0x5A);
        addr->setText(QStringLiteral("$6011"));
        typeInto(val, QStringLiteral("$A5"));
        adamdebug_read(m_d, 0x6011, &byte, 1);
        QCOMPARE((int)byte, 0xA5);
        adamdebug_write(m_d, 0x6012, 0x11);
        addr->setText(QStringLiteral("6012"));
        typeInto(val, QStringLiteral("1FF"));        /* not a byte: refused */
        adamdebug_read(m_d, 0x6012, &byte, 1);
        QCOMPARE((int)byte, 0x11);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("byte")));
        /* Go to marks the row in the accent */
        QLineEdit *go = entry(QStringLiteral("$8000, label"));
        QVERIFY(go);
        typeInto(go, QStringLiteral("8024"));
        auto *mem = m_tabs->widget(2)->findChildren<QPlainTextEdit *>().last();
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$8020: ")));
        QVERIFY(mem->toPlainText().contains(QStringLiteral("31 00 64")));
        bool marked = false;
        for (const auto &s : mem->extraSelections())
            marked |= s.format.background().color() == adamDebugAccentFill(mem) &&
                      s.format.foreground().color() == adamDebugAccentFillInk(mem);
        QVERIFY(marked);
        /* the rows are labelled with the region they are in */
        QVERIFY2(mem->toPlainText().contains(QStringLiteral("; ")), "region names");
        /* search finds the program's LD HL (the 1K RAM repeats every 1K from $6000, so any mirror) */
        QVERIFY(loadProgram());
        typeInto(go, QStringLiteral("6200"));
        QLineEdit *find = entry(QStringLiteral("bytes or text"));
        QVERIFY(find);
        typeInto(find, QStringLiteral("21 34 12 00 22"));
        QLabel *note = nullptr;
        for (QLabel *label : m_tabs->widget(2)->findChildren<QLabel *>())
            if (label->text().startsWith(QLatin1String("Found")) || label->text().startsWith(QLatin1String("Not found"))) note = label;
        QVERIFY(note);
        QVERIFY2(note->text().contains(QRegularExpression(QStringLiteral("Found at \\$[67][0-9A-F]03"))), qPrintable(note->text()));
        /* text search, and a miss */
        auto *kind = m_tabs->widget(2)->findChild<QComboBox *>();
        QVERIFY(kind);
        kind->setCurrentIndex(1);
        typeInto(find, QStringLiteral("zzzzqq"));
        QVERIFY2(note->text().startsWith(QLatin1String("Not found")), qPrintable(note->text()));
        kind->setCurrentIndex(0);
        shot(QStringLiteral("memory"));
    }

    void breakpointsTab()
    {
        m_tabs->setCurrentIndex(3);
        QTableWidget *t = bpTable();
        QVERIFY(t);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QCOMPARE(t->rowCount(), 0);
        QLineEdit *start = entry(QStringLiteral("start"));
        QLineEdit *end = entry(QStringLiteral("end"));
        QLineEdit *cond = entry(QStringLiteral("condition"));
        QVERIFY(start && end && cond);
        QComboBox *type = m_tabs->currentWidget()->findChild<QComboBox *>();
        QVERIFY(type);
        QCOMPARE(type->count(), 6);
        QCOMPARE(type->itemText(0), QStringLiteral("Execute"));
        QCOMPARE(type->itemText(3), QStringLiteral("Access"));
        QPushButton *add = addButton(3);
        QPushButton *clear = nullptr;
        for (QPushButton *btn : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Clear All")) clear = btn;
        QLabel *msg = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->styleSheet().contains(QLatin1String("e01b24"))) msg = l;
        QVERIFY(add && clear && msg);

        /* exec at a hex address */
        typeInto(start, QStringLiteral("802D"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QCOMPARE(t->item(0, 1)->text(), QStringLiteral("exec"));
        QCOMPARE(t->item(0, 2)->text(), QStringLiteral("$802D"));
        QCOMPARE(t->item(0, 0)->checkState(), Qt::Checked);
        QVERIFY(start->text().isEmpty());
        /* a write breakpoint over a range, with a condition */
        type->setCurrentText(QStringLiteral("Write"));
        start->setText(QStringLiteral("6000"));
        end->setText(QStringLiteral("$60FF"));
        cond->setText(QStringLiteral("value > 0"));
        QTest::mouseClick(add, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);
        QCOMPARE(t->item(1, 1)->text(), QStringLiteral("write"));
        QCOMPARE(t->item(1, 2)->text(), QStringLiteral("$6000-$60FF"));
        QCOMPARE(t->item(1, 3)->text(), QStringLiteral("value > 0"));
        /* errors are shown, not swallowed */
        cond->setText(QStringLiteral("nosuchname == 1"));
        start->setText(QStringLiteral("7000"));
        end->clear();
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(!msg->text().isEmpty());
        QCOMPARE(t->rowCount(), 2);
        type->setCurrentText(QStringLiteral("In"));
        cond->clear();
        start->setText(QStringLiteral("300"));
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(!msg->text().isEmpty());
        QCOMPARE(t->rowCount(), 2);
        start->setText(QStringLiteral("FF"));
        end->setText(QStringLiteral("FC"));
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(!msg->text().isEmpty());
        QCOMPARE(t->rowCount(), 2);
        /* In, Out and Access entries: ports show two digits, Access is one row */
        start->setText(QStringLiteral("FC"));
        end->clear();
        QTest::mouseClick(add, Qt::LeftButton);
        type->setCurrentText(QStringLiteral("Out"));
        start->setText(QStringLiteral("FF"));
        QTest::mouseClick(add, Qt::LeftButton);
        type->setCurrentText(QStringLiteral("Access"));
        start->setText(QStringLiteral("6300"));
        QTest::mouseClick(add, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 5, 2000);
        QCOMPARE(t->item(2, 1)->text(), QStringLiteral("in"));
        QCOMPARE(t->item(2, 2)->text(), QStringLiteral("$FC"));
        QCOMPARE(t->item(3, 1)->text(), QStringLiteral("out"));
        QCOMPARE(t->item(4, 1)->text(), QStringLiteral("access"));
        adamdebug_breakpoint list[8];
        QCOMPARE(adamdebug_breakpoint_list(m_d, list, 8), 5);
        QCOMPARE(list[4].type, int(ADAMDEBUG_BP_ACCESS));
        /* an edited condition goes to the engine */
        t->item(1, 3)->setText(QStringLiteral("value > 1"));
        QTRY_VERIFY_WITH_TIMEOUT((adamdebug_breakpoint_list(m_d, list, 8),
                                  QString::fromUtf8(list[1].condition) == QLatin1String("value > 1")), 2000);
        for (int bpid : { list[2].id, list[3].id, list[4].id }) adamdebug_breakpoint_remove(m_d, bpid);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);
        start->clear();
        end->clear();
        msg->clear();
        type->setCurrentText(QStringLiteral("Execute"));

        /* the exec breakpoint stops the run, with its hits counted */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(adamdebug_is_stopped(m_d) && pcOf(m_d) == 0x802D, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("exec $802D")), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 4) && t->item(0, 4)->text().toInt() >= 1, 2000);
        /* Clear Hits zeroes the counts */
        QPushButton *hits = button(QStringLiteral("Clear Hits"));
        QVERIFY(hits);
        QTest::mouseClick(hits, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->item(0, 4)->text(), QStringLiteral("0"), 2000);
        /* unticking it lets the run pass; the write breakpoint then stops it instead */
        t->item(0, 0)->setCheckState(Qt::Unchecked);
        QCOMPARE(adamdebug_breakpoint_list(m_d, list, 8), 2);
        QCOMPARE(list[0].enabled, 0);
        QCOMPARE(list[1].enabled, 1);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(adamdebug_is_stopped(m_d) && statusLabel()->text().contains(QLatin1String("wrote $6000")),
                                 4000);
        QVERIFY(pcOf(m_d) != 0x802D);
        QTRY_VERIFY_WITH_TIMEOUT(consoleText().contains(QStringLiteral("wrote $6000")), 2000);
        shot(QStringLiteral("breakpoints"));
        /* the row's delete button */
        auto *rm = qobject_cast<QToolButton *>(t->cellWidget(1, 5));
        QVERIFY(rm);
        QTest::mouseClick(rm, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QCOMPARE(adamdebug_breakpoint_list(m_d, list, 8), 1);
        QTest::mouseClick(clear, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
        QCOMPARE(adamdebug_breakpoint_list(m_d, list, 8), 0);
    }

    /* a write breakpoint with a condition on the byte, stopping the program in the core's words */
    void breakpointWithCondition()
    {
        QVERIFY(loadProgram());
        m_tabs->setCurrentIndex(3);
        QLineEdit *addr = entry(QStringLiteral("start ("));
        QLineEdit *cond = entry(QStringLiteral("condition"));
        QVERIFY(addr && cond);
        addr->setText(QStringLiteral("$6200"));
        cond->setText(QStringLiteral("value == $34"));
        QComboBox *type = m_tabs->widget(3)->findChild<QComboBox *>();
        type->setCurrentIndex(2);   /* Write */
        addButton(3)->click();
        adamdebug_breakpoint bps[4];
        QCOMPARE(adamdebug_breakpoint_list(m_d, bps, 4), 1);
        QCOMPARE(bps[0].type, int(ADAMDEBUG_BP_WRITE));
        QCOMPARE(QString::fromUtf8(bps[0].condition), QStringLiteral("value == $34"));
        /* a condition that does not parse is refused */
        addr->setText(QStringLiteral("$6201"));
        cond->setText(QStringLiteral("value =="));
        addButton(3)->click();
        QCOMPARE(adamdebug_breakpoint_list(m_d, bps, 4), 1);
        type->setCurrentIndex(0);
        /* run the program: it stops after the store, the byte in the core's words */
        adamdebug_resume(m_d);
        QTRY_VERIFY_WITH_TIMEOUT(adamdebug_is_stopped(m_d), 4000);
        adamdebug_snapshot snap;
        adamdebug_stop_snapshot(m_d, &snap);
        QVERIFY2(QString::fromUtf8(snap.text).contains(QStringLiteral("$34")), snap.text);
        QTRY_VERIFY_WITH_TIMEOUT(button(QStringLiteral("Run (F5)")) != nullptr, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(consoleText().contains(QStringLiteral("$34")), 2000);
        adamdebug_breakpoint_clear(m_d);
    }

    void watchesAndBreakOnChange()
    {
        adamwatch_clear();
        m_tabs->setCurrentIndex(4);
        QLineEdit *entryBox = m_tabs->widget(4)->findChild<QLineEdit *>();
        QVERIFY(entryBox);
        entryBox->setText(QStringLiteral("$6200"));
        QTest::keyClick(entryBox, Qt::Key_Return);
        QCOMPARE(adamwatch_count(), 1);
        QListWidget *list = m_tabs->widget(4)->findChild<QListWidget *>();
        QVERIFY(list);
        QTRY_COMPARE(list->count(), 1);
        QVERIFY(list->item(0)->text().contains(QStringLiteral("6200")));
        list->setCurrentRow(0);
        for (QPushButton *btn : m_tabs->widget(4)->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Break on Change")) btn->click();
        adamdebug_breakpoint bps[4];
        QTRY_COMPARE(adamdebug_breakpoint_list(m_d, bps, 4), 1);
        QCOMPARE(QString::fromUtf8(bps[0].condition), QStringLiteral("value != old"));
        QCOMPARE(bps[0].type, int(ADAMDEBUG_BP_WRITE));
        /* Memory shows the watch, marked */
        for (QPushButton *btn : m_tabs->widget(4)->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Memory")) btn->click();
        QCOMPARE(m_tabs->currentIndex(), 2);
        m_tabs->setCurrentIndex(4);
        /* Remove */
        for (QPushButton *btn : m_tabs->widget(4)->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Remove")) btn->click();
        QCOMPARE(adamwatch_count(), 0);
        QTRY_COMPARE(list->count(), 0);
        adamdebug_breakpoint_clear(m_d);
        adamwatch_clear();
    }

    void traceAndHistory()
    {
        m_tabs->setCurrentIndex(7);
        QPushButton *record = nullptr;
        for (QPushButton *btn : m_tabs->widget(7)->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Record")) record = btn;
        QVERIFY(record);
        record->click();
        QVERIFY(adamdebug_history_enabled(m_d));
        QVERIFY(loadProgram());
        for (int idx = 0; idx < 4; ++idx) {
            adamdebug_step(m_d);
            QTest::qWait(60);
        }
        QTRY_VERIFY_WITH_TIMEOUT(adamdebug_history_count(m_d) >= 4, 3000);
        m_tabs->setCurrentIndex(0);
        m_tabs->setCurrentIndex(7);
        auto *text = m_tabs->widget(7)->findChild<QPlainTextEdit *>();
        QVERIFY(text);
        QTRY_VERIFY_WITH_TIMEOUT(text->toPlainText().contains(QStringLiteral("610")), 3000);
        /* the filter: branches only leaves the NOPs out */
        auto *filter = m_tabs->widget(7)->findChild<QComboBox *>();
        QVERIFY(filter);
        filter->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(!text->toPlainText().contains(QStringLiteral("NOP")), 3000);
        filter->setCurrentIndex(0);
        QTRY_VERIFY_WITH_TIMEOUT(text->toPlainText().contains(QStringLiteral("NOP")), 3000);
        record->click();
        QVERIFY(!adamdebug_history_enabled(m_d));
        adamdebug_history_clear(m_d);
        /* Trace is live: Record on, run, instructions arrive */
        m_tabs->setCurrentIndex(6);
        QPushButton *traceRec = nullptr;
        for (QPushButton *btn : m_tabs->widget(6)->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Record")) traceRec = btn;
        QVERIFY(traceRec);
        traceRec->click();
        QVERIFY(adamdebug_trace_enabled(m_d));
        adamdebug_cpu_set(m_d, ADAM_REG_PC, 0x802B);
        adamdebug_resume(m_d);
        auto *live = m_tabs->widget(6)->findChild<QPlainTextEdit *>();
        QTRY_VERIFY_WITH_TIMEOUT(live->toPlainText().contains(QStringLiteral("DJNZ")), 4000);
        traceRec->click();
        QVERIFY(!adamdebug_trace_enabled(m_d));
        adamdebug_trace_clear(m_d);
        adamdebug_stop(m_d);
        waitStopped();
    }

    void videoSoundAndCart()
    {
        m_tabs->setCurrentIndex(8);
        VideoTab *video = m_win->findChild<VideoTab *>();
        QVERIFY(video);
        video->refresh(true);
        QVERIFY(video->findChild<QComboBox *>() != nullptr);
        QLineEdit *vram = entry(QStringLiteral("addr [bytes"));
        QVERIFY(vram);
        typeInto(vram, QStringLiteral("3F00: 41 42"));
        uint8_t vb[2];
        adamdebug_vdp_read(m_d, 0x3F00, vb, 2);
        QCOMPARE((int)vb[0], 0x41);
        QCOMPARE((int)vb[1], 0x42);
        shot(QStringLiteral("video"));
        m_tabs->setCurrentIndex(9);
        auto soundText = [this] {
            auto *view = qobject_cast<QPlainTextEdit *>(m_tabs->widget(9));
            return view ? view->toPlainText() : QString();
        };
        QTRY_VERIFY2_WITH_TIMEOUT(soundText().contains(QStringLiteral("SN76489")), qPrintable(soundText()), 3000);
        shot(QStringLiteral("sound"));
        m_tabs->setCurrentIndex(5);
        QString ioText;
        QTRY_VERIFY_WITH_TIMEOUT(!m_tabs->widget(5)->findChildren<QPlainTextEdit *>().isEmpty(), 2000);
        for (QPlainTextEdit *view : m_tabs->widget(5)->findChildren<QPlainTextEdit *>()) ioText += view->toPlainText();
        QVERIFY2(ioText.contains(QStringLiteral("Memory map")), qPrintable(ioText));
        QVERIFY(ioText.contains(QStringLiteral("ColecoVision")));
        QVERIFY(ioText.contains(QStringLiteral("$7F=0F")));
        QVERIFY(ioText.contains(QStringLiteral("Controllers")));
        QVERIFY(ioText.contains(QStringLiteral("VDP")));
        shot(QStringLiteral("cart"));
        /* FujiNet: empty without the runtime, and says so */
        m_tabs->setCurrentIndex(11);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        bool note = false;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            note |= l->text().contains(QLatin1String("FujiNet is not running"));
        QVERIFY(note);
        shot(QStringLiteral("fujinet"));
        m_tabs->setCurrentIndex(0);
    }

    /* a PC written while the CPU is in HALT leaves it halted: the strip says so */
    void haltNote()
    {
        adamdebug_write(m_d, kStart + 0x10, 0x76);
        adamcore_z80_regs regs;
        memset(&regs, 0, sizeof regs);
        const adamdebug_cpu cpu = cpuNow();
        regs.sp = 0x6380;
        regs.im = uint8_t(cpu.im);
        char err[200] = "";
        regs.pc = kStart + 0x10;
        QVERIFY(adamdebug_regs_set(m_d, &regs, err, sizeof err));
        const uint64_t before = adamdebug_stop_serial(m_d);
        adamdebug_step(m_d);
        QTRY_VERIFY_WITH_TIMEOUT(adamdebug_stop_serial(m_d) != before && adamdebug_is_stopped(m_d), 3000);
        QVERIFY(cpuNow().halted);
        const QList<QLineEdit *> fields = regFields();
        fields[7]->setText(QStringLiteral("$6100"));
        QTest::keyClick(fields[7], Qt::Key_Return);
        QVERIFY(cpuNow().halted);
        const QString shown = m_win->statusBar()->currentMessage();
        QVERIFY2(shown.contains(QStringLiteral("halt"), Qt::CaseInsensitive), qPrintable(shown));
        QVERIFY(loadProgram());
    }

    /* ADAM_DEBUGGER_POPOUT peels a tab off at the start */
    void popoutEnv()
    {
        discardWindow();
        qputenv("ADAM_DEBUGGER_POPOUT", "sound");
        DebuggerWindow::showFor(nullptr, m_s);
        qunsetenv("ADAM_DEBUGGER_POPOUT");
        m_win = findWindow();
        QVERIFY(m_win);
        m_tabs = m_win->findChild<QTabWidget *>();
        QCOMPARE(m_tabs->count(), 11);
        QWidget *window = nullptr;
        for (QWidget *top : QApplication::topLevelWidgets())
            if (top->windowTitle().endsWith(QLatin1String("Sound"))) window = top;
        QVERIFY(window);
        discardWindow();
        for (QWidget *top : QApplication::topLevelWidgets())
            if (top->windowTitle().endsWith(QLatin1String("Sound"))) top->close();
        openWindow();
    }

    /* the video env hooks reach the tab */
    void videoEnv()
    {
        discardWindow();
        qputenv("ADAM_DEBUGGER_TAB", "video");
        qputenv("ADAM_DEBUGGER_VIDEO_SECTION", "vram");
        qputenv("ADAM_DEBUGGER_VIDEO_VIEW", "beam");
        qputenv("ADAM_DEBUGGER_VIDEO_ZOOM", "2");
        DebuggerWindow::showFor(nullptr, m_s);
        qunsetenv("ADAM_DEBUGGER_TAB");
        qunsetenv("ADAM_DEBUGGER_VIDEO_SECTION");
        qunsetenv("ADAM_DEBUGGER_VIDEO_VIEW");
        qunsetenv("ADAM_DEBUGGER_VIDEO_ZOOM");
        m_win = findWindow();
        QVERIFY(m_win);
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
        m_tabs = m_win->findChild<QTabWidget *>();
        QCOMPARE(m_tabs->currentIndex(), 8);
        VideoTab *video = m_win->findChild<VideoTab *>();
        QVERIFY(video);
        waitStopped();
        QTest::qWait(300);
        shot(QStringLiteral("video-env"));
        discardWindow();
        openWindow();
    }

    /* ADAM_DEBUGGER_WATCH, _WATCH_BREAK, _MEM_GOTO and _HISTORY act in a fresh window */
    void envHooks()
    {
        adamdebug_breakpoint_clear(m_d);
        adamwatch_clear();
        discardWindow();
        qputenv("ADAM_DEBUGGER_TAB", "watches");
        qputenv("ADAM_DEBUGGER_WATCH", "$6300");
        qputenv("ADAM_DEBUGGER_WATCH_BREAK", "$6300");
        qputenv("ADAM_DEBUGGER_MEM_GOTO", "$6300");
        qputenv("ADAM_DEBUGGER_HISTORY", "1");
        DebuggerWindow::showFor(nullptr, m_s);
        qunsetenv("ADAM_DEBUGGER_TAB");
        qunsetenv("ADAM_DEBUGGER_WATCH");
        qunsetenv("ADAM_DEBUGGER_WATCH_BREAK");
        qunsetenv("ADAM_DEBUGGER_MEM_GOTO");
        qunsetenv("ADAM_DEBUGGER_HISTORY");
        m_win = findWindow();
        QVERIFY(m_win);
        m_tabs = m_win->findChild<QTabWidget *>();
        QTRY_COMPARE_WITH_TIMEOUT(adamwatch_count(), 1, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(adamdebug_history_enabled(m_d), 3000);
        adamdebug_breakpoint bps[4];
        QTRY_COMPARE_WITH_TIMEOUT(adamdebug_breakpoint_list(m_d, bps, 4), 1, 3000);
        QCOMPARE(QString::fromUtf8(bps[0].condition), QStringLiteral("value != old"));
        /* MEM_GOTO put its address in the Memory view (the tab stays the one asked for) */
        QCOMPARE(m_tabs->currentIndex(), 4);
        m_tabs->setCurrentIndex(2);
        auto *dump = m_tabs->widget(2)->findChildren<QPlainTextEdit *>().last();
        QTRY_VERIFY_WITH_TIMEOUT(dump->toPlainText().contains(QStringLiteral("$6300:")), 3000);
        adamdebug_history_enable(m_d, 0);
        adamdebug_breakpoint_clear(m_d);
        adamwatch_clear();
    }

    /* DBG_TEST_SHOTS=<dir>: a PNG of every tab */
    void shots()
    {
        if (m_shots.isEmpty()) QSKIP("DBG_TEST_SHOTS not set");
        for (int idx = 0; idx < m_tabs->count(); ++idx) {
            m_tabs->setCurrentIndex(idx);
            QTest::qWait(250);
            QVERIFY(m_win->grab().save(QStringLiteral("%1/tab%2.png").arg(m_shots).arg(idx)));
        }
    }

    /* The main window's F12 (display focused: its ShortcutOverride lets the
     * chord through to the View menu's action) and the View menu entry. */
    void f12FromTheMainWindow()
    {
        /* the display is a QRhiWidget; with no GPU device (CI's offscreen Qt)
         * the window swaps in the software display, so ask for whichever
         * is there */
        QVERIFY(QFile::exists(QStringLiteral(":/shaders/display.vert.qsb")));
        QVERIFY(QFile::exists(QStringLiteral(":/shaders/display.frag.qsb")));
        MainWindow main(m_s);
        main.show();
        QVERIFY(QTest::qWaitForWindowExposed(&main));
        main.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&main));
        QTest::qWait(300);   /* let a queued renderFailed swap the display */
        QWidget *display = main.centralWidget();
        QVERIFY(display);
        display->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(QApplication::focusWidget() == display, 2000);
        QCOMPARE(QApplication::focusWidget(), display);
        QVERIFY(m_win->isVisible());
        QTest::keyClick(display, Qt::Key_F12);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!adamdebug_is_attached(m_d));
        main.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&main));
        QTest::keyClick(display, Qt::Key_F12);
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(adamdebug_is_attached(m_d), 2000);
        waitStopped();
        /* the menu entry does the same */
        QAction *entry = nullptr;
        for (QAction *a : main.findChildren<QAction *>())
            if (a->text() == QLatin1String("Debugger")) entry = a;
        QVERIFY(entry);
        QCOMPARE(entry->shortcut(), QKeySequence(Qt::Key_F12));
        entry->trigger();
        QVERIFY(!m_win->isVisible());
        entry->trigger();
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(adamdebug_is_attached(m_d), 2000);
        waitStopped();
    }

    /* A power cycle or media change keeps the engine attached: the new
     * machine stops at its first instruction and the window follows. */
    void sessionRestart()
    {
        m_tabs->setCurrentIndex(1);
        adamdebug_breakpoint_toggle(m_d, 0x802D);
        adamsession_stop(m_s);
        QCOMPARE(adamsession_start(m_s, &m_opts), 0);
        QTRY_VERIFY_WITH_TIMEOUT(adamdebug_is_stopped(m_d), 3000);
        QCOMPARE(pcOf(m_d), 0x0000);
        QVERIFY(adamdebug_breakpoint_check(m_d, 0x802D));       /* kept */
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().startsWith(QLatin1String("Stopped")), 2000);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(v->toPlainText().contains(QStringLiteral("> 0000")), 2000);
        /* and it runs from there to the breakpoint */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(adamdebug_is_stopped(m_d) && pcOf(m_d) == 0x802D, 5000);
    }

    void f12HidesAndDetaches()
    {
        QVERIFY(m_win->isVisible());
        QTest::keyClick(m_win, Qt::Key_F12);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!adamdebug_is_attached(m_d));
        const adamdebug_cpu before = cpuNow();
        QTRY_VERIFY_WITH_TIMEOUT(cpuNow().frame > before.frame + 3, 3000);
        /* the main window's F12 is toggleFor: it shows the window and attaches (stopping) */
        DebuggerWindow::toggleFor(nullptr, m_s);
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(adamdebug_is_attached(m_d), 2000);
        waitStopped();
        DebuggerWindow::toggleFor(nullptr, m_s);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!adamdebug_is_attached(m_d));
        DebuggerWindow::toggleFor(nullptr, m_s);
        QTRY_VERIFY_WITH_TIMEOUT(adamdebug_is_attached(m_d), 2000);
        waitStopped();
    }
};

QTEST_MAIN(DebuggerTest)
#include "debugger_test.moc"

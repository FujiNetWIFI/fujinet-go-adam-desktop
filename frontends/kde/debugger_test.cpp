/*
 * debugger_test -- the Debugger window (debugger/DebuggerWindow.cpp) driven
 * with synthetic events on the offscreen platform against a running
 * ColecoVision session whose cartridge is a hand-assembled loop (no ROMs
 * beyond the embedded ones): the tabs, attach/detach with show and hide, F12,
 * the stepping keys and buttons, the console (transcript, Tab completion,
 * history), the disassembly (the PC's line, click / double-click / F9 for a
 * breakpoint, selection and Run to Cursor, the wheel, the jump box), the
 * register and memory entries, the Breakpoints tab, and F12 and the View
 * menu from the main window.
 *
 * DBG_TEST_SHOTS=<dir> also grabs a PNG of each tab (DBG_TEST_LIGHT=1: in the
 * light scheme).
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
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QStyleHints>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QToolButton>

#include "FujiNetWindows.h"
#include "adamdebug.h"
#include "adamsession.h"
#include "DisplayWidget.h"
#include "MainWindow.h"
#include "debugger/DebuggerWindow.h"

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
    QLineEdit *entryIs(const QString &placeholder)
    {
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->placeholderText() == placeholder) return e;
        return nullptr;
    }
    QPlainTextEdit *disasmView()
    {
        for (QPlainTextEdit *v : m_win->findChildren<QPlainTextEdit *>())
            if (v->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff) return v;
        return nullptr;
    }
    QPlainTextEdit *consoleView()
    {
        for (QPlainTextEdit *v : m_win->findChildren<QPlainTextEdit *>())
            if (v->lineWrapMode() == QPlainTextEdit::WidgetWidth) return v;
        return nullptr;
    }
    QLabel *statusLabel()
    {
        for (QLabel *l : m_win->findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("Stopped")) ||
                l->text() == QLatin1String("Running"))
                return l;
        return nullptr;
    }
    QCheckBox *check(const QString &text)
    {
        for (QCheckBox *c : m_win->findChildren<QCheckBox *>())
            if (c->text() == text) return c;
        return nullptr;
    }
    QTableWidget *bpTable() { return m_win->findChild<QTableWidget *>(); }

    void waitStopped()
    {
        QTRY_VERIFY_WITH_TIMEOUT(adamdebug_is_stopped(m_d), 3000);
        /* the window follows within its poll */
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel() &&
                                     statusLabel()->text().startsWith(QLatin1String("Stopped")), 2000);
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
            if (b.text().mid(2, 4) == want) return n;
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

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qputenv("SDL_AUDIODRIVER", "dummy");
        if (qEnvironmentVariableIsSet("DBG_TEST_SHOTS")) m_shots = qEnvironmentVariable("DBG_TEST_SHOTS");
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        if (qEnvironmentVariableIsSet("DBG_TEST_LIGHT"))
            QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
#endif
        /* main.cpp's app-wide rule for toggled push buttons: the Run button
         * is one while the machine is stopped */
        const QColor accent = adamAccentColor();
        qApp->setStyleSheet(
            QStringLiteral("QPushButton:checked { background-color: %1; color: black;"
                           " border: 1px solid %3; border-radius: 4px; padding: 4px 12px; }"
                           "QPushButton:checked:hover { background-color: %2; }")
                .arg(accent.name(), accent.lighter(112).name(), accent.darker(140).name()));
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

        /* the window opens on the tab ADAM_DEBUGGER_TAB names */
        qputenv("ADAM_DEBUGGER_TAB", "memmap");
        DebuggerWindow::showFor(nullptr, m_s);
        qunsetenv("ADAM_DEBUGGER_TAB");
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->objectName() == QLatin1String("debugger")) m_win = qobject_cast<QMainWindow *>(w);
        QVERIFY(m_win);
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
        m_tabs = m_win->findChild<QTabWidget *>();
        QVERIFY(m_tabs);
        QCOMPARE(m_tabs->currentIndex(), 5);
        m_tabs->setCurrentIndex(0);
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
    }

    void tabsAndAttach()
    {
        QStringList names;
        for (int i = 0; i < m_tabs->count(); ++i) names << m_tabs->tabText(i);
        QCOMPARE(names, (QStringList{ QStringLiteral("Console"), QStringLiteral("CPU && Memory"),
                                      QStringLiteral("Disassembly"), QStringLiteral("VDP"),
                                      QStringLiteral("Breakpoints"), QStringLiteral("Memory Map && I/O"),
                                      QStringLiteral("FujiNet") }));
        QVERIFY(adamdebug_is_attached(m_d));
        QVERIFY(adamdebug_is_stopped(m_d));
        /* Run is the accent while stopped, whichever text it wears, in one size */
        QPushButton *run = button(QStringLiteral("Run (F5)"));
        QVERIFY(run);
        QVERIFY(run->isChecked());
        const QSize size = run->size();
        QTest::mouseClick(run, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!adamdebug_is_stopped(m_d), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(run->text() == QLatin1String("Stop (F5)"), 2000);
        QVERIFY(!run->isChecked());
        QCOMPARE(run->size(), size);
        QCOMPARE(statusLabel()->text(), QStringLiteral("Running"));
        QTest::mouseClick(run, Qt::LeftButton);
        waitStopped();
        QVERIFY(run->isChecked());
        QCOMPARE(run->size(), size);
    }

    void stepKeys()
    {
        QCOMPARE(pcOf(m_d), 0x802B);
        QTest::keyClick(m_win, Qt::Key_F7);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x802D, 2000);
        QCOMPARE(statusLabel()->text().left(13), QStringLiteral("Stopped: step"));
        /* F8 steps over the CALL */
        QTest::keyClick(m_win, Qt::Key_F8);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x8030, 2000);
        QVERIFY(statusLabel()->text().contains(QLatin1String("step over")));
        /* the buttons do the same */
        adamdebug_write(m_d, 0x63FE, 0x30);          /* a return address, as a CALL left it */
        adamdebug_write(m_d, 0x63FF, 0x80);
        adamdebug_cpu_set(m_d, ADAM_REG_SP, 0x63FE);
        adamdebug_cpu_set(m_d, ADAM_REG_PC, 0x8050);
        QTest::mouseClick(button(QStringLiteral("Step Out")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x8030, 2000);
        QVERIFY(statusLabel()->text().contains(QLatin1String("step out")));
        /* Frame: Shift+F7 lands on a frame boundary */
        adamdebug_cpu c, c2;
        adamdebug_cpu_get(m_d, &c);
        QTest::keyClick(m_win, Qt::Key_F7, Qt::ShiftModifier);
        QTRY_VERIFY_WITH_TIMEOUT(adamdebug_is_stopped(m_d) &&
                                     (adamdebug_cpu_get(m_d, &c2), c2.frame > c.frame), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("frame")), 2000);
        /* stepping while running is refused with a word, not silently */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!adamdebug_is_stopped(m_d), 2000);
        QTest::keyClick(m_win, Qt::Key_F7);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Stop the machine")));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
    }

    void consoleCommands()
    {
        m_tabs->setCurrentIndex(0);
        QLineEdit *in = entry(QStringLiteral("command"));
        QPlainTextEdit *out = consoleView();
        QVERIFY(in && out);
        QCOMPARE(m_win->focusWidget(), in);         /* typing goes straight in */
        typeInto(in, QStringLiteral("regs"));
        QVERIFY(out->toPlainText().contains(QStringLiteral("> regs")));
        QVERIFY(out->toPlainText().contains(QStringLiteral("flags")));
        QVERIFY(in->text().isEmpty());
        /* a command that moves the machine moves the window */
        typeInto(in, QStringLiteral("set pc $8027"));
        QCOMPARE(pcOf(m_d), 0x8027);
        typeInto(in, QStringLiteral("break 32813"));    /* decimal, the engine's rule: $802D */
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
        QCOMPARE(in->text(), QStringLiteral("break 32813"));
        QTest::keyClick(in, Qt::Key_Down);
        QCOMPARE(in->text(), QStringLiteral("run"));
        in->clear();
        shot(QStringLiteral("console"));
    }

    /* Load Symbols > Next to the Cartridge: <cart>.sym beside loop.rom */
    void symbolsNextToTheCartridge()
    {
        m_tabs->setCurrentIndex(0);
        QAction *next = nullptr;
        for (QAction *a : m_win->findChildren<QAction *>())
            if (a->text() == QLatin1String("Next to the Cartridge")) next = a;
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
        adamdebug_set_label(m_d, 0x8030, nullptr);
    }

    void disassemblyHighlights()
    {
        m_tabs->setCurrentIndex(2);
        QPlainTextEdit *v = disasmView();
        QVERIFY(v);
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x802B) >= 0, 2000);
        const int pcLine = lineOf(v, 0x802B);
        QCOMPARE(v->document()->findBlockByNumber(pcLine).text().left(2), QStringLiteral(" >"));
        /* follow-PC keeps it about a third of the way down */
        const int rows = v->document()->blockCount();
        QVERIFY(pcLine >= rows / 3 - 2 && pcLine <= rows / 3 + 2);
        /* the PC's line: the accent fill with black ink, applied last */
        auto sels = v->extraSelections();
        QVERIFY(!sels.isEmpty());
        const auto &pc = sels.last();
        QCOMPARE(pc.cursor.blockNumber(), pcLine);
        QVERIFY(pc.format.boolProperty(QTextFormat::FullWidthSelection));
        QCOMPARE(pc.format.background().color(), adamAccentColor());
        QCOMPARE(pc.format.foreground().color(), QColor(Qt::black));

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
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, 0x802D)).text().startsWith(
                                     QStringLiteral("○")), 2000);
        QCOMPARE(dotColor(lineOf(v, 0x802D)), QColor(0x9a, 0x99, 0x96));
        shot(QStringLiteral("disassembly"));
    }

    void clickSelectsAndGutterToggles()
    {
        m_tabs->setCurrentIndex(2);
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
        m_tabs->setCurrentIndex(2);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x8033) >= 0, 2000);
        clickLine(0x8033, 30);
        QTest::mouseClick(button(QStringLiteral("Run to Cursor")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x8033, 3000);
        QVERIFY(statusLabel()->text().contains(QLatin1String("run to")));
        /* the selection survives the stop, drawn under the PC's fill */
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0x8033) >= 0, 2000);
    }

    void wheelJumpAndFollow()
    {
        m_tabs->setCurrentIndex(2);
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

    void registersFlagsMemory()
    {
        m_tabs->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QList<QLineEdit *> regs;
        for (QLineEdit *e : m_tabs->currentWidget()->findChildren<QLineEdit *>())
            if (e->toolTip().startsWith(QLatin1String("Enter sets it"))) regs << e;
        QCOMPARE(regs.size(), 15);                  /* AF..HL', I, R, IM; IFF1/2 are the boxes */
        QTRY_COMPARE_WITH_TIMEOUT(regs[7]->text(), QStringLiteral("802B"), 2000);   /* PC */
        QCOMPARE(regs[14]->text(), QString::number(0));
        typeInto(regs[7], QStringLiteral("8030"));
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0x8030, 2000);
        typeInto(regs[8], QStringLiteral("1234"));  /* AF' */
        adamdebug_cpu c;
        adamdebug_cpu_get(m_d, &c);
        QCOMPARE(c.af2, 0x1234);
        /* the flags */
        QCheckBox *carry = check(QStringLiteral("C"));
        QVERIFY(carry);
        adamdebug_cpu_set(m_d, ADAM_FLAG_C, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!carry->isChecked(), 2000);
        QTest::mouseClick(carry, Qt::LeftButton, {}, QPoint(6, carry->height() / 2));
        adamdebug_cpu_get(m_d, &c);
        QCOMPARE(c.cf, 1);
        QCheckBox *iff1 = check(QStringLiteral("IFF1"));
        QTest::mouseClick(iff1, Qt::LeftButton, {}, QPoint(6, iff1->height() / 2));
        adamdebug_cpu_get(m_d, &c);
        QCOMPARE(c.iff1, 1);
        adamdebug_cpu_set(m_d, ADAM_REG_IFF1, 0);
        adamdebug_cpu_set(m_d, ADAM_FLAG_C, 0);
        /* the beam / cycles line */
        bool beam = false;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("scanline"))) beam = l->text().contains(QLatin1String("T-states"));
        QVERIFY(beam);
        /* memory: write through the map, go to a label or an address */
        QLineEdit *addr = entryIs(QStringLiteral("$C000"));
        QLineEdit *val = entryIs(QStringLiteral("$00"));
        QVERIFY(addr && val);
        addr->setText(QStringLiteral("6010"));       /* hex */
        typeInto(val, QStringLiteral("5a"));
        uint8_t b = 0;
        adamdebug_read(m_d, 0x6010, &b, 1);
        QCOMPARE((int)b, 0x5A);
        addr->setText(QStringLiteral("$6011"));
        typeInto(val, QStringLiteral("$A5"));
        adamdebug_read(m_d, 0x6011, &b, 1);
        QCOMPARE((int)b, 0xA5);
        adamdebug_write(m_d, 0x6012, 0x11);
        addr->setText(QStringLiteral("6012"));
        typeInto(val, QStringLiteral("1FF"));        /* not a byte */
        adamdebug_read(m_d, 0x6012, &b, 1);
        QCOMPARE((int)b, 0x11);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("byte")));
        QLineEdit *go = entryIs(QStringLiteral("$C000 or label"));
        QVERIFY(go);
        typeInto(go, QStringLiteral("8024"));
        QPlainTextEdit *mem = nullptr;
        for (QPlainTextEdit *e : m_tabs->currentWidget()->findChildren<QPlainTextEdit *>()) mem = e;
        QVERIFY(mem);
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$8020: ")));
        QVERIFY(mem->toPlainText().contains(QStringLiteral("31 00 64")));
        /* the row Go to found is marked in the accent colour, black ink */
        bool marked = false;
        for (const auto &s : mem->extraSelections())
            marked |= s.format.background().color() == adamAccentColor() &&
                      s.format.foreground().color() == QColor(Qt::black);
        QVERIFY(marked);
        /* edits need a stopped machine */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!adamdebug_is_stopped(m_d), 2000);
        typeInto(regs[7], QStringLiteral("8024"));
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Stop the machine")));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        shot(QStringLiteral("cpu"));
    }

    void breakpointsTab()
    {
        m_tabs->setCurrentIndex(4);
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
        QPushButton *add = button(QStringLiteral("Add"));
        QPushButton *clear = button(QStringLiteral("Clear All"));
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
        /* a write watchpoint over a range, with a condition */
        type->setCurrentText(QStringLiteral("write"));
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
        type->setCurrentText(QStringLiteral("in"));
        cond->clear();
        start->setText(QStringLiteral("300"));
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(!msg->text().isEmpty());
        QCOMPARE(t->rowCount(), 2);
        start->setText(QStringLiteral("FF"));
        end->setText(QStringLiteral("FC"));
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(msg->text().contains(QLatin1String("before")));
        start->clear();
        end->clear();
        msg->clear();
        type->setCurrentText(QStringLiteral("exec"));

        /* the exec breakpoint stops the run, with its hits counted */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(adamdebug_is_stopped(m_d) && pcOf(m_d) == 0x802D, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("exec $802D")), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 4) && t->item(0, 4)->text().toInt() >= 1, 2000);
        /* unticking it lets the run pass; the write watch then stops it instead */
        t->item(0, 0)->setCheckState(Qt::Unchecked);
        adamdebug_breakpoint list[2];
        QCOMPARE(adamdebug_breakpoint_list(m_d, list, 2), 2);
        QCOMPARE(list[0].enabled, 0);
        QCOMPARE(list[1].enabled, 1);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(adamdebug_is_stopped(m_d) && statusLabel()->text().contains(QLatin1String("write $6000")),
                                 4000);
        QVERIFY(pcOf(m_d) != 0x802D);
        shot(QStringLiteral("breakpoints"));
        /* the row's delete button */
        auto *rm = qobject_cast<QToolButton *>(t->cellWidget(1, 5));
        QVERIFY(rm);
        QTest::mouseClick(rm, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QCOMPARE(adamdebug_breakpoint_list(m_d, list, 2), 1);
        QTest::mouseClick(clear, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
        QCOMPARE(adamdebug_breakpoint_list(m_d, list, 2), 0);
    }

    void otherTabs()
    {
        /* Memory Map & I/O: the engine's text */
        m_tabs->setCurrentIndex(5);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        auto *io = qobject_cast<QPlainTextEdit *>(m_tabs->currentWidget());
        QVERIFY(io);
        QTRY_VERIFY_WITH_TIMEOUT(io->toPlainText().contains(QStringLiteral("$7F=0F")), 2000);
        QVERIFY(io->toPlainText().contains(QStringLiteral("ColecoVision")));
        shot(QStringLiteral("memmap"));
        /* VDP: the pictures and the VRAM editor */
        m_tabs->setCurrentIndex(3);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        bool pix = false;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            pix |= !l->pixmap(Qt::ReturnByValue).isNull() && l->pixmap(Qt::ReturnByValue).width() == 256;
        QVERIFY(pix);
        QLineEdit *vram = entry(QStringLiteral("addr [bytes"));
        QVERIFY(vram);
        typeInto(vram, QStringLiteral("3F00: 41 42"));
        uint8_t b[2];
        adamdebug_vdp_read(m_d, 0x3F00, b, 2);
        QCOMPARE((int)b[0], 0x41);
        QCOMPARE((int)b[1], 0x42);
        shot(QStringLiteral("vdp"));
        /* FujiNet: empty without the runtime, and says so */
        m_tabs->setCurrentIndex(6);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        bool note = false;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            note |= l->text().contains(QLatin1String("FujiNet is not running"));
        QVERIFY(note);
        shot(QStringLiteral("fujinet"));
        m_tabs->setCurrentIndex(0);
    }

    /* The main window's F12 (display focused: its ShortcutOverride lets the
     * chord through to the View menu's action) and the View menu entry. */
    void f12FromTheMainWindow()
    {
        /* the display is a QOpenGLWidget: CI's offscreen Qt has no GL */
        QOpenGLContext gl;
        if (!gl.create())
            QSKIP("no OpenGL context on this platform");
        MainWindow main(m_s);
        main.show();
        QVERIFY(QTest::qWaitForWindowExposed(&main));
        main.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&main));
        auto *display = main.findChild<DisplayWidget *>();
        QVERIFY(display);
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
        m_tabs->setCurrentIndex(2);
        adamdebug_breakpoint_toggle(m_d, 0x802D);
        adamsession_stop(m_s);
        QCOMPARE(adamsession_start(m_s, &m_opts), 0);
        QTRY_VERIFY_WITH_TIMEOUT(adamdebug_is_stopped(m_d), 3000);
        QCOMPARE(pcOf(m_d), 0x0000);
        QVERIFY(adamdebug_breakpoint_check(m_d, 0x802D));       /* kept */
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().startsWith(QLatin1String("Stopped")), 2000);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(v->toPlainText().contains(QStringLiteral(">0000")), 2000);
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
        adamdebug_cpu c, c2;
        adamdebug_cpu_get(m_d, &c);
        QTRY_VERIFY_WITH_TIMEOUT((adamdebug_cpu_get(m_d, &c2), c2.frame > c.frame + 3), 3000);
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

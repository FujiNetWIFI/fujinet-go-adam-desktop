/*
 * Debugger window (Qt6 Widgets) over the Z80 / VDP debugger engine, via
 * core/include/adamdebug.h. Mirrors the GNOME one tab for tab, and the
 * SMS / MSX KDE debuggers' design: a QTabWidget under a toolbar.
 *
 * The engine is attached while the window is shown (which stops the
 * machine, as on every sibling) and detached when it is hidden, which lets
 * the machine run on. The window is never deleted by its close button: it
 * is hidden, and comes back as it was.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <cstdint>
#include <functional>
#include <vector>

#include "adamdebug.h"
#include "adamsession.h"

class DebuggerWindow : public QMainWindow {
    Q_OBJECT
public:
    /* Shows the window (attaching, so the machine stops). */
    static void showFor(QWidget *parent, adamsession *session);
    /* F12: shows it, or hides it (detaching) when it is already up. */
    static void toggleFor(QWidget *parent, adamsession *session);

protected:
    bool event(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    explicit DebuggerWindow(adamsession *session, QWidget *parent);
    ~DebuggerWindow() override;
    QWidget *buildToolbar();
    QWidget *buildConsole();
    QWidget *buildCpu();
    QWidget *buildDisasm();
    QWidget *buildVdp();
    QWidget *buildBreaks();
    QWidget *buildIo();
    QWidget *buildFujinet();

    void refreshStatus();
    void refreshCpu();
    void refreshMem();
    void refreshDisasm();
    void refreshVdp();
    void refreshBps();
    void refreshIo();
    void refreshFujinet(bool force);
    void refreshPage(int page, bool live);
    void refreshAll();
    void resync();
    void syncLater();
    void settleAndSync();
    void tick();

    void consoleAppend(const QString &text);
    void runConsole();
    void completeConsole();
    void say(const QString &msg);
    void message(const QString &msg);

    void toggleRun();
    void stepAnd(const std::function<void()> &fn);
    void runToCursor();
    void toggleBreakpoint(int addr);
    void toggleSelected();
    void addBreakpoint();
    void setReg(int i);
    void setFlag(int id, bool on);
    void memGoto();
    void memWrite();
    void jumpTo();
    void vramSubmit();
    void vramPage(int delta);
    void loadSymbols(bool fromFile);
    void save(const char *kind, const QString &title, const QString &file);
    int visibleRows() const;
    bool numberOf(const QString &text, long *out, QString *why = nullptr);
    bool addressOf(const QString &text, long *out, QString *why = nullptr);
    void setEntry(QLineEdit *e, const QString &text);

    adamsession *m_session;
    adamdebug *m_dbg;
    unsigned m_seenGen = 0;
    unsigned m_seenStops = 0;
    bool m_wasStopped = false;
    int m_runningTicks = 0;
    QTimer m_timer;
    QTabWidget *m_tabs = nullptr;

    QLabel *m_status = nullptr;
    QPushButton *m_runBtn = nullptr;

    /* console */
    QPlainTextEdit *m_conOut = nullptr;
    QLineEdit *m_conIn = nullptr;
    QStringList m_history;
    int m_historyPos = 0;

    /* cpu + memory */
    QLineEdit *m_reg[15] = {};
    QCheckBox *m_flag[6] = {};
    QCheckBox *m_iff[2] = {};
    QLabel *m_beam = nullptr;
    QPlainTextEdit *m_mem = nullptr;
    QLineEdit *m_memGoto = nullptr, *m_memAddr = nullptr, *m_memVal = nullptr;
    int m_memTop = 0;
    int m_memMark = -1;                 /* the address Go to found; -1 none */

    /* disassembly */
    QPlainTextEdit *m_disasm = nullptr;
    QCheckBox *m_followPc = nullptr;
    QLineEdit *m_jump = nullptr;
    uint16_t m_disasmTop = 0;
    int m_selected = -1;                /* the selected line's address, or -1 */
    int m_wheel = 0;                    /* wheel travel not yet a row */
    std::vector<uint16_t> m_lineAddr;

    /* vdp */
    QLabel *m_nt = nullptr, *m_pat = nullptr, *m_spr = nullptr, *m_pal = nullptr;
    QComboBox *m_patBank = nullptr;
    QPlainTextEdit *m_spriteInfo = nullptr, *m_vdpState = nullptr, *m_vramView = nullptr;
    QLineEdit *m_vramEntry = nullptr;
    QLabel *m_vramStatus = nullptr;
    uint16_t m_vramBase = 0;

    /* breakpoints */
    QTableWidget *m_bpTable = nullptr;
    QString m_bpSig;                    /* what the table shows, to rebuild on change */
    bool m_bpFilling = false;
    QLabel *m_bpEmpty = nullptr, *m_bpMsg = nullptr;
    QComboBox *m_bpType = nullptr;
    QLineEdit *m_bpStart = nullptr, *m_bpEnd = nullptr, *m_bpCond = nullptr;

    /* memory map and I/O */
    QPlainTextEdit *m_io = nullptr;

    /* fujinet */
    QLabel *m_fujiNote = nullptr;
    QPlainTextEdit *m_fujiText = nullptr, *m_fujiState = nullptr;
    uint64_t m_fujiLast = 0;
    int m_fujiCount = -1;
};

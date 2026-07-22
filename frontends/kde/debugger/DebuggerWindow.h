/*
 * Debugger window for the KDE/Qt frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QMainWindow>

#include "adamdebug.h"
#include "adamsession.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

class DebuggerWindow : public QMainWindow {
    Q_OBJECT
public:
    /* Shows (creating on first use) the debugger for the session. */
    static void showFor(QWidget *parent, adamsession *session);

signals:
    void stopped(int reason, quint16 pc);

private:
    DebuggerWindow(QWidget *parent, adamsession *session);
    ~DebuggerWindow() override;

    void buildUi();
    void refreshAll();
    void refreshDisasm();
    void refreshRegs();
    void refreshMem();
    void refreshBps();
    void refreshTrace();
    void refreshVdp();
    void onStopped(int reason, quint16 pc);
    void pauseContinue();
    bool parseAddr(const QString &text, quint16 *out);

    adamsession *m_session;
    adamdebug *m_dbg;

    QLabel *m_status;
    QPushButton *m_pauseBtn;
    QPlainTextEdit *m_disasm;
    quint16 m_disasmBase = 0;
    bool m_followPc = true;

    QLineEdit *m_regEdit[8];
    QLabel *m_flags;

    QLineEdit *m_memAddr;
    QPlainTextEdit *m_memView;
    quint16 m_memBase = 0xFC30;

    QLineEdit *m_bpEntry;
    QPlainTextEdit *m_bpView;
    QPlainTextEdit *m_traceView;

    QLabel *m_nt, *m_pat, *m_spr, *m_pal;
    QComboBox *m_patBank;
    QPlainTextEdit *m_spriteInfo;

    QTimer *m_tick;
};

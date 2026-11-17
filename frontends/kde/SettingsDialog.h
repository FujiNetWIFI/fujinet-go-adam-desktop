/*
 * SettingsDialog: preferences for the KDE/Qt frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDialog>
#include <QVector>

#include "adamsession.h"

class QComboBox;
class QCheckBox;
class QLineEdit;
class QFormLayout;
class QTimer;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(adamsession *session, QWidget *parent = nullptr);

    /* True when an option needing a session restart was changed. */
    bool machineOptionsChanged() const { return m_machineDirty; }

    /* True when the Input page's "Open Controllers" closed the dialog (as
     * OK): a modal dialog would block the window it opens. */
    bool openControllersRequested() const { return m_openControllers; }

public slots:
    void accept() override;

private:
    QComboBox *addCombo(class QFormLayout *form, const QString &label,
                        const char *key, int def, const QStringList &items);
    QCheckBox *addCheck(class QFormLayout *form, const QString &label,
                        const char *key, int def);
    bool saveValue(const char *key, int def, int value);
    QWidget *buildInputPage();
    void rebuildPads();

    adamsession *m_session;
    bool m_machineDirty = false;
    bool m_openControllers = false;

    QFormLayout *m_padForm = nullptr;
    QVector<QWidget *> m_padRows;
    unsigned m_padGen = 0;

    QComboBox *m_machine, *m_palette, *m_expansion, *m_joystick, *m_aspect;
    QCheckBox *m_swapButtons, *m_reverseKeypad, *m_smooth;
    QCheckBox *m_external;
    QLineEdit *m_externalAddr;
};

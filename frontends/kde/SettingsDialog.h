/*
 * SettingsDialog: preferences for the KDE/Qt frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDialog>

#include "adamsession.h"

class QComboBox;
class QCheckBox;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(adamsession *session, QWidget *parent = nullptr);

    /* True when an option needing a session restart was changed. */
    bool machineOptionsChanged() const { return m_machineDirty; }

public slots:
    void accept() override;

private:
    QComboBox *addCombo(class QFormLayout *form, const QString &label,
                        const char *key, int def, const QStringList &items);
    QCheckBox *addCheck(class QFormLayout *form, const QString &label,
                        const char *key, int def);
    bool saveValue(const char *key, int def, int value);

    adamsession *m_session;
    bool m_machineDirty = false;

    QComboBox *m_machine, *m_palette, *m_expansion, *m_joystick, *m_aspect;
    QCheckBox *m_swapButtons, *m_reverseKeypad, *m_smooth;
};

/*
 * SettingsDialog: machine options (session restart on OK) and display
 * options (applied live by the caller). Values go through the shared
 * settings store, so the GNOME frontend sees the same configuration.
 * Option lists mirror the Android app's Settings.kt.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QVBoxLayout>

SettingsDialog::SettingsDialog(adamsession *session, QWidget *parent)
    : QDialog(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("Preferences"));

    auto *machineBox = new QGroupBox(QStringLiteral("Machine"), this);
    auto *machineForm = new QFormLayout(machineBox);
    m_machine = addCombo(machineForm, QStringLiteral("Machine"), "machine", 0,
                         {QStringLiteral("ADAM (computer)"),
                          QStringLiteral("ColecoVision (game)")});
    m_palette = addCombo(machineForm, QStringLiteral("Palette"), "palette", 0,
                         {QStringLiteral("Default (TMS9928)"),
                          QStringLiteral("Palette 2"),
                          QStringLiteral("Palette 3"),
                          QStringLiteral("Palette 4")});
    m_expansion = addCombo(
        machineForm, QStringLiteral("Expansion module"), "expansion", 0,
        {QStringLiteral("None"),
         QStringLiteral("Roller controller (mouse)"),
         QStringLiteral("Roller controller (joystick)"),
         QStringLiteral("Driving module (joystick)"),
         QStringLiteral("Driving module (mouse)"),
         QStringLiteral("Super Action speed roller, both ports (mouse)"),
         QStringLiteral("Speed roller, port 1 (mouse)"),
         QStringLiteral("Speed roller, port 2 (mouse)")});
    m_joystick = addCombo(machineForm, QStringLiteral("Joystick mode"),
                          "joystick_mode", 1,
                          {QStringLiteral("No joystick"),
                           QStringLiteral("Both ports"),
                           QStringLiteral("Port 2 only"),
                           QStringLiteral("Port 1 only")});
    m_swapButtons = addCheck(machineForm,
                             QStringLiteral("Swap joystick buttons"),
                             "swap_buttons", 0);
    m_reverseKeypad = addCheck(machineForm, QStringLiteral("Reverse keypad"),
                               "reverse_keypad", 0);

    auto *fujinetBox = new QGroupBox(QStringLiteral("FujiNet"), this);
    auto *fujinetForm = new QFormLayout(fujinetBox);
    m_external = addCheck(fujinetForm, QStringLiteral("Use external FujiNet"),
                          "fujinet_external", 0);
    m_externalAddr = new QLineEdit(
        QString::fromUtf8(adamsession_get_str(session, "fujinet_external_addr",
                                              "")),
        this);
    m_externalAddr->setPlaceholderText(QStringLiteral("0.0.0.0:65216"));
    m_externalAddr->setEnabled(m_external->isChecked());
    connect(m_external, &QCheckBox::toggled, m_externalAddr,
            &QWidget::setEnabled);
    fujinetForm->addRow(QStringLiteral("Listen address:port"),
                        m_externalAddr);
    auto *hint = new QLabel(
        QStringLiteral("Skips the built-in FujiNet. Point the other "
                       "instance's [BOIP] host and port here. 0.0.0.0 accepts "
                       "connections from the network; 127.0.0.1 only from "
                       "this computer."),
        this);
    hint->setWordWrap(true);
    fujinetForm->addRow(hint);

    auto *displayBox = new QGroupBox(QStringLiteral("Display"), this);
    auto *displayForm = new QFormLayout(displayBox);
    m_aspect = addCombo(displayForm, QStringLiteral("Aspect ratio"),
                        "aspect_mode", 0,
                        {QStringLiteral("Square pixels (256:212)"),
                         QStringLiteral("TV (4:3)"),
                         QStringLiteral("Integer scale")});
    m_smooth = addCheck(displayForm, QStringLiteral("Smooth scaling"),
                        "smooth_scaling", 0);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this,
            &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this,
            &SettingsDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(machineBox);
    layout->addWidget(fujinetBox);
    layout->addWidget(displayBox);
    layout->addWidget(buttons);
}

QComboBox *SettingsDialog::addCombo(QFormLayout *form, const QString &label,
                                    const char *key, int def,
                                    const QStringList &items)
{
    auto *combo = new QComboBox(this);
    combo->addItems(items);
    combo->setCurrentIndex(adamsession_get_int(m_session, key, def));
    form->addRow(label, combo);
    return combo;
}

QCheckBox *SettingsDialog::addCheck(QFormLayout *form, const QString &label,
                                    const char *key, int def)
{
    auto *check = new QCheckBox(this);
    check->setChecked(adamsession_get_int(m_session, key, def) != 0);
    form->addRow(label, check);
    return check;
}

bool SettingsDialog::saveValue(const char *key, int def, int value)
{
    if (adamsession_get_int(m_session, key, def) == value)
        return false;
    adamsession_set_int(m_session, key, value);
    return true;
}

void SettingsDialog::accept()
{
    const QByteArray addr = m_externalAddr->text().trimmed().toUtf8();
    if (m_external->isChecked()) {
        char host[64];
        int port;
        if (adamsession_parse_hostport(addr.constData(), host, sizeof host,
                                       &port) != 0) {
            QMessageBox::warning(
                this, QStringLiteral("External FujiNet"),
                QStringLiteral("\"%1\" is not an address:port. Example: "
                               "0.0.0.0:65216")
                    .arg(m_externalAddr->text()));
            m_externalAddr->setFocus();
            return;
        }
    }

    m_machineDirty |= saveValue("machine", 0, m_machine->currentIndex());
    m_machineDirty |= saveValue("palette", 0, m_palette->currentIndex());
    m_machineDirty |= saveValue("expansion", 0, m_expansion->currentIndex());
    m_machineDirty |=
        saveValue("joystick_mode", 1, m_joystick->currentIndex());
    m_machineDirty |=
        saveValue("swap_buttons", 0, m_swapButtons->isChecked() ? 1 : 0);
    m_machineDirty |=
        saveValue("reverse_keypad", 0, m_reverseKeypad->isChecked() ? 1 : 0);

    m_machineDirty |=
        saveValue("fujinet_external", 0, m_external->isChecked() ? 1 : 0);
    if (QString::fromUtf8(adamsession_get_str(m_session,
                                              "fujinet_external_addr", "")) !=
        QString::fromUtf8(addr)) {
        adamsession_set_str(m_session, "fujinet_external_addr",
                            addr.constData());
        m_machineDirty = true;
    }

    saveValue("aspect_mode", 0, m_aspect->currentIndex());
    saveValue("smooth_scaling", 0, m_smooth->isChecked() ? 1 : 0);
    QDialog::accept();
}

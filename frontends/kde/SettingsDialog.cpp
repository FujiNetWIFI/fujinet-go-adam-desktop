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
#include "help/HelpWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QShortcut>
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

    /* Help, F1 and Ctrl+Shift+H (the app's shortcuts don't reach a modal dialog) */
    setProperty("adamHelpContext", QStringLiteral("prefs/Machine"));
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Help, this);
    connect(buttons, &QDialogButtonBox::helpRequested, this, [this] { adamHelpFor(this); });
    for (const char *key : { "F1", "Ctrl+Shift+H" })
        new QShortcut(QKeySequence(QString::fromLatin1(key)), this, [this] { adamHelpFor(this); });
    connect(buttons, &QDialogButtonBox::accepted, this,
            &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this,
            &SettingsDialog::reject);

    auto *general = new QWidget(this);
    auto *generalLayout = new QVBoxLayout(general);
    generalLayout->addWidget(machineBox);
    generalLayout->addWidget(fujinetBox);
    generalLayout->addWidget(displayBox);
    generalLayout->addStretch(1);

    auto *tabs = new QTabWidget(this);
    tabs->addTab(general, QStringLiteral("General"));
    tabs->addTab(buildInputPage(), QStringLiteral("Input"));
    connect(tabs, &QTabWidget::currentChanged, this, [this](int page) {
        setProperty("adamHelpContext", page == 1 ? QStringLiteral("prefs/Input")
                                                 : QStringLiteral("prefs/Machine"));
    });

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(buttons);
}

/* The Input page: one row per pad (its controller, applied at once and
 * remembered, so not part of what OK restarts for), Reset Controls and Open
 * Controllers. Keys and gamepad buttons are re-mapped in the Controllers
 * window. */
QWidget *SettingsDialog::buildInputPage()
{
    auto *page = new QWidget(this);
    auto *pageLayout = new QVBoxLayout(page);

    auto *padsBox = new QGroupBox(QStringLiteral("Gamepads"), page);
    auto *padsLayout = new QVBoxLayout(padsBox);
    auto *padsNote = new QLabel(
        QStringLiteral("The first pad is Controller 1, the next Controller 2, unless chosen here."), padsBox);
    padsNote->setWordWrap(true);
    padsLayout->addWidget(padsNote);
    m_padForm = new QFormLayout;
    padsLayout->addLayout(m_padForm);
    pageLayout->addWidget(padsBox);
    m_padGen = adamsession_gamepad_generation(m_session);
    rebuildPads();
    auto *padTimer = new QTimer(this);
    connect(padTimer, &QTimer::timeout, this, [this] {
        const unsigned gen = adamsession_gamepad_generation(m_session);
        if (gen == m_padGen) return;
        m_padGen = gen;
        rebuildPads();
    });
    padTimer->start(1000);

    auto *controls = new QGroupBox(QStringLiteral("Controls"), page);
    auto *controlsLayout = new QVBoxLayout(controls);
    auto *controlsNote = new QLabel(
        QStringLiteral("Keys and gamepad buttons are re-mapped in the Controllers window (Map)."), controls);
    controlsNote->setWordWrap(true);
    controlsLayout->addWidget(controlsNote);
    auto *row = new QHBoxLayout;
    auto *resetControls = new QPushButton(QStringLiteral("Reset Controls"), controls);
    resetControls->setToolTip(QStringLiteral("Put every key and gamepad button back to its default"));
    auto *openWindow = new QPushButton(QStringLiteral("Open Controllers"), controls);
    openWindow->setToolTip(QStringLiteral("Close Preferences and open the Controllers window"));
    row->addWidget(resetControls);
    row->addWidget(openWindow);
    row->addStretch(1);
    controlsLayout->addLayout(row);
    auto *state = new QLabel(controls);
    controlsLayout->addWidget(state);
    pageLayout->addWidget(controls);
    pageLayout->addStretch(1);

    connect(resetControls, &QPushButton::clicked, this, [this, state] {
        adamsession_bindings_reset(m_session);
        state->setText(QStringLiteral("Every key and gamepad button is back to its default."));
    });
    connect(openWindow, &QPushButton::clicked, this, [this] {
        m_openControllers = true;
        accept();
    });
    return page;
}

void SettingsDialog::rebuildPads()
{
    for (QWidget *row : m_padRows)
        m_padForm->removeRow(row);
    m_padRows.clear();
    const int count = adamsession_gamepad_count(m_session);
    if (count == 0) {
        auto *none = new QLabel(QStringLiteral(
            "No gamepads connected - plug one in: it is picked up as it appears."));
        none->setWordWrap(true);
        none->setStyleSheet(QStringLiteral("color: gray;"));
        m_padForm->addRow(none);
        m_padRows.push_back(none);
        return;
    }
    for (int idx = 0; idx < count && idx < 8; ++idx) {
        char name[128];
        adamsession_gamepad_name(m_session, idx, name, sizeof name);
        auto *box = new QComboBox;
        box->addItems({QStringLiteral("Automatic"), QStringLiteral("Controller 1"),
                       QStringLiteral("Controller 2")});
        const int assign = adamsession_gamepad_assignment(m_session, idx);
        box->setCurrentIndex(assign < 0 || assign > 1 ? 0 : assign + 1);
        auto tip = [this, idx](QComboBox *combo) {
            const int eff = adamsession_gamepad_effective_port(m_session, idx);
            combo->setToolTip(eff >= 0 ? QStringLiteral("Driving controller %1").arg(eff + 1)
                                       : QStringLiteral("Driving nothing"));
        };
        tip(box);
        connect(box, &QComboBox::currentIndexChanged, box, [this, idx, box, tip](int sel) {
            adamsession_gamepad_assign(m_session, idx, sel <= 0 ? -1 : sel - 1);
            tip(box);
        });
        m_padForm->addRow(QString::fromUtf8(name), box);
        m_padRows.push_back(box);
    }
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

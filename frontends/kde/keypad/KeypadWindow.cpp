/*
 * KeypadWindow -- see KeypadWindow.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KeypadWindow.h"

#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QVBoxLayout>

#include "../KeyForward.h"

/* ---- PadButton ------------------------------------------------------------ */

PadButton::PadButton(const QString &face, int target, QWidget *parent)
    : QPushButton(face, parent), m_target(target), m_face(face)
{
    /* Not focusable: a click must not steal the keyboard from the window,
     * and tabbing through thirty buttons is nobody's idea of input. */
    setFocusPolicy(Qt::NoFocus);
    setCheckable(true);
}

void PadButton::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && !m_down) {
        m_down = true;
        emit pressedTarget(m_target);
    }
    QPushButton::mousePressEvent(e);
}

void PadButton::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_down) {
        m_down = false;
        emit releasedTarget(m_target, rect().contains(e->position().toPoint()));
    }
    QPushButton::mouseReleaseEvent(e);
}

/* Dragging off a button must release it, or the machine believes it is held
 * forever. */
void PadButton::leaveEvent(QEvent *e)
{
    if (m_down) {
        m_down = false;
        emit releasedTarget(m_target, false);
    }
    QPushButton::leaveEvent(e);
}

/* ---- KeypadWindow ----------------------------------------------------------- */

/* Fixed button sizes, so the panel is the same shape in both modes. */
static constexpr int kButtonWidth = 100;
static constexpr int kButtonHeight = 44;
static constexpr int kSpacing = 6;

static const char *const kKeypadFace[ADAM_KEYPAD_KEYS] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "#"
};
/* The physical 3x4 layout: 1-9 in reading order, then * 0 #. */
static const int kKeypadOrder[ADAM_KEYPAD_KEYS] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 11
};

KeypadWindow::KeypadWindow(adamsession *session, QWidget *parent)
    /* Qt::Dialog, not Qt::Window: a plain window is an ordinary top-level
     * and a tiling window manager tiles it, stretching a panel of fixed-size
     * buttons across half the screen. A dialog is a utility window --
     * floated by every WM, kept above its parent, off the tiling grid. */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint |
                          Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint),
      m_session(session)
{
    setWindowTitle(QStringLiteral("Controllers"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    /* Size to the controls and stay there: nothing here benefits from more
     * room, and stretching only pulls the buttons out of shape. */
    root->setSizeConstraint(QLayout::SetFixedSize);
    auto *ports = new QHBoxLayout;
    ports->addWidget(buildController(0));
    ports->addWidget(buildController(1));
    root->addLayout(ports);

    auto *sys = new QHBoxLayout;
    sys->addStretch();
    for (int sa = 0; sa < ADAM_SYSACT_COUNT; sa++) {
        PadButton *b = control(
            QString::fromUtf8(adam_target_short_name(ADAM_TARGET_SYSACT(sa))),
            ADAM_TARGET_SYSACT(sa));
        b->setFixedWidth(qMax(kButtonWidth,
                              b->fontMetrics().horizontalAdvance(b->face()) + 40));
        sys->addWidget(b);
    }
    sys->addStretch();
    root->addLayout(sys);

    auto *maprow = new QHBoxLayout;
    m_mapButton = new QPushButton(QStringLiteral("Map"));
    m_mapButton->setFocusPolicy(Qt::NoFocus);
    m_mapButton->setCheckable(true);
    connect(m_mapButton, &QPushButton::clicked, this,
            [this](bool on) { setMapState(on ? -1 : -2); });
    m_clearButton = new QPushButton(QStringLiteral("Clear"));
    m_clearButton->setFocusPolicy(Qt::NoFocus);
    m_clearButton->setToolTip(
        QStringLiteral("Unbind the key and gamepad button of the picked control"));
    connect(m_clearButton, &QPushButton::clicked, this, [this] {
        if (m_mapState < 0) return;
        adamsession_binding_set_key(m_session, m_mapState, 0, nullptr, 0);
        adamsession_binding_set_button(m_session, m_mapState,
                                       ADAM_PAD_BTN_NONE, nullptr, 0);
        refreshLabels();
        m_hint->setText(QStringLiteral("%1 cleared").arg(
            QString::fromUtf8(adam_target_name(m_mapState))));
    });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    defaults->setFocusPolicy(Qt::NoFocus);
    connect(defaults, &QPushButton::clicked, this, [this] {
        adamsession_bindings_reset(m_session);
        refreshLabels();
        m_hint->setText(QStringLiteral("Default bindings restored"));
    });
    /* Two lines of hint, whatever it says: a long message must not widen
     * the fixed-size window. */
    m_hint = new QLabel;
    m_hint->setForegroundRole(QPalette::PlaceholderText);
    m_hint->setWordWrap(true);
    m_hint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_hint->setFixedHeight(2 * m_hint->fontMetrics().lineSpacing());
    /* One size for the three, so the checked (accent) box of Map cannot move
     * the row. */
    QSize bs = m_mapButton->sizeHint();
    bs = bs.expandedTo(m_clearButton->sizeHint()).expandedTo(defaults->sizeHint());
    for (QPushButton *b : { m_mapButton, m_clearButton, defaults })
        b->setFixedSize(bs);
    maprow->addWidget(m_mapButton);
    maprow->addWidget(m_clearButton);
    maprow->addWidget(defaults);
    maprow->addWidget(m_hint, 1);
    root->addLayout(maprow);

    connect(&m_pollTimer, &QTimer::timeout, this, &KeypadWindow::poll);
    setMapState(-2);
}

PadButton *KeypadWindow::control(const QString &face, int target)
{
    auto *b = new PadButton(face, target);
    b->setFixedSize(kButtonWidth, kButtonHeight);
    connect(b, &PadButton::pressedTarget, this, &KeypadWindow::onPressed);
    connect(b, &PadButton::releasedTarget, this, &KeypadWindow::onReleased);
    m_controls.push_back(b);
    return b;
}

QWidget *KeypadWindow::buildController(int port)
{
    /* The heading is an explicit centred label rather than the group box's
     * own title: QGroupBox's title alignment is advisory, and several styles
     * draw it left regardless. */
    auto *box = new QGroupBox;
    auto *v = new QVBoxLayout(box);
    v->setSpacing(kSpacing);

    auto *title = new QLabel(QStringLiteral("Controller %1").arg(port + 1));
    title->setAlignment(Qt::AlignHCenter);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    v->addWidget(title);

    auto add = [&](const QString &face, int act) {
        return control(face, ADAM_TARGET_PORT(port, act));
    };

    auto *pad = new QGridLayout;
    pad->setSpacing(kSpacing);
    for (int i = 0; i < ADAM_KEYPAD_KEYS; i++) {
        const int key = kKeypadOrder[i];
        pad->addWidget(add(QString::fromUtf8(kKeypadFace[key]),
                           ADAM_ACT_KEYPAD + key), i / 3, i % 3);
    }
    v->addLayout(pad);

    auto *stick = new QGridLayout;
    stick->setSpacing(kSpacing);
    stick->addWidget(add(QStringLiteral("▲"), ADAM_ACT_UP), 0, 1);
    stick->addWidget(add(QStringLiteral("◀"), ADAM_ACT_LEFT), 1, 0);
    stick->addWidget(add(QStringLiteral("▶"), ADAM_ACT_RIGHT), 1, 2);
    stick->addWidget(add(QStringLiteral("▼"), ADAM_ACT_DOWN), 2, 1);
    v->addLayout(stick);

    /* The two fire buttons share the keypad's width. */
    auto *fires = new QHBoxLayout;
    fires->setSpacing(kSpacing);
    const int fireWidth = (3 * kButtonWidth + kSpacing) / 2;
    for (const int act : { (int)ADAM_ACT_FIRE_L, (int)ADAM_ACT_FIRE_R }) {
        PadButton *b = add(QString::fromUtf8(adam_target_short_name(
                               ADAM_TARGET_PORT(port, act))), act);
        b->setFixedWidth(fireWidth);
        fires->addWidget(b);
    }
    v->addLayout(fires);
    return box;
}

void KeypadWindow::onPressed(int target)
{
    /* Armed: a press picks the control (again, if one is already picked). */
    if (m_mapState != -2) { setMapState(target); return; }
    m_mouseHeld = target;
    if (target >= ADAM_TARGET_SYSACT(0)) return;   /* acts on release */
    adamsession_press(m_session, target, 1);
}

void KeypadWindow::onReleased(int target, bool inside)
{
    if (m_mouseHeld != target) return;             /* it was a Map pick */
    m_mouseHeld = -1;
    if (target >= ADAM_TARGET_SYSACT(0)) {
        /* A real button: pressing and dragging off must not reset the
         * machine. */
        if (inside) adamsession_press(m_session, target, 1);
        return;
    }
    adamsession_press(m_session, target, 0);
}

void KeypadWindow::setMapState(int state)
{
    m_mapState = state;
    adamsession_gamepad_capture_cancel(m_session);
    m_mapButton->setChecked(state != -2);
    m_clearButton->setEnabled(state >= 0);
    if (state == -2) {
        m_hint->clear();
    } else if (state == -1) {
        m_hint->setText(QStringLiteral("Click a control to remap"));
    } else {
        m_hint->setText(QStringLiteral("Press a key or gamepad button for %1")
                            .arg(QString::fromUtf8(adam_target_name(state))));
        adamsession_gamepad_capture_begin(m_session);
    }
    refreshLabels();
    if (state == -2) poll();
}

/* A fresh binding is announced, with whoever lost the key or button named,
 * and Map goes back to waiting for the next control. */
void KeypadWindow::setBound(int target, const QString &what, const char *stolen)
{
    setMapState(-1);
    QString msg = QStringLiteral("%1 = %2")
                      .arg(QString::fromUtf8(adam_target_name(target)), what);
    if (stolen && stolen[0])
        msg += QStringLiteral(" (taken from %1)").arg(QString::fromUtf8(stolen));
    m_hint->setText(msg);
}

void KeypadWindow::refreshLabels()
{
    for (PadButton *b : m_controls) {
        if (m_mapState == -2) {
            b->setText(b->face());
            b->setToolTip(QString());
            continue;
        }
        const adam_binding bind = adamsession_binding_get(m_session, b->target());
        char key[48];
        adamsession_keysym_name(bind.keysym, key, sizeof key);
        const QString keyText = key[0] ? QString::fromUtf8(key)
                                       : QStringLiteral("—");
        const QString padText = bind.button != ADAM_PAD_BTN_NONE
            ? QString::fromUtf8(adam_pad_button_name(bind.button)) : QString();

        /* Each line elided to the button; a '&' would read as a mnemonic. */
        const int room = kButtonWidth - 18;
        auto fit = [&](const QString &t) {
            return b->fontMetrics().elidedText(t, Qt::ElideRight, room)
                .replace(QLatin1Char('&'), QStringLiteral("&&"));
        };
        QString text = fit(keyText);
        if (!padText.isEmpty()) text += QLatin1Char('\n') + fit(padText);
        b->setText(text);
        b->setToolTip(QStringLiteral("%1: %2%3")
            .arg(QString::fromUtf8(adam_target_name(b->target())), keyText,
                 padText.isEmpty() ? QString()
                                   : QStringLiteral(" / Gamepad ") + padText));
        b->setHeld(b->target() == m_mapState);
    }
}

/* What the machine sees held, from every source, lit in the accent colour;
 * in Map mode, the gamepad capture. */
void KeypadWindow::poll()
{
    if (m_mapState >= 0) {
        int button;
        if (adamsession_gamepad_capture_poll(m_session, &button)) {
            const int target = m_mapState;
            char stolen[160];
            adamsession_binding_set_button(m_session, target, button, stolen,
                                           sizeof stolen);
            setBound(target,
                     QStringLiteral("Gamepad %1")
                         .arg(QString::fromUtf8(adam_pad_button_name(button))),
                     stolen);
        }
        return;
    }
    if (m_mapState != -2) return;
    const uint32_t held[2] = { adamsession_buttons_held(m_session, 0),
                               adamsession_buttons_held(m_session, 1) };
    for (PadButton *b : m_controls) {
        const int t = b->target();
        if (t < 2 * ADAM_ACT_PER_PORT)
            b->setHeld(held[t / ADAM_ACT_PER_PORT] &
                       (1u << (t % ADAM_ACT_PER_PORT)));
    }
}

void KeypadWindow::showEvent(QShowEvent *e)
{
    poll();
    m_pollTimer.start(50);
    QWidget::showEvent(e);
}

void KeypadWindow::hideEvent(QHideEvent *e)
{
    m_pollTimer.stop();
    if (m_mouseHeld >= 0 && m_mouseHeld < ADAM_TARGET_SYSACT(0))
        adamsession_press(m_session, m_mouseHeld, 0);
    m_mouseHeld = -1;
    setMapState(-2);
    QWidget::hideEvent(e);
}

/* Focus loss: nothing the keyboard held may stay held. */
void KeypadWindow::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::ActivationChange && !isActiveWindow())
        adamsession_release_all(m_session);
    QWidget::changeEvent(e);
}

/* Tab types on the ADAM (and binds in Map mode); it is not focus traversal. */
bool KeypadWindow::event(QEvent *e)
{
    if (e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) {
            keyPressEvent(ke);
            return true;
        }
    }
    return QWidget::event(e);
}

void KeypadWindow::keyPressEvent(QKeyEvent *e)
{
    /* F9 toggles this window, from inside it too. */
    if (e->key() == Qt::Key_F9) {
        if (!e->isAutoRepeat()) hide();
        return;
    }
    if (m_mapState >= 0) {
        /* Any other key binds, modifiers and Escape included -- except the
         * chords the main window keeps, which a binding could never see. */
        const uint32_t ks = adamKeysymFromQt(e);
        if (e->isAutoRepeat() || !ks) return;
        if (e->key() >= Qt::Key_F10 && e->key() <= Qt::Key_F12) {
            m_hint->setText(QStringLiteral("F9 to F12 are reserved"));
            return;
        }
        const int target = m_mapState;
        char stolen[160], name[48];
        adamsession_binding_set_key(m_session, target, ks, stolen, sizeof stolen);
        adamsession_keysym_name(ks, name, sizeof name);
        setBound(target, QString::fromUtf8(name), stolen);
        return;
    }
    if (m_mapState == -1) return;
    if (!adamForwardKey(m_session, e, true)) QWidget::keyPressEvent(e);
}

/* A release always goes to the session, in Map mode too: it only lets go of
 * what a press asserted, so a key held while Map was armed cannot stick. */
void KeypadWindow::keyReleaseEvent(QKeyEvent *e)
{
    if (!adamForwardKey(m_session, e, false)) QWidget::keyReleaseEvent(e);
}

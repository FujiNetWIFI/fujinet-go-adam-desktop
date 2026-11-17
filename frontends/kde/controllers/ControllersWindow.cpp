/*
 * ControllersWindow -- see ControllersWindow.h.
 *
 * The two ADAM hand controllers as a player sees them, side by side
 * (Controller 1 on the left) in a taupe case under a thin brand strip: each
 * a cream body with a short stick on a disc at the top ringed by eight
 * direction wedges, a fire button down either side and the 12-key phone keypad
 * in the frame of the overlay-card slot. Below the case, a strip with the
 * app's two console actions, Reset Computer and Reset Game. Everything is
 * painted from the session's layout (adamsession_controls_layout, in units
 * of a keypad key's width), so every frontend shows the same parts in the same
 * places; the colours are this file's.
 *
 * Gestures act on mouse press AND release: the console samples its controllers
 * once a frame, so a press present only for the instant of a click would fall
 * between frames. A key or a fire button is held while the mouse is down on it
 * (dragging off lets it go); a press on the stick or a wedge tilts it, and a
 * drag moves on from one direction to the next. One contact at a time: chords
 * come from the keyboard or a pad. Hiding the window and losing the focus let
 * go of everything the mouse holds.
 *
 * What the console sees held -- host keyboard, gamepads and this window -- is
 * lit in the accent, so the window doubles as an input tester for a new pad.
 * Keys typed with it active go to the console as in the main window.
 *
 * FIXED SIZE: the drawing is a fixed number of pixels per layout unit, Map
 * legends are fitted into their parts, and gamepad names are elided, so
 * neither Map mode nor a long name resizes the window. A singleton, hidden
 * rather than destroyed, so a half-done remap survives closing it. A timer
 * runs only while it is visible.
 *
 * MAP MODE: Map arms a two-step rebind -- click a part (a wedge, a fire
 * button, a key, or one of the console actions below), then press a host key or
 * a gamepad button. Rebinding steals the key from whatever held it (named in
 * the hint). Escape cancels; while Map is armed nothing is typed into the
 * console.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ControllersWindow.h"

#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPolygonF>
#include <QPushButton>
#include <QRadialGradient>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QtMath>

#include <functional>
#include <utility>
#include <vector>

#include "../Accent.h"
#include "../KeyForward.h"
#include "../debugger/DebuggerWindow.h"
#include "../help/HelpWindow.h"

namespace {

constexpr qreal kUnit = 40.0;        /* pixels per layout unit (a keypad key's width) */
constexpr int kCasePad = 18;         /* the case's margin round the controls */
constexpr int kBrandH = 22;          /* the brand strip along the top */
constexpr int kTopGap = 12;
constexpr int kTop = kBrandH + kTopGap;   /* where the layout's origin sits */
constexpr int kStripGap = 10;        /* between the case and the console strip */
constexpr int kStripH = 34;
constexpr int kStripW = 214;
constexpr int kStripSpace = 12;
constexpr int kStripTail = 6;
constexpr int kMaxPadRows = 3;
constexpr qreal kDimAlpha = 0.4;     /* an unpicked part while Map waits for a key */
constexpr qreal kCapReach = 0.14;    /* the stick cap's travel, in disc radii */

/* ADAM cream: the colours of coleco's black controller, recoloured (DESIGN-adam.md). */
const QColor kInk(0x2E, 0x2A, 0x26);        /* dark text on the case */
const QColor kDimInk(0x7A, 0x71, 0x5C);
const QColor kCaseTop(0xBD, 0xB2, 0x93);    /* the case's warm taupe */
const QColor kCaseBottom(0xA3, 0x98, 0x77);
const QColor kCaseEdge(0x6E, 0x66, 0x50);
const QColor kCaseLight(0xD4, 0xCB, 0xB0);
const QColor kBrandBar(0x1E, 0x1B, 0x1A);   /* the ADAM logo bar's black */
const QColor kBodyTop(0xED, 0xE5, 0xCE);    /* a controller's cream body */
const QColor kBodyBottom(0xD9, 0xCF, 0xB2);
const QColor kBodyEdge(0x8F, 0x84, 0x66);
const QColor kBodyLight(0xF7, 0xF2, 0xE3);
const QColor kPlate(0xCF, 0xC6, 0xAE);      /* the stick's socket ring */
const QColor kPlateEdge(0xA8, 0x9E, 0x84);
const QColor kHub(0xA9, 0x9D, 0x7E);
const QColor kWedge(0xE1, 0xD9, 0xC0);
const QColor kWedgeHover(0xEF, 0xE9, 0xD6);
const QColor kWedgeEdge(0x8F, 0x84, 0x66);
const QColor kCapLight(0xDD, 0xD5, 0xBE);
const QColor kCapDark(0xB8, 0xAE, 0x92);
const QColor kFireTop(0xD9, 0x46, 0x1E);    /* the orange-red side buttons */
const QColor kFireSide(0x7C, 0x24, 0x0F);
const QColor kFireHover(0xE8, 0x5A, 0x30);
const QColor kCardTop(0xB9, 0xBB, 0xBE);    /* the overlay card's silver faceplate */
const QColor kCardBottom(0x8E, 0x90, 0x93);
const QColor kCardEdge(0x4A, 0x4A, 0x4E);
const QColor kSlot(0x0B, 0x0B, 0x0C);
const QColor kKeyTop(0xB3, 0xAA, 0x92);     /* the keypad's warm grey caps */
const QColor kKeyHover(0xC4, 0xBC, 0xA6);
const QColor kKeySide(0x8A, 0x80, 0x68);
const QColor kKeyInk(0x4A, 0x44, 0x38);
const QColor kStripCap(0xCB, 0xC1, 0xA6);   /* the console strip's buttons */
const QColor kStripHover(0xDD, 0xD5, 0xBE);
const QColor kStripSide(0x8F, 0x84, 0x66);

QString local(const char *text) { return QString::fromUtf8(text ? text : ""); }

QFont scaledFont(QFont fnt, qreal scale, bool bold = false)
{
    fnt.setPointSizeF((fnt.pointSizeF() > 0 ? fnt.pointSizeF() : 10) * scale);
    fnt.setBold(bold);
    return fnt;
}

QFont pixelFont(QFont fnt, int pixels, bool bold)
{
    fnt.setPixelSize(pixels);
    fnt.setBold(bold);
    return fnt;
}

/* Lines centred in rect, the font shrunk until they fit. */
void drawFitted(QPainter &pnt, const QRectF &rect, const QString &text, QFont fnt)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (;;) {
        const QFontMetricsF metrics(fnt);
        qreal wide = 0;
        for (const QString &line : lines) wide = qMax(wide, metrics.horizontalAdvance(line));
        const qreal size = fnt.pixelSize() > 0 ? fnt.pixelSize() : fnt.pointSizeF();
        if ((wide <= rect.width() && metrics.lineSpacing() * lines.size() <= rect.height() + 2) || size <= 5.0)
            break;
        if (fnt.pixelSize() > 0) fnt.setPixelSize(fnt.pixelSize() - 1);
        else fnt.setPointSizeF(fnt.pointSizeF() - 0.5);
    }
    pnt.setFont(fnt);
    pnt.drawText(rect, Qt::AlignCenter, text);
}

/* A part's key as the window writes it, or a dash for none. */
QString keyName(adamsession *sess, int target)
{
    const adam_binding bind = adamsession_binding_get(sess, target);
    char name[48] = "";
    if (bind.keysym) adamsession_keysym_name(bind.keysym, name, sizeof name);
    return local(name);
}

/* The legend on a part: long key names shortened to fit (tooltips keep the full name). */
QString legendName(QString key)
{
    if (key.startsWith(QLatin1String("Keypad "))) return QStringLiteral("KP ") + key.mid(7);
    if (key.startsWith(QLatin1String("Left "))) return QStringLiteral("L ") + key.mid(5);
    if (key.startsWith(QLatin1String("Right "))) return QStringLiteral("R ") + key.mid(6);
    if (key == QLatin1String("Backspace")) return QStringLiteral("BkSp");
    if (key == QLatin1String("Escape")) return QStringLiteral("Esc");
    return key;
}

QString bindingText(adamsession *sess, int target)
{
    const QString key = keyName(sess, target);
    return key.isEmpty() ? QStringLiteral("—") : legendName(key);
}

QString bindingTip(adamsession *sess, int target)
{
    const adam_binding bind = adamsession_binding_get(sess, target);
    const QString key = keyName(sess, target);
    const QString pad = bind.button != ADAM_PAD_BTN_NONE ? local(adam_pad_button_name(bind.button))
                                                           : QStringLiteral("none");
    return QStringLiteral("%1\nKey: %2    Gamepad: %3")
        .arg(local(adam_target_name(target)), key.isEmpty() ? QStringLiteral("none") : key, pad);
}

QPointF unit(qreal deg)   /* a direction, degrees counter-clockwise from 3 o'clock, as drawn */
{
    return QPointF(std::cos(qDegreesToRadians(deg)), -std::sin(qDegreesToRadians(deg)));
}

QColor mixWhite(const QColor &base, qreal part)
{
    return QColor(int(base.red() + (255 - base.red()) * part), int(base.green() + (255 - base.green()) * part),
                  int(base.blue() + (255 - base.blue()) * part));
}

/* ---- ElidedLabel ------------------------------------------------------------ */

/* A label that never widens the window: one line elides, more wrap into a
 * fixed height. */
class ElidedLabel : public QWidget {
public:
    explicit ElidedLabel(bool dim, int lines = 1) : m_dim(dim), m_lines(lines)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    void setText(const QString &text)
    {
        if (text == m_text) return;
        m_text = text;
        update();
    }
    QSize sizeHint() const override { return QSize(0, fontMetrics().lineSpacing() * m_lines); }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setPen(m_dim ? QColor(Qt::gray) : palette().color(QPalette::WindowText));
        if (m_lines > 1)
            pnt.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, m_text);
        else
            pnt.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(m_text, Qt::ElideRight, width()));
    }

private:
    QString m_text;
    bool m_dim;
    int m_lines;
};

/* ---- ConsoleWidget: the two controllers and the console strip ------------------------- */

class ConsoleWidget : public QWidget {
public:
    /* onPick: a part was chosen in Map mode (its target); onHint: a message for the hint line */
    ConsoleWidget(adamsession *session, std::function<void(int)> onPick, std::function<void(const QString &)> onHint)
        : m_session(session), m_onPick(std::move(onPick)), m_onHint(std::move(onHint))
    {
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        load();
    }

    /* Lets go of everything the mouse holds (also on focus loss and rebuild). */
    void releaseAll()
    {
        setPointer(0, 0);
        setPointer(1, 0);
        m_hold = None;
        m_holdIdx = -1;
        update();
    }

    /* -2 idle, -1 armed, >= 0 waiting for a key or pad button for that target */
    void setMap(int state)
    {
        m_map = state;
        update();
    }
    void setShowBindings(bool visible)
    {
        m_showBindings = visible;
        update();
    }

    /* What the console sees held (keyboard, gamepads, the mouse), and the
     * stick caps easing toward it; repaints only on a change. */
    void pollHeld()
    {
        bool changed = false;
        for (int port = 0; port < ADAM_PORTS; ++port) {
            const unsigned held = adamsession_buttons_held(m_session, port);
            if (held != m_held[port]) {
                m_held[port] = held;
                changed = true;
            }
            if (easeCap(port)) changed = true;
        }
        if (changed) update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setRenderHint(QPainter::Antialiasing);
        paintCase(pnt);
        for (int port = 0; port < ADAM_PORTS; ++port) paintController(pnt, port);
        for (int sysact = 0; sysact < ADAM_SYSACT_COUNT; ++sysact) paintStrip(pnt, sysact);
    }

    void mousePressEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        const QPointF pos = evt->position();
        if (m_map != -2) {
            pick(pos);
            return;
        }
        if (m_hold != None) return;          /* one thing at a time */
        const int strip = stripAt(pos);
        if (strip >= 0) {
            m_hold = Strip;
            m_holdIdx = strip;
            m_stripOn = true;
            update();
            return;
        }
        const int idx = hitAt(pos);
        if (idx < 0) return;
        const adam_control &ctl = m_layout[idx];
        switch (ctl.kind) {
        case ADAM_CTL_KEY:
        case ADAM_CTL_FIRE:
            m_hold = Part;
            m_holdIdx = idx;
            setPointer(ctl.port, ctl.act_mask);
            break;
        case ADAM_CTL_STICK:
        case ADAM_CTL_DIRECTION:
            m_hold = Disc;
            m_dragPort = ctl.port;
            setPointer(ctl.port, discMask(ctl.port, pos));
            break;
        default: return;
        }
        update();
    }

    /* Dragging off a key or a fire button lets it go (and back on, holds it
     * again); on the stick it moves from one direction to the next. */
    void mouseMoveEvent(QMouseEvent *evt) override
    {
        const QPointF pos = evt->position();
        const int hover = m_hold == None ? hoverAt(pos) : -1;
        if (hover != m_hover) {
            m_hover = hover;
            update();
        }
        if (m_hold == Disc) {
            setPointer(m_dragPort, discMask(m_dragPort, pos));
            update();
        } else if (m_hold == Part) {
            const bool over = hitAt(pos) == m_holdIdx;
            const adam_control &ctl = m_layout[m_holdIdx];
            setPointer(ctl.port, over ? ctl.act_mask : 0);
        } else if (m_hold == Strip) {
            m_stripOn = stripAt(pos) == m_holdIdx;
            update();
        }
    }

    void mouseReleaseEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        if (m_hold == Strip && stripAt(evt->position()) == m_holdIdx) {
            /* fires on release, like a real button: dragging off cancels. The
             * main window's queue runs it, as it does for a key or a pad. */
            adamsession_sysaction_post(m_session, m_holdIdx);
        }
        if (m_hold != None) releaseAll();
        m_hover = hoverAt(evt->position());
        update();
    }

    void leaveEvent(QEvent *evt) override
    {
        if (m_hover != -1) {
            m_hover = -1;
            update();
        }
        QWidget::leaveEvent(evt);
    }

    bool event(QEvent *evt) override
    {
        if (evt->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(evt);
            const QString tip = tipAt(help->pos());
            if (tip.isEmpty())
                QToolTip::hideText();
            else
                QToolTip::showText(help->globalPos(), tip, this);
            return true;
        }
        return QWidget::event(evt);
    }

private:
    enum Hold { None, Disc, Part, Strip };
    static constexpr int kStripBase = 1000;   /* m_hover value of a strip button, plus its sysaction */

    /* one controller's parts, by index into the layout */
    struct Pad {
        int caption = -1, overlay = -1, stick = -1;
        int dir[8];
        int fire[2] = { -1, -1 };
        int keycap[ADAM_KEYPAD_KEYS];
        QPointF centre;
        qreal radius = 0;
        QRectF body;
    };

    void load()
    {
        float wide = 0, tall = 0;
        m_layout = adamsession_controls_layout(&m_count, &wide, &tall);
        for (Pad &pad : m_pad) {
            for (int &slot : pad.dir) slot = -1;
            for (int &slot : pad.keycap) slot = -1;
        }
        int nextDir[ADAM_PORTS] = {};
        int nextKey[ADAM_PORTS] = {};
        for (int idx = 0; idx < m_count; ++idx) {
            const adam_control &ctl = m_layout[idx];
            if (ctl.port < 0 || ctl.port >= ADAM_PORTS) continue;
            Pad &pad = m_pad[ctl.port];
            switch (ctl.kind) {
            case ADAM_CTL_CAPTION: pad.caption = idx; break;
            case ADAM_CTL_OVERLAY: pad.overlay = idx; break;
            case ADAM_CTL_STICK: pad.stick = idx; break;
            case ADAM_CTL_DIRECTION:
                if (nextDir[ctl.port] < 8) pad.dir[nextDir[ctl.port]++] = idx;
                break;
            case ADAM_CTL_FIRE: pad.fire[(ctl.act_mask & (1u << ADAM_ACT_FIRE_L)) ? 0 : 1] = idx; break;
            case ADAM_CTL_KEY:
                if (nextKey[ctl.port] < ADAM_KEYPAD_KEYS) pad.keycap[nextKey[ctl.port]++] = idx;
                break;
            default: break;
            }
        }
        for (Pad &pad : m_pad) {
            if (pad.stick < 0 || pad.caption < 0) continue;
            const QRectF disc = rectOf(pad.stick);
            pad.centre = disc.center();
            pad.radius = disc.width() / 2;
            /* the body sits inside the fire buttons, which straddle its edges */
            const QRectF cap = rectOf(pad.caption);
            pad.body = QRectF(cap.left() + 0.35 * kUnit, cap.bottom(), cap.width() - 0.7 * kUnit,
                              (tall - 0.1 - m_layout[pad.caption].height) * kUnit);
        }
        m_caseH = kTop + qCeil(tall * kUnit) + kCasePad;
        m_panelW = qCeil(wide * kUnit) + 2 * kCasePad;
        m_panelH = m_caseH + kStripGap + kStripH + kStripTail;
        setFixedSize(m_panelW, m_panelH);
    }

    QRectF rectOf(int idx) const
    {
        const adam_control &ctl = m_layout[idx];
        return QRectF(kCasePad + ctl.xpos * kUnit, kTop + ctl.ypos * kUnit, ctl.width * kUnit, ctl.height * kUnit);
    }

    int hitAt(const QPointF &pos) const
    {
        return adamsession_controls_hit(float((pos.x() - kCasePad) / kUnit), float((pos.y() - kTop) / kUnit));
    }

    /* the console strip's button under pos (its sysaction), or -1 */
    int stripAt(const QPointF &pos) const
    {
        for (int sysact = 0; sysact < ADAM_SYSACT_COUNT; ++sysact)
            if (stripRect(sysact).contains(pos)) return sysact;
        return -1;
    }

    /* what the pointer is over, for the hover tint: a layout index, or kStripBase + a sysaction */
    int hoverAt(const QPointF &pos) const
    {
        const int strip = stripAt(pos);
        return strip >= 0 ? kStripBase + strip : hitAt(pos);
    }

    QRectF stripRect(int sysact) const
    {
        const qreal total = ADAM_SYSACT_COUNT * kStripW + (ADAM_SYSACT_COUNT - 1) * kStripSpace;
        return QRectF((width() - total) / 2 + sysact * (kStripW + kStripSpace), m_caseH + kStripGap, kStripW,
                      kStripH);
    }

    void setPointer(int port, unsigned mask)
    {
        if (port < 0 || port >= ADAM_PORTS || m_ptrMask[port] == mask) return;
        m_ptrMask[port] = mask;
        adamsession_pointer_set(m_session, port, mask);
    }

    /* The direction a pointer at pos holds on the stick it began on: nothing
     * near the middle, and past the rim still tilts that way. */
    unsigned discMask(int port, const QPointF &pos) const
    {
        const Pad &pad = m_pad[port];
        const QPointF delta = pos - pad.centre;
        return adamsession_stick_from_vector(float(delta.x()), float(delta.y()), float(pad.radius));
    }

    /* Map mode: a click on a part picks its target (a diagonal drives two
     * contacts, so it has none). */
    void pick(const QPointF &pos)
    {
        const int strip = stripAt(pos);
        if (strip >= 0) {
            m_onPick(ADAM_TARGET_SYSACT(strip));
            return;
        }
        const int idx = hitAt(pos);
        if (idx < 0) return;
        const adam_control &ctl = m_layout[idx];
        if (ctl.target >= 0) {
            m_onPick(ctl.target);
        } else if (ctl.kind == ADAM_CTL_DIRECTION) {
            m_onHint(QStringLiteral("A diagonal holds two directions: map Up, Down, Left and Right."));
        } else if (ctl.kind == ADAM_CTL_STICK) {
            m_onHint(QStringLiteral("Map the stick's four wedges: Up, Down, Left and Right."));
        }
    }

    QString tipAt(const QPoint &pos) const
    {
        const int strip = stripAt(pos);
        if (strip >= 0) return bindingTip(m_session, ADAM_TARGET_SYSACT(strip));
        const int idx = hitAt(pos);
        if (idx < 0) return QString();
        const adam_control &ctl = m_layout[idx];
        if (ctl.target >= 0) return bindingTip(m_session, ctl.target);
        switch (ctl.kind) {
        case ADAM_CTL_STICK: return QStringLiteral("Drag to tilt the stick");
        case ADAM_CTL_DIRECTION:
            return QStringLiteral("%1 (a diagonal holds two directions)").arg(local(ctl.label));
        default: return QString();
        }
    }

    bool mapping() const { return m_map != -2; }
    bool showBind() const { return mapping() || m_showBindings; }

    /* A part's lit state: held by anyone, or the one Map picked. */
    bool lit(const adam_control &ctl) const
    {
        if (mapping()) return ctl.target >= 0 && ctl.target == m_map;
        switch (ctl.kind) {
        case ADAM_CTL_KEY:
        case ADAM_CTL_FIRE: return (m_held[ctl.port] & ctl.act_mask) != 0;
        case ADAM_CTL_DIRECTION: return (m_held[ctl.port] & ctl.act_mask) == ctl.act_mask;
        default: return false;
        }
    }

    /* While Map waits for a key, every part but the picked one steps back. */
    bool dimmed(const adam_control &ctl) const { return m_map >= 0 && ctl.target != m_map; }

    /* The cap eases toward the held tilt (0.4 per tick, snapping when close). */
    bool easeCap(int port)
    {
        const unsigned held = mapping() ? 0u : m_held[port];
        auto bit = [held](int act) { return int((held >> act) & 1u); };
        QPointF goal(bit(ADAM_ACT_RIGHT) - bit(ADAM_ACT_LEFT), bit(ADAM_ACT_DOWN) - bit(ADAM_ACT_UP));
        const qreal len = std::hypot(goal.x(), goal.y());
        if (len > 1) goal /= len;
        QPointF &cur = m_cap[port];
        if (cur == goal) return false;
        const QPointF next = cur + (goal - cur) * 0.4;
        cur = std::hypot(goal.x() - next.x(), goal.y() - next.y()) < 0.004 ? goal : next;
        return true;
    }

    /* ---- painting ---- */

    void paintCase(QPainter &pnt)
    {
        const QRectF body(0.5, 0.5, width() - 1.0, m_caseH - 1.0);
        QLinearGradient charcoal(0, 0, 0, m_caseH);
        charcoal.setColorAt(0, kCaseTop);
        charcoal.setColorAt(1, kCaseBottom);
        pnt.setPen(kCaseEdge);
        pnt.setBrush(charcoal);
        pnt.drawRoundedRect(body, 16, 16);

        /* the thin highlight along the top edge */
        pnt.setBrush(Qt::NoBrush);
        pnt.setPen(QPen(kCaseLight, 1.5));
        pnt.drawLine(QPointF(18, 1.5), QPointF(width() - 18, 1.5));

        /* the brand strip, with the accent under it */
        QPainterPath clip;
        clip.addRoundedRect(body, 16, 16);
        pnt.save();
        pnt.setClipPath(clip);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(kBrandBar);
        pnt.drawRect(QRectF(0, 0, width(), kBrandH));
        pnt.setBrush(adamCtlAccent());
        pnt.drawRect(QRectF(0, kBrandH - 2, width(), 2));
        pnt.restore();
        QFont brand = scaledFont(font(), 0.85, true);
        brand.setLetterSpacing(QFont::AbsoluteSpacing, 3);
        pnt.setFont(brand);
        pnt.setPen(mixWhite(adamCtlAccent(), 0.35));
        pnt.drawText(QRectF(0, 0, width(), kBrandH - 2), Qt::AlignCenter, QStringLiteral("ADAM"));
    }

    /* an annular sector round the disc: direction sector * 45 degrees, a little under 45 wide */
    static QPainterPath wedgePath(const QPointF &centre, qreal rad0, qreal rad1, int sector)
    {
        const qreal start = sector * 45.0 - 22.5 + 1.0, sweep = 43.0;
        const QRectF outer(centre.x() - rad1, centre.y() - rad1, 2 * rad1, 2 * rad1);
        const QRectF inner(centre.x() - rad0, centre.y() - rad0, 2 * rad0, 2 * rad0);
        QPainterPath path;
        path.arcMoveTo(outer, start);
        path.arcTo(outer, start, sweep);
        path.arcTo(inner, start + sweep, -sweep);
        path.closeSubpath();
        return path;
    }

    void paintController(QPainter &pnt, int port)
    {
        const Pad &pad = m_pad[port];
        if (pad.stick < 0) return;

        /* the black body, the caption above it */
        QLinearGradient shell(pad.body.topLeft(), pad.body.bottomLeft());
        shell.setColorAt(0, kBodyTop);
        shell.setColorAt(1, kBodyBottom);
        pnt.setPen(QPen(kBodyEdge, 1));
        pnt.setBrush(shell);
        pnt.drawRoundedRect(pad.body, 14, 14);
        pnt.setPen(QPen(kBodyLight, 1));
        pnt.drawLine(pad.body.topLeft() + QPointF(14, 1.5), pad.body.topRight() + QPointF(-14, 1.5));
        pnt.setPen(kInk);
        pnt.setFont(scaledFont(font(), 0.9, true));
        pnt.drawText(rectOf(pad.caption), Qt::AlignCenter, local(m_layout[pad.caption].label));

        paintCard(pnt, pad);
        paintStick(pnt, pad, port);

        for (int slot = 0; slot < 2; ++slot)
            if (pad.fire[slot] >= 0) paintFire(pnt, pad.fire[slot]);
        for (int slot = 0; slot < ADAM_KEYPAD_KEYS; ++slot)
            if (pad.keycap[slot] >= 0) paintKey(pnt, pad.keycap[slot]);
    }

    /* the overlay card's frame behind the keys: a silver faceplate with its slot */
    void paintCard(QPainter &pnt, const Pad &pad)
    {
        if (pad.overlay < 0) return;
        const QRectF rect = rectOf(pad.overlay);
        QLinearGradient silver(rect.topLeft(), rect.bottomLeft());
        silver.setColorAt(0, kCardTop);
        silver.setColorAt(1, kCardBottom);
        pnt.setPen(QPen(kCardEdge, 1));
        pnt.setBrush(silver);
        pnt.drawRoundedRect(rect, 8, 8);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(kCardEdge);
        pnt.drawRoundedRect(QRectF(rect.left() + 10, rect.top() + 4, rect.width() - 20, 5), 2, 2);
        pnt.setBrush(kSlot);
        pnt.drawRoundedRect(QRectF(rect.left() + 11, rect.top() + 4, rect.width() - 22, 3), 1.5, 1.5);
    }

    void paintStick(QPainter &pnt, const Pad &pad, int port)
    {
        const qreal outer = pad.radius, inner = outer * 0.5;
        pnt.setPen(QPen(kPlateEdge, 1.5));
        pnt.setBrush(kPlate);
        pnt.drawEllipse(pad.centre, outer + 3, outer + 3);
        for (int sector = 0; sector < 8; ++sector) {
            if (pad.dir[sector] < 0) continue;
            const adam_control &ctl = m_layout[pad.dir[sector]];
            const bool held = lit(ctl);
            const bool over = !held && m_hover == pad.dir[sector];
            pnt.setOpacity(dimmed(ctl) ? kDimAlpha : 1.0);
            pnt.setPen(QPen(held ? adamCtlAccentDark() : kWedgeEdge, 1));
            pnt.setBrush(held ? adamCtlAccent() : (over ? kWedgeHover : kWedge));
            pnt.drawPath(wedgePath(pad.centre, inner + 1, outer, sector));
            const QPointF mid = pad.centre + unit(sector * 45.0) * ((inner + outer) / 2);
            if (showBind() && ctl.target >= 0) {
                pnt.setPen(held ? adamCtlAccentInk() : kInk);
                drawFitted(pnt, QRectF(mid.x() - 24, mid.y() - 13, 48, 26), bindingText(m_session, ctl.target),
                           scaledFont(font(), 0.7, true));
            } else {
                /* a chevron pointing out */
                const QPointF dir = unit(sector * 45.0), perp(-dir.y(), dir.x());
                const qreal size = sector % 2 ? 3.5 : 5;
                QPolygonF chevron;
                chevron << mid + dir * size << mid - dir * size + perp * (size * 1.3)
                        << mid - dir * size - perp * (size * 1.3);
                pnt.setPen(Qt::NoPen);
                pnt.setBrush(held ? adamCtlAccentInk() : kDimInk);
                pnt.drawPolygon(chevron);
            }
            pnt.setOpacity(1.0);
        }
        /* the hub the cap sits in, then the cap */
        pnt.setPen(QPen(kPlateEdge, 1.5));
        pnt.setBrush(kHub);
        pnt.drawEllipse(pad.centre, inner - 1, inner - 1);
        paintCap(pnt, pad, port);
    }

    /* the stick's cap from above: a short stub with a disc top, shifted toward the tilt */
    void paintCap(QPainter &pnt, const Pad &pad, int port)
    {
        const qreal capRad = pad.radius * 0.5 * 0.82;
        const QPointF tilt = m_cap[port];
        const bool tilted = std::hypot(tilt.x(), tilt.y()) > 0.01;
        const QPointF cen = pad.centre + tilt * (pad.radius * kCapReach);

        /* its shadow lengthens with the tilt */
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(QColor(0, 0, 0, 110));
        pnt.drawEllipse(cen + QPointF(1.5, 2.5) + tilt * 3.0, capRad + 1, capRad + 1);

        QRadialGradient dome(cen + QPointF(-capRad * 0.3, -capRad * 0.35), capRad * 1.5);
        dome.setColorAt(0, kCapLight);
        dome.setColorAt(1, kCapDark);
        pnt.setBrush(dome);
        pnt.setPen(QPen(tilted ? adamCtlAccent() : kWedgeEdge, tilted ? 2.5 : 2.0));
        pnt.drawEllipse(cen, capRad, capRad);

        /* the light crescent on its upper left */
        const qreal glow = capRad * 0.72;
        QPainterPath crescent;
        crescent.arcMoveTo(QRectF(cen.x() - glow, cen.y() - glow, 2 * glow, 2 * glow), 110);
        crescent.arcTo(QRectF(cen.x() - glow, cen.y() - glow, 2 * glow, 2 * glow), 110, 70);
        pnt.setBrush(Qt::NoBrush);
        pnt.setPen(QPen(QColor(0xFA, 0xF6, 0xE8, 170), 2.0, Qt::SolidLine, Qt::RoundCap));
        pnt.drawPath(crescent);
    }

    void paintFire(QPainter &pnt, int idx)
    {
        const adam_control &ctl = m_layout[idx];
        const bool held = lit(ctl);
        const bool over = !held && m_hover == idx;
        const QRectF full = rectOf(idx).adjusted(1, 0, -1, 0);
        pnt.setOpacity(dimmed(ctl) ? kDimAlpha : 1.0);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(kFireSide);
        pnt.drawRoundedRect(full, 7, 7);
        pnt.setBrush(held ? adamCtlAccent() : (over ? kFireHover : kFireTop));
        pnt.setPen(held ? QPen(adamCtlAccentInk(), 2) : QPen(kBodyEdge, 0.8));
        pnt.drawRoundedRect(full.adjusted(1, 1, -1, -4), 6, 6);
        /* the real buttons are blank; the binding shows while mapping */
        if (showBind()) {
            pnt.setPen(held ? adamCtlAccentInk() : QColor(Qt::white));
            QString text = bindingText(m_session, ctl.target);
            text.replace(QLatin1Char(' '), QLatin1Char('\n'));      /* "Left Ctrl" in two lines */
            drawFitted(pnt, full.adjusted(1, 2, -1, -5), text, pixelFont(font(), 11, true));
        }
        pnt.setOpacity(1.0);
    }

    void paintKey(QPainter &pnt, int idx)
    {
        const adam_control &ctl = m_layout[idx];
        const bool held = lit(ctl);
        const bool over = !held && m_hover == idx;
        const QRectF full = rectOf(idx);
        pnt.setOpacity(dimmed(ctl) ? kDimAlpha : 1.0);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(kKeySide);
        pnt.drawRoundedRect(full, 4, 4);
        const QRectF face = full.adjusted(1, 1, -1, -3);
        pnt.setBrush(held ? adamCtlAccent() : (over ? kKeyHover : kKeyTop));
        pnt.setPen(held ? QPen(adamCtlAccentInk(), 2) : QPen(kBodyEdge, 0.8));
        pnt.drawRoundedRect(face, 4, 4);
        pnt.setPen(held ? adamCtlAccentInk() : kKeyInk);
        if (showBind()) {
            /* the digit smaller, the key it is bound to below it */
            pnt.setFont(pixelFont(font(), 12, true));
            pnt.drawText(face.adjusted(0, 0, 0, -face.height() * 0.42), Qt::AlignCenter, local(ctl.label));
            drawFitted(pnt, face.adjusted(1, face.height() * 0.55, -1, 0), bindingText(m_session, ctl.target),
                       pixelFont(font(), 9, true));
        } else {
            pnt.setFont(pixelFont(font(), 15, true));
            pnt.drawText(face, Qt::AlignCenter, local(ctl.label));
        }
        pnt.setOpacity(1.0);
    }

    /* The console actions, along the bottom. */
    void paintStrip(QPainter &pnt, int sysact)
    {
        const int target = ADAM_TARGET_SYSACT(sysact);
        const bool down = m_hold == Strip && m_holdIdx == sysact && m_stripOn;
        const bool picked = mapping() && m_map == target;
        const bool over = !down && m_hover == kStripBase + sysact;
        const QRectF full = stripRect(sysact);
        pnt.setOpacity(m_map >= 0 && !picked ? kDimAlpha : 1.0);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(kStripSide);
        pnt.drawRoundedRect(full, 8, 8);
        const QRectF face = down ? full.adjusted(0, 3, 0, 0) : full.adjusted(0, 0, 0, -3);
        pnt.setBrush(down || picked ? adamCtlAccent() : (over ? kStripHover : kStripCap));
        pnt.setPen(down || picked ? QPen(adamCtlAccentInk(), 1.5) : QPen(kBodyEdge, 0.8));
        pnt.drawRoundedRect(face, 8, 8);
        pnt.setPen(down || picked ? adamCtlAccentInk() : kInk);
        QString text = local(adam_target_short_name(target));
        if (showBind()) text += QStringLiteral("  ") + bindingText(m_session, target);
        drawFitted(pnt, face.adjusted(8, 0, -8, 0), text, scaledFont(font(), 0.9, true));
        pnt.setOpacity(1.0);
    }

    adamsession *m_session;
    std::function<void(int)> m_onPick;
    std::function<void(const QString &)> m_onHint;
    const adam_control *m_layout = nullptr;
    int m_count = 0;
    Pad m_pad[ADAM_PORTS];
    unsigned m_held[ADAM_PORTS] = {};
    QPointF m_cap[ADAM_PORTS];         /* each stick cap's eased tilt, -1..1 */
    unsigned m_ptrMask[ADAM_PORTS] = {};
    bool m_showBindings = false;
    int m_map = -2;
    Hold m_hold = None;
    int m_holdIdx = -1;          /* the part the mouse holds (a sysaction for the strip) */
    int m_dragPort = -1;         /* the stick the mouse began on */
    bool m_stripOn = false;      /* the pointer is still over the strip button it pressed */
    int m_hover = -1;
    int m_caseH = 0;
    int m_panelW = 0, m_panelH = 0;
};

/* ---- KeyboardWidget: the ADAM's own keys ------------------------------------------------------ */

constexpr qreal kKbUnit = 24.0;      /* pixels per keyboard layout unit (a key) */
constexpr int kKbMargin = 10;
constexpr int kKbBarH = 16;          /* the logo bar above the SmartKeys */

const QColor kKbCaseTop(0xE6, 0xDC, 0xC4);     /* the keyboard's cream case */
const QColor kKbCaseBottom(0xD6, 0xCB, 0xAE);
const QColor kKbCaseEdge(0x8F, 0x84, 0x66);
const QColor kKbBar(0x1E, 0x1B, 0x1A);         /* the logo bar */
const QColor kKbAlpha(0xE8, 0xE2, 0xD2);       /* off-white character keys */
const QColor kKbSpecial(0x9A, 0x8E, 0x7E);     /* grey-taupe named keys */
const QColor kKbReturn(0x7F, 0x74, 0x68);
const QColor kKbSmart(0x2A, 0x26, 0x24);       /* black SmartKeys */
const QColor kKbInk(0x2E, 0x2A, 0x26);
const QColor kKbShifted(0x5A, 0x54, 0x4A);
const QColor kKbSmartInk(0xF0, 0xED, 0xE4);
const QColor kKbSide(0x6E, 0x66, 0x50);

class KeyboardWidget : public QWidget {
public:
    explicit KeyboardWidget(adamsession *session) : m_session(session)
    {
        setFocusPolicy(Qt::NoFocus);
        float wide = 0, high = 0;
        m_caps = adamsession_keyboard_layout(&m_count, &wide, &high);
        m_bits.assign(size_t(m_count), 0);
        setFixedSize(qCeil(wide * kKbUnit) + 2 * kKbMargin, kKbBarH + qCeil(high * kKbUnit) + 2 * kKbMargin);
    }

    /* Lets go of the key the mouse holds (latched modifiers stay: the core owns them). */
    void releaseGrab()
    {
        if (m_grab >= 0 && m_grab < m_count)
            adamsession_keyboard_press(m_session, m_grab, 0);
        m_grab = -1;
    }

    /* Map mode: dimmed, and presses do nothing. */
    void setInert(bool inert)
    {
        if (inert)
            releaseGrab();
        m_inert = inert;
        update();
    }

    void pollHeld()
    {
        bool changed = false;
        for (int idx = 0; idx < m_count; ++idx) {
            const int bits = adamsession_keyboard_held(m_session, idx);
            if (bits != m_bits[size_t(idx)]) {
                m_bits[size_t(idx)] = bits;
                changed = true;
            }
        }
        if (changed)
            update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter paint(this);
        paint.setRenderHint(QPainter::Antialiasing);
        const QRectF body(0.5, 0.5, width() - 1.0, height() - 1.0);
        QLinearGradient shell(0, 0, 0, height());
        shell.setColorAt(0, kKbCaseTop);
        shell.setColorAt(1, kKbCaseBottom);
        paint.setPen(QPen(kKbCaseEdge, 1));
        paint.setBrush(shell);
        paint.drawRoundedRect(body, 12, 12);

        /* the logo bar, with the rainbow stripe under it */
        QPainterPath clip;
        clip.addRoundedRect(body, 12, 12);
        paint.save();
        paint.setClipPath(clip);
        paint.setPen(Qt::NoPen);
        paint.setBrush(kKbBar);
        paint.drawRect(QRectF(0, 0, width(), kKbBarH));
        static const QColor rainbow[] = { QColor(0xD6, 0x3A, 0x2F), QColor(0xE8, 0x8A, 0x1E), QColor(0xE8, 0xC8, 0x2A),
                                          QColor(0x3E, 0xA8, 0x4A), QColor(0x2F, 0x6F, 0xC8) };
        const qreal seg = qreal(width()) / 5;
        for (int band = 0; band < 5; ++band) {
            paint.setBrush(rainbow[band]);
            paint.drawRect(QRectF(band * seg, kKbBarH - 2, seg + 1, 2));
        }
        paint.restore();
        paint.setPen(kKbSmartInk);
        QFont brand = scaledFont(font(), 0.8, true);
        brand.setLetterSpacing(QFont::AbsoluteSpacing, 3);
        paint.setFont(brand);
        paint.drawText(QRectF(0, 0, width(), kKbBarH - 2), Qt::AlignCenter, QStringLiteral("ADAM"));

        paint.setOpacity(m_inert ? 0.4 : 1.0);
        for (int idx = 0; idx < m_count; ++idx)
            paintCap(paint, idx);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || m_inert || m_grab >= 0)
            return;
        const int idx = capAt(event->position());
        if (idx < 0)
            return;
        m_grab = idx;
        adamsession_keyboard_press(m_session, idx, 1);
        pollHeld();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || m_grab < 0)
            return;
        releaseGrab();
        pollHeld();
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(event);
            const int idx = capAt(help->pos());
            if (idx < 0 || (m_caps[idx].kind != ADAM_KEYCAP_MODIFIER && m_caps[idx].kind != ADAM_KEYCAP_LOCK)) {
                QToolTip::hideText();
            } else {
                QToolTip::showText(help->globalPos(),
                                   m_caps[idx].kind == ADAM_KEYCAP_LOCK ? QStringLiteral("Stays on until pressed again")
                                                                        : QStringLiteral("Sticks until the next key"),
                                   this);
            }
            return true;
        }
        return QWidget::event(event);
    }

private:
    QRectF rectOf(const adam_keycap &cap) const
    {
        return QRectF(kKbMargin + cap.left * kKbUnit, kKbBarH + kKbMargin + cap.top * kKbUnit, cap.wide * kKbUnit,
                      cap.high * kKbUnit);
    }

    int capAt(const QPointF &pos) const
    {
        for (int idx = m_count - 1; idx >= 0; --idx)
            if (rectOf(m_caps[idx]).contains(pos))
                return idx;
        return -1;
    }

    /* "WILD CARD" and "MOVE/COPY" on two lines, to fit the cap */
    static QString wrapLabel(QString label)
    {
        label.replace(QLatin1Char('/'), QStringLiteral("/\n"));
        label.replace(QLatin1Char(' '), QLatin1Char('\n'));
        return label;
    }

    void paintCap(QPainter &paint, int idx)
    {
        const adam_keycap &cap = m_caps[idx];
        const int bits = m_bits[size_t(idx)];
        const bool active = (bits & 1) != 0;
        const bool latched = (bits & 2) != 0;
        const bool lit = active || latched;
        const QString label = local(cap.label);
        QColor fill = kKbAlpha, ink = kKbInk;
        switch (cap.kind) {
        case ADAM_KEYCAP_NORMAL:
        case ADAM_KEYCAP_SPACE: break;
        case ADAM_KEYCAP_SMARTKEY: fill = kKbSmart; ink = kKbSmartInk; break;
        default: fill = label == QLatin1String("RETURN") ? kKbReturn : kKbSpecial; break;
        }
        const QRectF face = rectOf(cap).adjusted(0, 0, 0, -2);

        paint.setPen(Qt::NoPen);
        paint.setBrush(kKbSide);
        paint.drawRoundedRect(face.translated(0, 2), 4, 4);
        paint.setPen(lit ? QPen(adamCtlAccentDark(), 1.5) : QPen(kKbSide, 1));
        paint.setBrush(lit ? adamCtlAccent() : fill);
        paint.drawRoundedRect(face, 4, 4);

        if (lit)
            ink = adamCtlAccentInk();
        paint.setPen(ink);
        if (cap.kind == ADAM_KEYCAP_ARROW) {
            paintArrow(paint, face, label, ink);
            return;
        }
        if (cap.shifted) {
            paint.setPen(lit ? ink : kKbShifted);
            drawFitted(paint, QRectF(face.left() + 3, face.top() + 1, face.width() - 6, face.height() * 0.4),
                       local(cap.shifted), pixelFont(font(), 9, true));
            paint.setPen(ink);
            drawFitted(paint, QRectF(face.left(), face.top() + face.height() * 0.32, face.width(), face.height() * 0.68),
                       label, pixelFont(font(), 13, true));
        } else if (cap.kind == ADAM_KEYCAP_SMARTKEY) {
            drawFitted(paint, face.adjusted(2, 0, -2, 0), label, pixelFont(font(), 12, true));
        } else {
            const bool word = label.size() > 1;
            drawFitted(paint, face.adjusted(1, 0, -1, 0), wrapLabel(label), pixelFont(font(), word ? 9 : 13, true));
        }
        if (latched) {
            paint.setPen(Qt::NoPen);
            paint.setBrush(adamCtlAccentInk());
            paint.drawEllipse(QPointF(face.left() + 6, face.top() + 6), 2.5, 2.5);
        }
    }

    /* an arrow key: a drawn triangle, so no font has to carry the glyph */
    static void paintArrow(QPainter &paint, const QRectF &face, const QString &glyph, const QColor &ink)
    {
        const QPointF mid = face.center();
        const qreal size = face.height() * 0.28;
        QPolygonF head;
        const ushort code = glyph.isEmpty() ? 0 : glyph.at(0).unicode();
        if (code == 0x2191)
            head << mid + QPointF(0, -size) << mid + QPointF(-size, size * 0.8) << mid + QPointF(size, size * 0.8);
        else if (code == 0x2193)
            head << mid + QPointF(0, size) << mid + QPointF(-size, -size * 0.8) << mid + QPointF(size, -size * 0.8);
        else if (code == 0x2190)
            head << mid + QPointF(-size, 0) << mid + QPointF(size * 0.8, -size) << mid + QPointF(size * 0.8, size);
        else
            head << mid + QPointF(size, 0) << mid + QPointF(-size * 0.8, -size) << mid + QPointF(-size * 0.8, size);
        paint.setPen(Qt::NoPen);
        paint.setBrush(ink);
        paint.drawPolygon(head);
    }

    adamsession *m_session;
    const adam_keycap *m_caps = nullptr;
    int m_count = 0;
    std::vector<int> m_bits;
    int m_grab = -1;              /* the cap the mouse holds */
    bool m_inert = false;
};

/* ---- ControllersWindow --------------------------------------------------------------- */

class ControllersWindow : public QWidget {
public:
    ControllersWindow(adamsession *session, QWidget *parent);
    void present();
    void refresh();

protected:
    void keyPressEvent(QKeyEvent *evt) override;
    void keyReleaseEvent(QKeyEvent *evt) override;
    void showEvent(QShowEvent *evt) override;
    void hideEvent(QHideEvent *evt) override;
    void changeEvent(QEvent *evt) override;
    /* Tab is a key of the console's, not focus traversal. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    void letGo();
    void setMapState(int state);
    void bound(int target, const QString &what, const char *stolen);
    void pollCapture();
    void pollSlow();
    void rebuildPads();
    void refreshPadPorts();

    adamsession *m_session;
    ConsoleWidget *m_console = nullptr;
    KeyboardWidget *m_keys = nullptr;
    QWidget *m_padArea = nullptr;
    QVBoxLayout *m_padBox = nullptr;
    std::vector<QWidget *> m_padRows;
    std::vector<QLabel *> m_padEff;       /* each pad's "drives controller N" */
    unsigned m_padGen = ~0u;
    QPushButton *m_mapButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    ElidedLabel *m_hint = nullptr;
    QTimer m_captureTimer;
    QTimer m_heldTimer;
    QTimer m_slowTimer;
    uint32_t m_swallow = 0;               /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a target, >= 0 waiting for a key or
     * gamepad button for that target. */
    int m_mapState = -2;
};

ControllersWindow::ControllersWindow(adamsession *session, QWidget *parent)
    /* A dialog-type window, so a tiling compositor floats it: it is a panel
     * of fixed-size parts and nothing here wants more room. */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint),
      m_session(session)
{
    setObjectName(QStringLiteral("controllers"));
    setWindowTitle(QStringLiteral("Controllers"));
    setProperty("adamHelpContext", QStringLiteral("controllers"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    /* Sized to its parts and kept there: a resizable panel gets tiled and its
     * parts stretched. */
    root->setSizeConstraint(QLayout::SetFixedSize);

    m_console = new ConsoleWidget(
        session,
        [this](int target) { setMapState(target); },
        [this](const QString &text) { m_hint->setText(text); });
    m_console->setObjectName(QStringLiteral("console"));
    root->addWidget(m_console, 0, Qt::AlignHCenter);

    /* the ADAM's own keyboard, to click: shown by the Keyboard checkbox */
    m_keys = new KeyboardWidget(session);
    m_keys->setObjectName(QStringLiteral("keyboard"));
    m_keys->setVisible(adamsession_get_int(session, "controllers_keyboard", 0) > 0);
    root->addWidget(m_keys, 0, Qt::AlignHCenter);

    /* Gamepads: one row per pad, rebuilt as they come and go, in room for a
     * fixed number so the window does not change size when one is plugged in */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads"));
    auto *padsBox = new QVBoxLayout(pads);
    m_padArea = new QWidget;
    m_padBox = new QVBoxLayout(m_padArea);
    m_padBox->setContentsMargins(0, 0, 0, 0);
    m_padBox->setSpacing(2);
    const int rowHeight = QComboBox().sizeHint().height() + 2;
    m_padArea->setFixedHeight(rowHeight * kMaxPadRows + m_padBox->spacing() * (kMaxPadRows - 1));
    m_padBox->setAlignment(Qt::AlignTop);
    padsBox->addWidget(m_padArea);
    root->addWidget(pads);

    auto *mapRow = new QHBoxLayout;
    m_mapButton = new QPushButton(QStringLiteral("Map"));
    m_mapButton->setFocusPolicy(Qt::NoFocus);
    m_mapButton->setFixedWidth(qMax(m_mapButton->sizeHint().width(),
                                    m_mapButton->fontMetrics().horizontalAdvance(QStringLiteral("Done")) + 32));
    m_mapButton->setToolTip(QStringLiteral("Pick a part, then press a key or a gamepad button to give it"));
    connect(m_mapButton, &QPushButton::clicked, this, [this] { setMapState(m_mapState == -2 ? -1 : -2); });
    m_clearButton = new QPushButton(QStringLiteral("Clear"));
    m_clearButton->setFocusPolicy(Qt::NoFocus);
    m_clearButton->setToolTip(QStringLiteral("Take the picked control's key and gamepad button away"));
    connect(m_clearButton, &QPushButton::clicked, this, [this] {
        if (m_mapState < 0) return;
        char stolen[128] = "";
        adamsession_binding_set_key(m_session, m_mapState, 0, stolen, sizeof stolen);
        adamsession_binding_set_button(m_session, m_mapState, ADAM_PAD_BTN_NONE, stolen, sizeof stolen);
        m_hint->setText(QStringLiteral("Cleared. Press a key or a gamepad button to bind it again"));
        m_console->update();
    });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    defaults->setFocusPolicy(Qt::NoFocus);
    defaults->setToolTip(QStringLiteral("Put every key and gamepad button back to its default"));
    connect(defaults, &QPushButton::clicked, this, [this] {
        adamsession_bindings_reset(m_session);
        setMapState(m_mapState == -2 ? -2 : -1);
        m_hint->setText(QStringLiteral("Every control is back to its default."));
    });
    auto *bindings = new QCheckBox(QStringLiteral("Show bindings"));
    bindings->setFocusPolicy(Qt::NoFocus);
    bindings->setToolTip(QStringLiteral("Show the key each part has (hover for its gamepad button)"));
    /* "controllers_show_bindings"; the older spellings are read when it is absent */
    int savedBindings = adamsession_get_int(session, "controllers_show_bindings", -1);
    if (savedBindings < 0) savedBindings = adamsession_get_int(session, "controllers_bindings", -1);
    if (savedBindings < 0) savedBindings = adamsession_get_int(session, "controllers_key_names", -1);
    if (savedBindings < 0) savedBindings = adamsession_get_int(session, "show_bindings", -1);
    bindings->setChecked(savedBindings > 0);
    connect(bindings, &QCheckBox::toggled, this, [this](bool checked) {
        adamsession_set_int(m_session, "controllers_show_bindings", checked ? 1 : 0);
        m_console->setShowBindings(checked);
    });
    m_console->setShowBindings(bindings->isChecked());
    auto *keyboard = new QCheckBox(QStringLiteral("Keyboard"));
    keyboard->setFocusPolicy(Qt::NoFocus);
    keyboard->setToolTip(QStringLiteral("Show the ADAM's keyboard to click"));
    keyboard->setChecked(m_keys->isVisible());
    connect(keyboard, &QCheckBox::toggled, this, [this](bool show) {
        adamsession_set_int(m_session, "controllers_keyboard", show ? 1 : 0);
        if (!show) {
            m_keys->releaseGrab();
            adamsession_keyboard_release(m_session);
        }
        m_keys->setVisible(show);
    });
    mapRow->addWidget(m_mapButton);
    mapRow->addWidget(m_clearButton);
    mapRow->addWidget(defaults);
    mapRow->addWidget(bindings);
    mapRow->addWidget(keyboard);
    mapRow->addStretch(1);
    mapRow->addWidget(adamHelpButton(QStringLiteral("controllers")));
    root->addLayout(mapRow);
    m_hint = new ElidedLabel(true, 2);
    root->addWidget(m_hint);

    connect(&m_captureTimer, &QTimer::timeout, this, [this] { pollCapture(); });
    connect(&m_heldTimer, &QTimer::timeout, this, [this] { m_console->pollHeld(); m_keys->pollHeld(); });
    connect(&m_slowTimer, &QTimer::timeout, this, [this] { pollSlow(); });
    setMapState(-2);
    letGo();
    m_console->pollHeld();
}

/* Lets go of what the mouse holds in this window. */
void ControllersWindow::letGo()
{
    m_console->releaseAll();
    m_keys->releaseGrab();
}

/* ---- Map mode -------------------------------------------------------------------------- */

void ControllersWindow::setMapState(int state)
{
    m_mapState = state;
    adamCtlSetAccent(m_mapButton, state != -2);
    m_clearButton->setEnabled(state >= 0);
    if (state == -2) {
        m_captureTimer.stop();
        adamsession_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Map"));
        m_hint->setText(QStringLiteral("Press a part with the mouse to hold it. Drag on the stick to tilt it."));
    } else if (state == -1) {
        m_captureTimer.stop();
        adamsession_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Done"));
        m_hint->setText(QStringLiteral("Click a part to remap it. Escape leaves Map."));
    } else {
        letGo();
        m_hint->setText(QStringLiteral("Press a key or a gamepad button for %1 (Escape cancels).")
                            .arg(local(adam_target_name(state))));
        adamsession_gamepad_capture_begin(m_session);
        m_captureTimer.start(50);
    }
    m_console->setMap(m_mapState);
    m_keys->setInert(state != -2);
    m_console->pollHeld();
}

/* A binding was made: stay armed, since remapping several in a row is normal. */
void ControllersWindow::bound(int target, const QString &what, const char *stolen)
{
    setMapState(-1);
    m_hint->setText(stolen[0] ? QStringLiteral("%1 is now %2 (taken from %3)")
                                    .arg(local(adam_target_name(target)), what, local(stolen))
                              : QStringLiteral("%1 is now %2").arg(local(adam_target_name(target)), what));
}

void ControllersWindow::pollCapture()
{
    int button;
    if (m_mapState < 0) { m_captureTimer.stop(); return; }
    if (!adamsession_gamepad_capture_poll(m_session, &button)) return;
    char stolen[128] = "";
    const int target = m_mapState;
    adamsession_binding_set_button(m_session, target, button, stolen, sizeof stolen);
    bound(target, local(adam_pad_button_name(button)), stolen);
}

/* ---- polling ---------------------------------------------------------------------------- */

/* Hot-plugged pads and auto-assignment changes. */
void ControllersWindow::pollSlow()
{
    const unsigned gen = adamsession_gamepad_generation(m_session);
    if (gen != m_padGen) {
        m_padGen = gen;
        rebuildPads();
    } else {
        refreshPadPorts();
    }
}

/* ---- gamepads --------------------------------------------------------------------------- */

void ControllersWindow::refreshPadPorts()
{
    for (int idx = 0; idx < (int)m_padEff.size(); ++idx) {
        const int eff = adamsession_gamepad_effective_port(m_session, idx);
        const QString text = eff >= 0 ? QStringLiteral("drives controller %1").arg(eff + 1)
                                      : QStringLiteral("drives nothing");
        if (m_padEff[(size_t)idx]->text() != text) m_padEff[(size_t)idx]->setText(text);
    }
}

/* One row per pad: its name, the controller it drives and the one it is set to. */
void ControllersWindow::rebuildPads()
{
    for (QWidget *row : m_padRows) delete row;
    m_padRows.clear();
    m_padEff.clear();
    const int count = adamsession_gamepad_count(m_session);
    if (count == 0) {
        auto *none = new QLabel(QStringLiteral("No gamepads connected - plug one in"));
        none->setStyleSheet(QStringLiteral("color: gray;"));
        m_padBox->addWidget(none);
        m_padRows.push_back(none);
        return;
    }
    for (int idx = 0; idx < count && idx < kMaxPadRows; ++idx) {
        char name[128];
        adamsession_gamepad_name(m_session, idx, name, sizeof name);
        auto *row = new QWidget;
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        auto *label = new ElidedLabel(false);
        label->setText(local(name));
        label->setToolTip(local(name));
        auto *eff = new QLabel;
        eff->setStyleSheet(QStringLiteral("color: gray;"));
        /* -1 = automatic (connection order), 0-1 = that controller */
        auto *box = new QComboBox;
        box->setFocusPolicy(Qt::NoFocus);
        box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Controller 1"), QStringLiteral("Controller 2") });
        const int assign = adamsession_gamepad_assignment(m_session, idx);
        box->setCurrentIndex(assign < 0 || assign > 1 ? 0 : assign + 1);
        connect(box, &QComboBox::currentIndexChanged, this, [this, idx](int sel) {
            adamsession_gamepad_assign(m_session, idx, sel <= 0 ? -1 : sel - 1);
            refreshPadPorts();
        });
        line->addWidget(label, 1);
        line->addWidget(eff);
        line->addWidget(box);
        m_padBox->addWidget(row);
        m_padRows.push_back(row);
        m_padEff.push_back(eff);
    }
    if (count > kMaxPadRows) {
        /* the rest are in Preferences ▸ Input */
        m_padEff.back()->setToolTip(QStringLiteral("%1 more gamepads are listed in Preferences").arg(count - kMaxPadRows));
    }
    refreshPadPorts();
}

/* ---- the window ------------------------------------------------------------------------- */

void ControllersWindow::present()
{
    m_padGen = adamsession_gamepad_generation(m_session);
    rebuildPads();
    show();
    raise();
}

void ControllersWindow::refresh()
{
    if (isVisible()) {
        letGo();
        m_console->pollHeld();
    }
}

void ControllersWindow::showEvent(QShowEvent *evt)
{
    m_console->pollHeld();
    m_keys->pollHeld();
    m_heldTimer.start(20);
    m_slowTimer.start(250);
    QWidget::showEvent(evt);
}

void ControllersWindow::hideEvent(QHideEvent *evt)
{
    m_heldTimer.stop();
    m_slowTimer.stop();
    setMapState(-2);
    letGo();
    adamsession_keyboard_release(m_session);
    QWidget::hideEvent(evt);
}

/* Only what this window holds: the main window releases its own keys. */
void ControllersWindow::changeEvent(QEvent *evt)
{
    if (evt->type() == QEvent::ActivationChange && !isActiveWindow()) {
        letGo();
        adamsession_release_all(m_session);
        m_swallow = 0;
    }
    QWidget::changeEvent(evt);
}

void ControllersWindow::keyPressEvent(QKeyEvent *evt)
{
    if (evt->key() == Qt::Key_F9) {
        if (!evt->isAutoRepeat()) hide();
        return;
    }
    if (evt->key() == Qt::Key_F12) {
        if (!evt->isAutoRepeat()) DebuggerWindow::toggleFor(parentWidget(), m_session);
        return;
    }
    if (adamIsAppKey(evt)) return;      /* F10 and F11 belong to the main window */
    const uint32_t keysym = adamKeysymFromQt(evt);
    if (m_swallow && keysym == m_swallow) return;

    if (m_mapState != -2) {
        if (evt->isAutoRepeat()) return;
        /* Esc backs out one step: a pending target, then Map itself */
        if (evt->key() == Qt::Key_Escape) {
            m_swallow = keysym;
            setMapState(m_mapState >= 0 ? -1 : -2);
            return;
        }
        if (m_mapState < 0 || !keysym) return;      /* armed, or nothing to bind: nothing is typed */
        char stolen[128] = "", name[48] = "";
        const int target = m_mapState;
        adamsession_binding_set_key(m_session, target, keysym, stolen, sizeof stolen);
        adamsession_keysym_name(keysym, name, sizeof name);
        m_swallow = keysym;
        bound(target, local(name), stolen);
        return;
    }
    /* Otherwise what the main window does with it: the session's bindings
     * first (a console action goes to the main window's queue, as a gamepad's
     * does), then the ADAM's keyboard. */
    if (!adamForwardKey(m_session, evt, true)) QWidget::keyPressEvent(evt);
}

/* A release always goes to the session, in Map mode too: it only lets go of
 * what a press asserted, so a key held while Map was armed cannot stick. */
void ControllersWindow::keyReleaseEvent(QKeyEvent *evt)
{
    if (evt->isAutoRepeat()) return;
    const uint32_t keysym = adamKeysymFromQt(evt);
    if (m_swallow && keysym == m_swallow) {
        m_swallow = 0;
        return;
    }
    if (!adamForwardKey(m_session, evt, false)) QWidget::keyReleaseEvent(evt);
}

QPointer<ControllersWindow> g_window;

} // namespace

void adamControllersToggle(QWidget *parent, adamsession *session)
{
    if (!g_window) g_window = new ControllersWindow(session, parent);
    if (g_window->isVisible()) g_window->hide();
    else g_window->present();
}

void adamControllersShow(QWidget *parent, adamsession *session)
{
    if (!g_window) g_window = new ControllersWindow(session, parent);
    if (!g_window->isVisible()) g_window->present();
    else g_window->raise();
}

bool adamControllersVisible()
{
    return g_window && g_window->isVisible();
}

void adamControllersRefresh()
{
    if (g_window) g_window->refresh();
}

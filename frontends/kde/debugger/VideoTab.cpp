/*
 * The debugger's Video tab (Qt6 Widgets).
 *
 * Everything that decides what to show lives in the core (adamvidtab.h): this file is widgets.
 * The picture comes from the controller's cached buffer already opaque (0xFFRRGGBB), so
 * Format_RGB32 is safe on KWin; a mouse move redraws only the marks, never re-renders.
 * Only the section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "VideoTab.h"
#include "Accent.h"

extern "C" {
#include "adamvidcap.h"
}

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

constexpr int kPokeMax = 64;

QFont monoFont(QFont base)
{
    base.setFamily(QStringLiteral("monospace"));
    base.setStyleHint(QFont::TypeWriter);
    return base;
}

QLabel *monoLabel()
{
    auto *label = new QLabel;
    label->setFont(monoFont(label->font()));
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    return label;
}

QTableWidget *makeTable(const QStringList &heads)
{
    auto *table = new QTableWidget(0, heads.size());
    table->setHorizontalHeaderLabels(heads);
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setFont(monoFont(table->font()));
    return table;
}

QString hex(unsigned value, int digits)
{
    return QStringLiteral("%1").arg(value, digits, 16, QLatin1Char('0')).toUpper();
}

void setCell(QTableWidget *table, int row, int col, const QString &text, bool hot)
{
    auto *item = table->item(row, col);
    if (!item) {
        item = new QTableWidgetItem;
        table->setItem(row, col, item);
    }
    item->setText(text);
    item->setBackground(hot ? QBrush(adamDebugAccentFill(table)) : QBrush());
    item->setForeground(hot ? QBrush(adamDebugAccentFillInk(table)) : QBrush());
}

} // namespace

/* ---- the picture ------------------------------------------------------------ */

class VideoPicture : public QWidget
{
public:
    explicit VideoPicture(VideoTab *owner) : m_owner(owner)
    {
        setMouseTracking(true);
        setMinimumSize(320, 200);
    }
    /* the controller's buffer, or a message when this view cannot be drawn */
    void sync()
    {
        int width = 0, height = 0;
        const uint32_t *pixels = adamvid_tab_picture(m_owner->m_tab, &width, &height);
        if (pixels) {
            /* a view over the controller's buffer: valid until its next refresh */
            m_image = QImage(reinterpret_cast<const uchar *>(pixels), width, height, width * 4, QImage::Format_RGB32);
            m_msg.clear();
            setFixedSize(width, height);
        } else {
            m_image = QImage();
            const char *why = adamvid_tab_why(m_owner->m_tab);
            m_msg = QString::fromUtf8(why ? why : "Nothing to show");
            setMinimumSize(320, 200);
            setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter paint(this);
        paint.fillRect(rect(), QColor(18, 18, 24));
        if (m_image.isNull()) {
            paint.setPen(QColor(140, 140, 148));
            paint.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, m_msg);
            return;
        }
        paint.drawImage(0, 0, m_image);
        int hover[4] = { 0 }, pin[4] = { 0 };
        const int marks = adamvid_tab_marks(m_owner->m_tab, hover, pin);
        if (marks & 1) {
            paint.setPen(QPen(QColor(255, 255, 255, 230), 1));
            paint.drawRect(hover[0] - 1, hover[1] - 1, hover[2] + 1, hover[3] + 1);   /* just outside the pixel */
        }
        if (marks & 2) {
            paint.setPen(QPen(QColor(255, 217, 26), 2));
            paint.drawRect(pin[0] - 1, pin[1] - 1, pin[2] + 1, pin[3] + 1);
        }
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        const QPoint where = event->position().toPoint();
        if (adamvid_tab_hover(m_owner->m_tab, where.x(), where.y())) {
            update();
            m_owner->updatePixelSection();
        }
    }
    void leaveEvent(QEvent *) override
    {
        if (adamvid_tab_hover(m_owner->m_tab, -1, -1)) {
            update();
            m_owner->updatePixelSection();
        }
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        const QPoint where = event->position().toPoint();
        adamvid_tab_pin(m_owner->m_tab, where.x(), where.y());
        update();
        m_owner->updatePixelSection();
        /* a pin on a VRAM-backed pixel moves the VRAM section's dump to it */
        if (m_owner->m_sections->currentIndex() == ADAMVID_S_VRAM) m_owner->fillVram();
    }
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        const QPoint where = event->position().toPoint();
        adamvid_tab_hover(m_owner->m_tab, where.x(), where.y());
        m_owner->breakOnPixel();
        update();
    }

private:
    VideoTab *m_owner;
    QImage m_image;
    QString m_msg;
};

/* ---- the tab ------------------------------------------------------------------ */

VideoTab::VideoTab(adamdebug *debugger, QWidget *parent) : QWidget(parent), m_dbg(debugger)
{
    m_tab = adamvid_tab_new(debugger);

    auto *root = new QVBoxLayout(this);

    /* ---- toolbar: view, Decoded source, bank, zoom, break; then the overlays */
    auto *bar = new QHBoxLayout;
    m_view = new QComboBox;
    for (int idx = 0; idx < ADAMVID_V_COUNT; ++idx) m_view->addItem(QString::fromUtf8(adamvid_view_name(idx)));
    m_view->setToolTip(QStringLiteral(
        "Display: the last finished frame, as the main window shows it.\n"
        "Decoded: one source (Source) redrawn from the VDP's registers and VRAM.\n"
        "Beam: the Display, the lines the beam has not reached yet dimmed (stopped only).\n"
        "Values: each name table entry as a flat false colour of its value."));
    m_source = new QComboBox;
    for (int idx = 0; idx < ADAMVID_SRC_COUNT; ++idx) m_source->addItem(QString::fromUtf8(adamvid_source_name(idx)));
    m_source->setToolTip(QStringLiteral("What the Decoded view redraws: the screen, the pattern sheet, the 32 sprites or the palette"));
    m_bank = new QComboBox;
    m_bank->addItems({ QStringLiteral("Bank 0"), QStringLiteral("Bank 1"), QStringLiteral("Bank 2") });
    m_bank->setToolTip(QStringLiteral("The Patterns source in Graphics II shows one of three 256-tile banks"));
    m_zoom = new QComboBox;
    int zoomCount = 0;
    const int *zooms = adamvid_zooms(&zoomCount);
    for (int idx = 0; idx < zoomCount; ++idx)
        m_zoom->addItem(zooms[idx] ? QStringLiteral("%1×").arg(zooms[idx]) : QStringLiteral("Fit"), zooms[idx]);
    bar->addWidget(new QLabel(QStringLiteral("View")));
    bar->addWidget(m_view);
    bar->addWidget(new QLabel(QStringLiteral("Source")));
    bar->addWidget(m_source);
    bar->addWidget(m_bank);
    bar->addWidget(new QLabel(QStringLiteral("Zoom")));
    bar->addWidget(m_zoom);
    m_breakBtn = new QPushButton(QStringLiteral("Break on writes"));
    m_breakBtn->setToolTip(QStringLiteral("A breakpoint on the CPU's writes to what the inspected pixel is made from: "
                                          "VRAM through the VDP data port or a VDP register"));
    bar->addWidget(m_breakBtn);
    bar->addStretch(1);
    root->addLayout(bar);

    auto *overlays = new QHBoxLayout;
    overlays->addWidget(new QLabel(QStringLiteral("Overlays")));
    int overlayCount = 0;
    const adamvid_overlay_info *overlayInfo = adamvid_overlays(&overlayCount);
    for (int idx = 0; idx < overlayCount; ++idx) {
        auto *box = new QCheckBox(QString::fromUtf8(overlayInfo[idx].label));
        box->setProperty("bit", overlayInfo[idx].bit);
        m_overlays << box;
        overlays->addWidget(box);
    }
    overlays->addStretch(1);
    root->addLayout(overlays);

    m_status = monoLabel();
    root->addWidget(m_status);

    auto *split = new QSplitter(Qt::Horizontal);
    root->addWidget(split, 1);

    /* ---- left: picture */
    auto *left = new QWidget;
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    m_scroll = new QScrollArea;
    m_pic = new VideoPicture(this);
    m_pic->setObjectName(QStringLiteral("videoPicture"));
    m_scroll->setWidget(m_pic);
    m_scroll->setWidgetResizable(false);
    m_scroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    leftLayout->addWidget(m_scroll, 1);
    m_note = new QLabel;
    leftLayout->addWidget(m_note);
    split->addWidget(left);

    /* ---- right: sections, in adamvid_section_name order */
    m_sections = new QTabWidget;
    {
        auto *page = new QWidget;
        auto *pageLayout = new QVBoxLayout(page);
        m_pixelText = monoLabel();
        m_pixelText->setWordWrap(true);
        pageLayout->addWidget(m_pixelText);
        auto *btns = new QHBoxLayout;
        auto *brk = new QPushButton(QStringLiteral("Break on its writes"));
        auto *unpin = new QPushButton(QStringLiteral("Unpin"));
        btns->addWidget(brk);
        btns->addWidget(unpin);
        btns->addStretch(1);
        pageLayout->addLayout(btns);
        pageLayout->addStretch(1);
        connect(brk, &QPushButton::clicked, this, [this] { breakOnPixel(); });
        connect(unpin, &QPushButton::clicked, this, [this] {
            adamvid_tab_unpin(m_tab);
            m_pic->update();
            updatePixelSection();
        });
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(page);
        m_sections->addTab(scroll, QString::fromUtf8(adamvid_section_name(ADAMVID_S_PIXEL)));
    }
    m_regs = makeTable({ QStringLiteral("Name"), QStringLiteral("Value"), QStringLiteral("Meaning") });
    m_sections->addTab(m_regs, QString::fromUtf8(adamvid_section_name(ADAMVID_S_REGISTERS)));
    m_swatches = makeTable({ QString(), QStringLiteral("Colour"), QStringLiteral("RGB"), QStringLiteral("Used"), QStringLiteral("Role") });
    m_swatches->verticalHeader()->setDefaultSectionSize(26);
    m_sections->addTab(m_swatches, QString::fromUtf8(adamvid_section_name(ADAMVID_S_COLOURS)));
    m_lines = makeTable({ QStringLiteral("Lines"), QStringLiteral("Frame line"), QStringLiteral("Mode"), QStringLiteral("Names"),
                          QStringLiteral("Patterns"), QStringLiteral("Colours"), QStringLiteral("Spr attr"), QStringLiteral("Spr pat"),
                          QStringLiteral("Backdrop"), QStringLiteral("Writes") });
    m_sections->addTab(m_lines, QString::fromUtf8(adamvid_section_name(ADAMVID_S_LINES)));
    {
        auto *page = new QWidget;
        auto *pageLayout = new QVBoxLayout(page);
        m_beam = new QProgressBar;
        m_beam->setRange(0, 1000);
        m_beam->setFormat(QStringLiteral("beam"));
        pageLayout->addWidget(m_beam);
        m_timingText = monoLabel();
        m_timingText->setWordWrap(true);
        pageLayout->addWidget(m_timingText);
        pageLayout->addStretch(1);
        m_sections->addTab(page, QString::fromUtf8(adamvid_section_name(ADAMVID_S_TIMING)));
    }
    {
        /* the VRAM hex view with its poke bar: "addr [bytes...]" scrolls, or writes */
        auto *page = new QWidget;
        auto *pageLayout = new QVBoxLayout(page);
        auto *row = new QHBoxLayout;
        m_vramEntry = new QLineEdit;
        m_vramEntry->setPlaceholderText(QStringLiteral("addr [bytes...] e.g. 1800: 41 42 43"));
        m_vramEntry->setToolTip(QStringLiteral("An address scrolls the dump; with hex bytes after it, Enter writes them (VRAM poke)"));
        auto *prev = new QPushButton(QStringLiteral("◀"));
        auto *next = new QPushButton(QStringLiteral("▶"));
        prev->setFixedWidth(40);
        next->setFixedWidth(40);
        m_vramStatus = new QLabel;
        m_vramStatus->hide();      /* shown with the first message */
        row->addWidget(m_vramEntry, 1);
        row->addWidget(prev);
        row->addWidget(next);
        pageLayout->addLayout(row);
        pageLayout->addWidget(m_vramStatus);
        m_vramView = new QPlainTextEdit;
        m_vramView->setReadOnly(true);
        m_vramView->setFont(monoFont(m_vramView->font()));
        m_vramView->setLineWrapMode(QPlainTextEdit::NoWrap);
        pageLayout->addWidget(m_vramView, 1);
        connect(m_vramEntry, &QLineEdit::returnPressed, this, [this] { vramSubmit(); });
        connect(prev, &QPushButton::clicked, this, [this] { vramPage(-1); });
        connect(next, &QPushButton::clicked, this, [this] { vramPage(1); });
        m_sections->addTab(page, QString::fromUtf8(adamvid_section_name(ADAMVID_S_VRAM)));
    }
    split->addWidget(m_sections);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    m_sections->setMinimumWidth(480);
    split->setSizes({ 640, 640 });

    /* start from the controller's state (the ADAM_DEBUGGER_VIDEO_* variables) */
    m_updating = true;
    m_view->setCurrentIndex(adamvid_tab_view(m_tab));
    m_source->setCurrentIndex(adamvid_tab_source(m_tab));
    m_bank->setCurrentIndex(qBound(0, adamvid_tab_bank(m_tab), 2));
    for (int idx = 0; idx < zoomCount; ++idx)
        if (zooms[idx] == adamvid_tab_zoom(m_tab)) m_zoom->setCurrentIndex(idx);
    for (QCheckBox *box : m_overlays) box->setChecked((adamvid_tab_overlays(m_tab) & box->property("bit").toUInt()) != 0);
    m_sections->setCurrentIndex(adamvid_tab_env_section(m_tab));
    updateSourceControls();
    updateBreakEnabled();
    m_updating = false;

    connect(m_view, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (m_updating || idx < 0) return;
        adamvid_tab_set_view(m_tab, idx);
        updateSourceControls();
        refresh(true);
    });
    connect(m_source, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (m_updating || idx < 0) return;
        adamvid_tab_set_source(m_tab, idx);
        updateSourceControls();
        refresh(true);
    });
    connect(m_bank, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (m_updating || idx < 0) return;
        adamvid_tab_set_bank(m_tab, idx);
        refresh(true);
    });
    connect(m_zoom, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (m_updating || idx < 0) return;
        adamvid_tab_set_zoom(m_tab, m_zoom->itemData(idx).toInt());
        refresh(true);
    });
    for (QCheckBox *box : m_overlays)
        connect(box, &QCheckBox::toggled, this, [this](bool) {
            if (m_updating) return;
            unsigned mask = 0;
            for (QCheckBox *each : m_overlays)
                if (each->isChecked()) mask |= each->property("bit").toUInt();
            adamvid_tab_set_overlays(m_tab, mask);
            refresh(true);
        });
    connect(m_breakBtn, &QPushButton::clicked, this, [this] { breakOnPixel(); });
    connect(m_sections, &QTabWidget::currentChanged, this, [this](int) {
        m_regsSig.clear();
        m_linesSig.clear();
        refreshView(ADAMVID_TAB_TABLES);
    });
}

VideoTab::~VideoTab()
{
    adamvid_tab_free(m_tab);
}

/* The source is the Decoded view's; the bank the Patterns source's. */
void VideoTab::updateSourceControls()
{
    const bool decoded = adamvid_tab_view(m_tab) == ADAMVID_V_DECODED;
    m_source->setEnabled(decoded);
    m_bank->setEnabled(decoded && adamvid_tab_source(m_tab) == ADAMVID_SRC_PATTERNS);
}

/* Break on writes needs VRAM or a register under the inspected pixel. */
void VideoTab::updateBreakEnabled()
{
    m_breakBtn->setEnabled(adamvid_tab_can_break(m_tab) != 0);
}

void VideoTab::note(const QString &text)
{
    m_note->setText(text);
}

void VideoTab::breakOnPixel()
{
    const adamvid_pixel *pixel = adamvid_tab_pixel(m_tab);
    const int bpid = adamvid_tab_break_on_pixel(m_tab);
    if (bpid >= 0 && pixel && pixel->nbreak > 0) {
        switch (pixel->break_kind[0]) {
        case ADAMVID_BK_REGISTER: note(QStringLiteral("Break on writes to VDP register %1 (see Breakpoints)").arg(pixel->break_lo[0])); break;
        default:
            note(QStringLiteral("Break on VRAM writes to $%1-$%2 (see Breakpoints)")
                     .arg(hex(pixel->break_lo[0], 4), hex(pixel->break_hi[0], 4)));
            break;
        }
    } else if (bpid >= 0) {
        note(QStringLiteral("Write breakpoint added (see Breakpoints)"));
    } else {
        note(QStringLiteral("Nothing to break on: hover or pin a tile, a sprite, a pattern or a colour"));
    }
    updatePixelSection();
}

void VideoTab::updatePixelSection()
{
    const adamvid_pixel *pixel = adamvid_tab_pixel(m_tab);
    updateBreakEnabled();
    m_pixelText->setText(pixel ? QString::fromUtf8(pixel->text)
                               : QStringLiteral("Move over the picture to inspect a pixel.\nClick to pin it; "
                                                "double-click to break on writes to it."));
}

void VideoTab::fillRegs()
{
    const adamvid_reg *regs;
    const int count = adamvid_tab_regs(m_tab, &regs);
    QString sig;
    for (int idx = 0; idx < count; ++idx)
        sig += QString::fromUtf8(regs[idx].value) + (regs[idx].age < ADAMVID_REG_FADE ? QLatin1Char('*') : QLatin1Char('.')) +
               QString::number(regs[idx].writes);
    sig += QString::number(count);
    if (sig == m_regsSig) return;
    m_regsSig = sig;
    const int vpos = m_regs->verticalScrollBar()->value();
    m_regs->setRowCount(count);
    for (int idx = 0; idx < count; ++idx) {
        const bool hot = regs[idx].age < ADAMVID_REG_FADE || regs[idx].writes > 0;
        QString meaning = QString::fromUtf8(regs[idx].meaning);
        if (regs[idx].writes)
            meaning += QStringLiteral("  [%1 write%2, first at line %3]")
                           .arg(regs[idx].writes).arg(regs[idx].writes == 1 ? QString() : QStringLiteral("s")).arg(regs[idx].first_line);
        setCell(m_regs, idx, 0, QString::fromUtf8(regs[idx].name), hot);
        setCell(m_regs, idx, 1, QString::fromUtf8(regs[idx].value), hot);
        setCell(m_regs, idx, 2, meaning, hot);
    }
    m_regs->resizeColumnsToContents();
    m_regs->verticalScrollBar()->setValue(vpos);
}

/* The 16 colours, each with a swatch and what the registers use it for; the
 * one the inspected pixel has is marked. */
void VideoTab::fillColours()
{
    const adamvid_swatch *swatches;
    const int count = adamvid_tab_swatches(m_tab, &swatches);
    m_swatches->setRowCount(count);
    for (int idx = 0; idx < count; ++idx) {
        const bool inspected = swatches[idx].inspected != 0;
        auto *colour = m_swatches->item(idx, 0);
        if (!colour) {
            colour = new QTableWidgetItem;
            m_swatches->setItem(idx, 0, colour);
        }
        colour->setBackground(QBrush(QColor::fromRgb(swatches[idx].rgb & 0xFFFFFF)));
        setCell(m_swatches, idx, 1, QString::fromUtf8(swatches[idx].name), inspected);
        setCell(m_swatches, idx, 2, QStringLiteral("#") + hex(swatches[idx].rgb & 0xFFFFFF, 6), inspected);
        setCell(m_swatches, idx, 3, QStringLiteral("%1 px").arg(swatches[idx].uses), inspected);
        setCell(m_swatches, idx, 4,
                QString::fromUtf8(swatches[idx].role), inspected);
    }
    m_swatches->resizeColumnsToContents();
    m_swatches->setColumnWidth(0, 28);
}

/* One row for each band of display lines drawn from the same registers: a program that never changes them in the frame has one. */
void VideoTab::fillLines()
{
    const adamvid_band *bands;
    const int count = adamvid_tab_lines(m_tab, &bands);
    QString sig = QString::number(count);
    for (int idx = 0; idx < count; ++idx) sig += QString::fromUtf8(bands[idx].text);
    if (sig == m_linesSig) return;
    m_linesSig = sig;
    const int vpos = m_lines->verticalScrollBar()->value();
    m_lines->setRowCount(count);
    for (int idx = 0; idx < count; ++idx) {
        const adamvid_band &band = bands[idx];
        const bool hot = band.writes > 0;
        setCell(m_lines, idx, 0, QStringLiteral("%1-%2").arg(band.first).arg(band.last), hot);
        setCell(m_lines, idx, 1, QString::number(band.abs_first), hot);
        setCell(m_lines, idx, 2, QString::fromUtf8(band.mode), hot);
        setCell(m_lines, idx, 3, QStringLiteral("$") + hex(band.name_table, 4), hot);
        setCell(m_lines, idx, 4, QStringLiteral("$") + hex(band.pattern_table, 4), hot);
        setCell(m_lines, idx, 5, QStringLiteral("$") + hex(band.colour_table, 4), hot);
        setCell(m_lines, idx, 6, QStringLiteral("$") + hex(band.sprite_attr, 4), hot);
        setCell(m_lines, idx, 7, QStringLiteral("$") + hex(band.sprite_pattern, 4), hot);
        setCell(m_lines, idx, 8, QString::number(band.backdrop), hot);
        setCell(m_lines, idx, 9, QString::number(band.writes), hot);
    }
    m_lines->resizeColumnsToContents();
    m_lines->verticalScrollBar()->setValue(vpos);
}

void VideoTab::fillTiming()
{
    const char *const *lines;
    const int count = adamvid_tab_timing(m_tab, &lines);
    QString text;
    for (int idx = 0; idx < count; ++idx) {
        const QString line = QString::fromUtf8(lines[idx]);
        const int tabPos = line.indexOf(QLatin1Char('\t'));
        text += tabPos >= 0 ? line.left(tabPos).leftJustified(14) + QLatin1Char(' ') + line.mid(tabPos + 1) : line;
        text += QLatin1Char('\n');
    }
    m_timingText->setText(text.isEmpty() ? QStringLiteral("No timing yet") : text);
    m_beam->setValue(adamvid_tab_beam_permille(m_tab));
}

/* The VRAM dump from the controller's base (an address typed here, or a pin). */
void VideoTab::fillVram()
{
    char text[4096];
    adamvid_tab_text(m_tab, ADAMVID_S_VRAM, text, int(sizeof text));
    const QString shown = QString::fromLatin1(text);
    if (m_vramView->toPlainText() != shown) {
        const int vpos = m_vramView->verticalScrollBar()->value();
        m_vramView->setPlainText(shown);
        m_vramView->verticalScrollBar()->setValue(vpos);
    }
}

/* One entry does both jobs: an address alone scrolls the dump there, an
 * address followed by hex bytes writes them first ("1800: 41 42 43"). */
void VideoTab::vramSubmit()
{
    char err[160] = "";
    const QByteArray line = m_vramEntry->text().toUtf8();
    uint8_t bytes[kPokeMax];
    uint16_t addr = 0;
    const int count = adamvdp_parse_poke(line.constData(), &addr, bytes, kPokeMax);
    if (count < 0) {
        setVramNote(QStringLiteral("expected: addr [bytes...], e.g. 1800: 41 42"));
        return;
    }
    if (adamvid_tab_poke(m_tab, line.constData(), err, int(sizeof err)) < 0) {
        setVramNote(err[0] ? QString::fromUtf8(err) : QStringLiteral("The VRAM could not be written"));
        return;
    }
    if (count > 0) {
        setVramNote(QStringLiteral("wrote %1 byte%2 at $%3").arg(count).arg(count == 1 ? "" : "s").arg(hex(addr, 4)));
        m_vramEntry->clear();
    } else {
        setVramNote(QStringLiteral("$%1").arg(hex(addr, 4)));
    }
    refresh(true);
}

void VideoTab::setVramNote(const QString &text)
{
    m_vramStatus->setText(text);
    m_vramStatus->setVisible(!text.isEmpty());
}

void VideoTab::vramPage(int delta)
{
    const qint64 base = qint64(adamvid_tab_vram_base(m_tab)) + qint64(delta) * 16 * 16;
    adamvid_tab_set_vram_base(m_tab, uint32_t(qBound<qint64>(0, base, qint64(0x4000) - 256)));
    fillVram();
}

void VideoTab::refreshView(unsigned what)
{
    if (what & ADAMVID_TAB_PICTURE) m_pic->sync();
    if (what & ADAMVID_TAB_TABLES) {
        m_status->setText(QString::fromUtf8(adamvid_tab_status(m_tab)));
        switch (m_sections->currentIndex()) {
        case ADAMVID_S_REGISTERS: fillRegs(); break;
        case ADAMVID_S_COLOURS: fillColours(); break;
        case ADAMVID_S_LINES: fillLines(); break;
        case ADAMVID_S_TIMING: fillTiming(); break;
        case ADAMVID_S_VRAM: fillVram(); break;
        default: break;
        }
        updatePixelSection();
    }
}

void VideoTab::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    /* Fit follows the room the picture has */
    if (adamvid_tab_zoom(m_tab) == 0) refresh(false);
}

void VideoTab::refresh(bool force)
{
    const QSize viewport = m_scroll->viewport()->size();
    if (viewport.width() > 4 && viewport.height() > 4)
        adamvid_tab_set_viewport(m_tab, viewport.width() - 2, viewport.height() - 2);
    unsigned what = adamvid_tab_refresh(m_tab, force);
    if (force) what |= ADAMVID_TAB_PICTURE | ADAMVID_TAB_TABLES;
    if (what & ADAMVID_TAB_OFF) what |= ADAMVID_TAB_PICTURE | ADAMVID_TAB_TABLES;
    refreshView(what);
}

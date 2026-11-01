/*
 * The emulator display on the GPU: a QRhiWidget that keeps one texture the
 * size of the frame and draws one textured quad, only when the session
 * has published a new frame (see FrameFeed). Qt picks Vulkan, Metal, D3D or
 * OpenGL for it; the scaling, aspect and filtering all happen on the GPU.
 *
 * If no GPU device can be had it emits renderFailed() and the main window
 * swaps in the software SoftDisplayWidget.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QFocusEvent>
#include <QImage>
#include <QKeyEvent>
#include <QRhiWidget>
#include <cstdint>
#include <memory>
#include <vector>
#include <rhi/qrhi.h>

#include "DisplayView.h"
#include "FrameFeed.h"
#include "adamsession.h"

class DisplayWidget : public QRhiWidget, public DisplayView {
    Q_OBJECT
public:
    explicit DisplayWidget(adamsession *session, QWidget *parent = nullptr);
    ~DisplayWidget() override;

    QWidget *widget() override { return this; }
    void setAspectMode(int mode) override;
    void setSmooth(bool smooth) override;

protected:
    void initialize(QRhiCommandBuffer *cb) override;
    void render(QRhiCommandBuffer *cb) override;
    void releaseResources() override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool event(QEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *e) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    /* Tab belongs to the ADAM keyboard (and to Map mode), not to focus
     * traversal. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    void onFrame();
    void updateActive();

    adamsession *m_session;
    FrameFeed *m_feed;
    std::vector<uint16_t> m_fb;   /* RGB565 straight from the session */
    QImage m_scratch;          /* FB_WIDTH x FB_HEIGHT, converted to RGB32 */
    uint64_t m_serial = 0;
    bool m_dirty = false;      /* m_scratch holds a frame not yet uploaded */
    int m_aspectMode = AspectSquarePixels;
    bool m_smooth = false;
    bool m_smoothApplied = false;

    QRhi *m_rhi = nullptr;
    std::unique_ptr<QRhiBuffer> m_ubuf;
    std::unique_ptr<QRhiTexture> m_tex;
    std::unique_ptr<QRhiSampler> m_nearest, m_linear;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
};

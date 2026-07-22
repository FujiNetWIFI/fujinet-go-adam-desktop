/*
 * FujiNet configuration (embedded web UI) and console log windows.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QWidget>

#include "adamsession.h"

/* Web UI window (QtWebEngine when built with it, otherwise the system
 * browser) and the FujiNet console log window. */
void fujinet_config_show(QWidget *parent, adamsession *session);
void fujinet_log_show(QWidget *parent, adamsession *session);

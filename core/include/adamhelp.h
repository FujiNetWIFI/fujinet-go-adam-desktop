/*
 * adamhelp -- the app's Help: data/help/help.md, built in by
 * tools/help/make-help.py as topics of blocks of styled spans, which every
 * frontend lays out with its own toolkit.
 *
 * A frontend shows the blocks whose platforms include its own bit. Ids are
 * GitHub's heading slugs ("controllers-and-keys"); a Help button names a
 * context instead ("prefs/Machine", "debugger/cpu"), which stays put when a
 * heading is renamed. Everything here is constant data and reentrant.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ADAMHELP_H
#define ADAMHELP_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ADAMHELP_GNOME = 1, ADAMHELP_KDE = 2, ADAMHELP_WINDOWS = 4, ADAMHELP_MACOS = 8
};

typedef enum {
    ADAMHELP_H2 = 0, ADAMHELP_H3,
    ADAMHELP_PARA,
    ADAMHELP_BULLET, ADAMHELP_BULLET2,    /* "- " and "  - " */
    ADAMHELP_NUMBERED,                     /* a step; number is its own */
    ADAMHELP_NOTE,
    ADAMHELP_PRE,                          /* one TEXT span, lines as they are */
    ADAMHELP_TABLE_HEAD,                   /* starts a table; its rows follow */
    ADAMHELP_TABLE_ROW
} adamhelp_block_kind;

typedef enum {
    ADAMHELP_TEXT = 0, ADAMHELP_BOLD, ADAMHELP_ITALIC, ADAMHELP_CODE,
    ADAMHELP_KBD,                          /* keys; mac_text is the Mac's */
    ADAMHELP_LINK,                         /* target "#slug" or a URL */
    ADAMHELP_CELL                          /* tables: the next column starts */
} adamhelp_span_kind;

typedef struct {
    int kind;
    const char *text;                       /* UTF-8 */
    const char *mac_text;                   /* KBD on macOS, or NULL: text */
    const char *target;
} adamhelp_span;

typedef struct {
    int kind;
    unsigned platforms;                     /* ADAMHELP_* bits */
    int number;
    const char *id;                         /* headings: the slug */
    const char *context;                    /* headings: or NULL */
    const adamhelp_span *spans;            /* headings: one TEXT span */
    int nspans;
    /* Tables: the width of each column but the last, in ems, for frontends
     * that set tab stops (the last column wraps under itself). */
    const unsigned char *col_em;
    int ncol_em;
} adamhelp_block;

typedef struct {
    const char *id;
    const char *title;
    const char *context;                    /* or NULL */
    const adamhelp_block *blocks;
    int nblocks;
} adamhelp_topic;

int adamhelp_topic_count(void);
const adamhelp_topic *adamhelp_topic_at(int t);

/* "slug", "#slug" or a context: its topic, and the heading's block (-1 for
 * the topic itself). 0, or -1 if there is none. */
int adamhelp_find(const char *id, int *topic, int *block);

/* The span's text as platform shows it. */
const char *adamhelp_span_text(const adamhelp_span *sp, unsigned platform);

/* A heading block's text. */
const char *adamhelp_heading(const adamhelp_block *b);

/* The block after the section headed by block (the next heading at its
 * level or above, or the topic's end); -1: the whole topic. */
int adamhelp_section_end(int topic, int block);

/* The section headed by block (-1: from the topic's title) as text on
 * platform: a line per block, cells separated by " | ". Returns its length
 * (truncated to dstsz - 1). */
int adamhelp_plain_text(unsigned platform, int topic, int block, char *dst, int dstsz);

/* Sections that have every word of query (case-insensitive), best first:
 * where it is in a title, then how often. block -1 is a topic's start, up
 * to its first heading. Returns how many were written. */
typedef struct {
    int topic, block, score;
} adamhelp_hit;
int adamhelp_search(unsigned platform, const char *query, adamhelp_hit *out, int max);

/* The first match of a word of query in text at byte from or after: where
 * it starts and its length, for highlighting. 1, or 0 if there is none. */
int adamhelp_match(const char *text, const char *query, int from, int *start, int *len);

#ifdef __cplusplus
}
#endif

#endif /* ADAMHELP_H */

/*
 * browser.h - page loading, rendering and navigation API
 */
#pragma once
#include <stdarg.h>
#include <stdbool.h>
#include <citro2d.h>

#define BROWSER_MAX_HISTORY   64
#define BROWSER_MAX_BOOKMARKS 32
#define BROWSER_MAX_LINKS     128
#define BROWSER_URL_LEN       512
#define BROWSER_TITLE_LEN     128
#define BROWSER_MAX_TEXT      (256 * 1024)

/* ── Data types ─────────────────────────────────────────────────────────── */

typedef struct {
    char title[BROWSER_TITLE_LEN];
    char url[BROWSER_URL_LEN];
} Bookmark;

typedef struct {
    char  text[128];   /* visible link label                                 */
    char  href[BROWSER_URL_LEN];
    int   line;        /* which rendered line this link is on                */
} PageLink;

typedef enum {
    BLOAD_IDLE,
    BLOAD_PENDING,
    BLOAD_DONE,
    BLOAD_ERROR,
} BrowserLoadStatus;

typedef struct {
    /* ── Current page ──────────────────────────────────────────────── */
    char  current_url[BROWSER_URL_LEN];
    char  page_title[BROWSER_TITLE_LEN];
    char *page_text;                    /* stripped body text, heap          */
    char *raw_html;     /* full page source for JS script extraction */
    int   rendered_height;              /* total pixel height of page text   */
    int   scroll_y;

    /* ── Load state ────────────────────────────────────────────────── */
    BrowserLoadStatus load_status;
    float             load_progress;    /* 0.0 – 1.0                        */
    char              error_msg[256];

    /* ── Links on current page ──────────────────────────────────────── */
    PageLink links[BROWSER_MAX_LINKS];
    int      link_count;
    int      selected_link;

    /* ── History ────────────────────────────────────────────────────── */
    char history[BROWSER_MAX_HISTORY][BROWSER_URL_LEN];
    int  history_count;
    int  history_pos;   /* index of currently displayed page                */

    /* ── Bookmarks ──────────────────────────────────────────────────── */
    Bookmark bookmarks[BROWSER_MAX_BOOKMARKS];
    int      bookmark_count;

    /* ── Stats ──────────────────────────────────────────────────────── */
    int js_exec_count;

    /* Debug log ring buffer */
#define BROWSER_DEBUG_LINES   16
#define BROWSER_DEBUG_LINE_LEN 80
    char debug_log[BROWSER_DEBUG_LINES][BROWSER_DEBUG_LINE_LEN];
    int  debug_count;

    /* Raw HTTP response preview (first 200 bytes) */
    char   raw_preview[200];
    int    raw_status;
    size_t raw_bytes;
} BrowserCtx;

/* ── Lifecycle ──────────────────────────────────────────────────────────── */
void browser_init(BrowserCtx *b);
void browser_exit(BrowserCtx *b);

/* ── Navigation ─────────────────────────────────────────────────────────── */
void              browser_navigate(BrowserCtx *b, const char *url);
void              browser_cancel(BrowserCtx *b);
BrowserLoadStatus browser_poll(BrowserCtx *b);
bool              browser_back(BrowserCtx *b);    /* false = no history      */
void              browser_forward(BrowserCtx *b);
void              browser_reload(BrowserCtx *b);

/* ── Scrolling ──────────────────────────────────────────────────────────── */
void browser_scroll(BrowserCtx *b, int delta);
int  browser_max_scroll(BrowserCtx *b, int viewport_h);

/* ── Links ──────────────────────────────────────────────────────────────── */
int         browser_link_count(BrowserCtx *b);
const char *browser_link_text(BrowserCtx *b, int idx);
const char *browser_selected_href(BrowserCtx *b);

/* ── Bookmarks ──────────────────────────────────────────────────────────── */
void browser_add_bookmark(BrowserCtx *b, const char *title, const char *url);
void browser_delete_bookmark(BrowserCtx *b, int idx);

/* ── History ────────────────────────────────────────────────────────────── */
void browser_clear_history(BrowserCtx *b);

/* Write a debug message into the ring-buffer log. */
void browser_debug(BrowserCtx *b, const char *fmt, ...);

/* ── Rendering (citro2d) ────────────────────────────────────────────────── */
void browser_draw(BrowserCtx *b, C2D_Font font, C2D_TextBuf buf,
                  float x, float y, float w, float h,
                  int scroll_y, float scale, float line_h);

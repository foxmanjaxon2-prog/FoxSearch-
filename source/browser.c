#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdarg.h>
#include "browser.h"
#include "net.h"

/* =========================================================================
 * HTML parser — extracts title, plain body text, and anchor links
 * ========================================================================= */

typedef struct {
    char  title[BROWSER_TITLE_LEN];
    char *text;          /* heap-allocated stripped body text                */
    size_t text_len;

    PageLink *links;     /* heap-allocated array of BROWSER_MAX_LINKS        */
    int       link_count;
} ParseResult;

/* Decode common HTML entities in-place. Returns new length. */
static size_t decode_entities(char *s, size_t len)
{
    char *out = s;
    const char *p = s;
    const char *end = s + len;
    while (p < end) {
        if (*p == '&') {
            const char *semi = memchr(p, ';', (size_t)(end - p) < 12 ? (size_t)(end - p) : 12);
            if (semi) {
                size_t elen = (size_t)(semi - p + 1);
                if      (elen == 4  && !memcmp(p, "&lt;",  4)) { *out++ = '<'; p += 4; continue; }
                else if (elen == 4  && !memcmp(p, "&gt;",  4)) { *out++ = '>'; p += 4; continue; }
                else if (elen == 5  && !memcmp(p, "&amp;", 5)) { *out++ = '&'; p += 5; continue; }
                else if (elen == 6  && !memcmp(p, "&nbsp;",6)) { *out++ = ' '; p += 6; continue; }
                else if (elen == 6  && !memcmp(p, "&quot;",6)) { *out++ = '"'; p += 6; continue; }
                else if (elen == 5  && !memcmp(p, "&apos;",6)) { *out++ = '\'';p += 6; continue; }
            }
        }
        *out++ = *p++;
    }
    *out = '\0';
    return (size_t)(out - s);
}

static void html_parse(const char *html, size_t html_len, ParseResult *pr)
{
    memset(pr, 0, sizeof(*pr));
    pr->text = (char *)malloc(html_len + 1);
    if (!pr->text) return;

    pr->links = (PageLink *)calloc(BROWSER_MAX_LINKS, sizeof(PageLink));
    if (!pr->links) { free(pr->text); pr->text = NULL; return; }

    char *anchor_href = (char *)calloc(1, BROWSER_URL_LEN);
    if (!anchor_href) { free(pr->links); free(pr->text); pr->links = NULL; pr->text = NULL; return; }

    char *tag_buf = (char *)malloc(512);
    if (!tag_buf) { free(anchor_href); free(pr->links); free(pr->text); return; }

    size_t out_len = 0;
    bool   in_tag  = false;
    int    tag_len = 0;
    bool   in_anchor = false;
    int    anchor_text_start = 0;

    const char *p   = html;
    const char *end = html + html_len;

    while (p < end) {
        size_t remaining = (size_t)(end - p);

        /* ── Skip <script>…</script> entirely ──────────────────────── */
        if (remaining >= 7 && (p[0]=='<' && (p[1]=='s'||p[1]=='S') && (p[2]=='c'||p[2]=='C') && (p[3]=='r'||p[3]=='R') && (p[4]=='i'||p[4]=='I') && (p[5]=='p'||p[5]=='P') && (p[6]=='t'||p[6]=='T'))) {
            const char *close = strstr(p + 7, "</script>");
            p = close ? close + 9 : end;
            continue;
        }
        /* ── Skip <style>…</style> entirely ────────────────────────── */
        if (remaining >= 6 && (p[0]=='<' && (p[1]=='s'||p[1]=='S') && (p[2]=='t'||p[2]=='T') && (p[3]=='y'||p[3]=='Y') && (p[4]=='l'||p[4]=='L') && (p[5]=='e'||p[5]=='E'))) {
            const char *close = strstr(p + 6, "</style>");
            p = close ? close + 8 : end;
            continue;
        }
        /* ── Skip <noscript>…</noscript> (we run JS via QuickJS) ───── */
        if (remaining >= 9 && (p[0]=='<' && (p[1]=='n'||p[1]=='N') && (p[2]=='o'||p[2]=='O') && (p[3]=='s'||p[3]=='S') && (p[4]=='c'||p[4]=='C') && (p[5]=='r'||p[5]=='R') && (p[6]=='i'||p[6]=='I') && (p[7]=='p'||p[7]=='P') && (p[8]=='t'||p[8]=='T'))) {
            const char *close = strstr(p + 9, "</noscript>");
            p = close ? close + 11 : end;
            continue;
        }
        /* ── Grab <title> from <head>, skip rest of head ────────────── */
        if (remaining >= 5 && (p[0]=='<' && (p[1]=='h'||p[1]=='H') && (p[2]=='e'||p[2]=='E') && (p[3]=='a'||p[3]=='A') && (p[4]=='d'||p[4]=='D'))) {
            const char *head_end = strstr(p + 5, "</head>");
            if (!head_end) head_end = end;
            const char *ts = strstr(p, "<title>");
            if (ts && ts < head_end) {
                ts += 7;
                const char *te = strstr(ts, "</title>");
                if (te && te < head_end) {
                    size_t tl = (size_t)(te - ts);
                    if (tl >= BROWSER_TITLE_LEN) tl = BROWSER_TITLE_LEN - 1;
                    memcpy(pr->title, ts, tl);
                    pr->title[tl] = '\0';
                }
            }
            p = (head_end < end) ? head_end + 7 : end;
            continue;
        }

        char c = *p;

        if (in_tag) {
            if (c == '>') {
                tag_buf[tag_len] = '\0';
                in_tag = false;

                const char *t = tag_buf;
                bool is_close = (*t == '/');
                if (is_close) t++;
                while (*t == ' ') t++;
                char tname[32] = {0};
                for (int k = 0; k < 31 && *t && *t != ' ' && *t != '/'; k++, t++)
                    tname[k] = (char)tolower((unsigned char)*t);

                if (!strcmp(tname, "a")) {
                    if (!is_close && pr->link_count < BROWSER_MAX_LINKS) {
                        memset(anchor_href, 0, BROWSER_URL_LEN);
                        const char *hp = strstr(tag_buf, "href");
                        if (hp) {
                            hp += 4;
                            while (*hp == ' ' || *hp == '=') hp++;
                            char q = (*hp == '"' || *hp == '\'') ? *hp++ : 0;
                            int hi = 0;
                            while (*hp && hi < BROWSER_URL_LEN - 1) {
                                if (q && *hp == q) break;
                                if (!q && (*hp == ' ' || *hp == '>')) break;
                                anchor_href[hi++] = *hp++;
                            }
                            anchor_href[hi] = '\0';
                        }
                        in_anchor = (anchor_href[0] != '\0');
                        if (in_anchor) {
                            /* Ensure link starts on its own logical segment */
                            if (out_len > 0 && pr->text[out_len-1] != '\n')
                                pr->text[out_len++] = '\n';
                            pr->text[out_len++] = '\x04'; /* link marker */
                        }
                        anchor_text_start = (int)out_len;
                    } else if (is_close && in_anchor) {
                        /* Cap link text to 80 chars to avoid huge link lines */
                        int raw_len = (int)out_len - anchor_text_start;
                        /* Strip any marker bytes from beginning of link text */
                        int text_start = anchor_text_start;
                        while (text_start < (int)out_len &&
                               (unsigned char)pr->text[text_start] < 0x20)
                            text_start++;
                        int tl2 = (int)out_len - text_start;
                        PageLink *lk = &pr->links[pr->link_count];
                        if (tl2 > 0 && tl2 < 128) {
                            memcpy(lk->text, pr->text + text_start, tl2);
                            lk->text[tl2] = '\0';
                        } else {
                            snprintf(lk->text, 128, "%s", anchor_href);
                        }
                        strncpy(lk->href, anchor_href, BROWSER_URL_LEN - 1);
                        /* Truncate link text in output to avoid huge link lines */
                        if (raw_len > 80) out_len = anchor_text_start + 80;
                        pr->link_count++;
                        in_anchor = false;
                        /* Emit newline after link so next text starts fresh */
                        pr->text[out_len++] = '\n';
                        memset(anchor_href, 0, BROWSER_URL_LEN);
                    }
                } else {
                    /* Heading open tags: emit style marker byte + newline  */
                    if (!is_close) {
                        char hmark = 0;
                        if      (!strcmp(tname,"h1")) hmark = '\x01';
                        else if (!strcmp(tname,"h2")) hmark = '\x02';
                        else if (!strcmp(tname,"h3") || !strcmp(tname,"h4")) hmark = '\x03';
                        if (hmark) {
                            if (out_len > 0 && pr->text[out_len-1] != '\n')
                                pr->text[out_len++] = '\n';
                            pr->text[out_len++] = hmark;
                        }
                    }
                    /* Heading close / block-level → newline */
                    if (!strcmp(tname,"h1")||!strcmp(tname,"h2")||
                        !strcmp(tname,"h3")||!strcmp(tname,"h4")||
                        !strcmp(tname,"h5")||!strcmp(tname,"h6")||
                        !strcmp(tname,"br")||!strcmp(tname,"p")  ||
                        !strcmp(tname,"div")||!strcmp(tname,"li")||
                        !strcmp(tname,"tr")||!strcmp(tname,"dt")||
                        !strcmp(tname,"dd")||!strcmp(tname,"article")||
                        !strcmp(tname,"section")||!strcmp(tname,"header")||
                        !strcmp(tname,"footer")||!strcmp(tname,"nav")||
                        !strcmp(tname,"main")) {
                        if (out_len > 0 && pr->text[out_len-1] != '\n')
                            pr->text[out_len++] = '\n';
                    }
                }
                tag_len = 0;
            } else {
                if (tag_len < 511) tag_buf[tag_len++] = c;
            }
        } else if (c == '<') {
            in_tag  = true;
            tag_len = 0;
        } else {
            pr->text[out_len++] = c;
        }
        p++;
    }

    free(tag_buf);
    free(anchor_href);

    pr->text[out_len] = '\0';
    pr->text_len = decode_entities(pr->text, out_len);

    /* Collapse whitespace */
    size_t wr = 0;
    bool   last_nl = true;
    for (size_t i = 0; i < pr->text_len; i++) {
        char ch = pr->text[i];
        if (ch == '\r') continue;
        if (ch == ' ' || ch == '\t') {
            if (!last_nl) { pr->text[wr++] = ' '; }
        } else if (ch == '\n') {
            if (!last_nl) { pr->text[wr++] = '\n'; last_nl = true; }
        } else if ((unsigned char)ch < 0x20) {
            /* Style marker byte: preserve but do not reset last_nl.
             * Heading markers after newline must not eat the following newline. */
            pr->text[wr++] = ch;
            /* last_nl unchanged — keeps blank-line suppression working */
        } else {
            pr->text[wr++] = ch;
            last_nl = false;
        }
    }
    pr->text[wr] = '\0';
    pr->text_len = wr;

    /* Trim leading whitespace */
    size_t trim = 0;
    while (trim < pr->text_len && (pr->text[trim] == '\n' || pr->text[trim] == ' '))
        trim++;
    if (trim > 0) {
        memmove(pr->text, pr->text + trim, pr->text_len - trim + 1);
        pr->text_len -= trim;
    }
}

/* =========================================================================
 * URL resolution
 * ========================================================================= */

/* Resolve a possibly-relative href against the current page URL. */
static void resolve_url(const char *base, const char *href, char *out, int out_sz)
{
    if (!href || !href[0]) { out[0] = '\0'; return; }

    /* Already absolute */
    if (!strncmp(href, "http://", 7) || !strncmp(href, "https://", 8)) {
        snprintf(out, out_sz, "%s", href);
        return;
    }

    /* Protocol-relative */
    if (!strncmp(href, "//", 2)) {
        /* Detect base protocol */
        if (!strncmp(base, "https://", 8))
            snprintf(out, out_sz, "https:%.*s", out_sz - 7, href);
        else
            snprintf(out, out_sz, "http:%.*s", out_sz - 6, href);
        return;
    }

    /* Build base origin (https://host) */
    char origin[BROWSER_URL_LEN] = {0};
    const char *after_scheme = strstr(base, "://");
    if (!after_scheme) { snprintf(out, out_sz, "%s", href); return; }
    after_scheme += 3;
    const char *slash = strchr(after_scheme, '/');
    size_t origin_len = slash ? (size_t)(slash - base) : strlen(base);
    if (origin_len >= BROWSER_URL_LEN) origin_len = BROWSER_URL_LEN - 1;
    memcpy(origin, base, origin_len);
    origin[origin_len] = '\0';

    /* Root-relative */
    if (href[0] == '/') {
        snprintf(out, out_sz, "%s%s", origin, href);
        return;
    }

    /* Relative to current directory */
    char dir[BROWSER_URL_LEN] = {0};
    const char *last_slash = strrchr(slash ? slash : after_scheme, '/');
    size_t dir_len = last_slash ? (size_t)(last_slash - base + 1) : strlen(base);
    if (dir_len >= BROWSER_URL_LEN) dir_len = BROWSER_URL_LEN - 1;
    memcpy(dir, base, dir_len);
    dir[dir_len] = '\0';
    snprintf(out, out_sz, "%s%s", dir, href);
}

/* =========================================================================
 * Internal page load
 * ========================================================================= */

static void page_free(BrowserCtx *b)
{
    if (b->page_text) { free(b->page_text); b->page_text = NULL; }
    if (b->raw_html)  { free(b->raw_html);  b->raw_html  = NULL; }
    b->link_count      = 0;
    b->selected_link   = 0;
    b->rendered_height = 0;
    b->scroll_y        = 0;
}

static void do_load(BrowserCtx *b, const char *url)
{
    page_free(b);
    strncpy(b->current_url, url, BROWSER_URL_LEN - 1);
    b->load_status   = BLOAD_PENDING;
    b->load_progress = 0.1f;
    b->page_title[0] = '\0';
    b->error_msg[0]  = '\0';
    b->raw_preview[0] = '\0';
    b->raw_status  = 0;
    b->raw_bytes   = 0;
    browser_debug(b, "do_load start: %s", url);

    /* Fetch — net_fetch handles redirects internally */
    NetResponse resp = {0};
    NetResult   rc = net_fetch(url, &resp);

    if (rc != NET_OK) {
        /* TLS failure (0xD8A0A03C) on https:// — retry with http://.
         * We do this here at browser level so the failed httpc context
         * is fully closed before we open a new one.                     */
        if (strncmp(url, "https://", 8) == 0 &&
            strstr(resp.final_url, "0xD8A0A03C") != NULL) {
            char http_url[BROWSER_URL_LEN];
            snprintf(http_url, BROWSER_URL_LEN, "http://%s", url + 8);
            browser_debug(b, "TLS fail, retrying as http://");
            net_response_free(&resp);
            rc = net_fetch(http_url, &resp);
            if (rc == NET_OK) {
                snprintf(b->current_url, BROWSER_URL_LEN, "%s", http_url);
                goto fetch_ok;
            }
        }
        snprintf(b->error_msg, sizeof(b->error_msg),
                 "Network error: %s", net_error_str(rc));
        snprintf(b->raw_preview, sizeof(b->raw_preview),
                 "%.195s", resp.final_url[0] ? resp.final_url : net_error_str(rc));
        browser_debug(b, "NET ERR: %s | %s", net_error_str(rc), resp.final_url);
        b->load_status = BLOAD_ERROR;
        return;
    }
    fetch_ok:;

    /* Update URL to final destination (after any redirects) */
    if (resp.final_url[0])
        snprintf(b->current_url, BROWSER_URL_LEN, "%s", resp.final_url);

    b->load_progress = 0.5f;
    /* Store raw preview for debug screen */
    b->raw_status = resp.status;
    b->raw_bytes  = resp.body_len;
    if (resp.body && resp.body_len > 0) {
        size_t plen = resp.body_len < 199 ? resp.body_len : 199;
        memcpy(b->raw_preview, resp.body, plen);
        b->raw_preview[plen] = '\0';
        /* Replace control chars with space for display */
        for (size_t _i = 0; _i < plen; _i++)
            if ((unsigned char)b->raw_preview[_i] < 32)
                b->raw_preview[_i] = ' ';
    }
    browser_debug(b, "Fetch OK status=%d bytes=%zu", resp.status, resp.body_len);

    if (resp.status >= 400) {
        snprintf(b->error_msg, sizeof(b->error_msg),
                 "HTTP %d", resp.status);
        browser_debug(b, "HTTP ERR %d", resp.status);
        net_response_free(&resp);
        b->load_status = BLOAD_ERROR;
        return;
    }

    /* Store a copy of raw HTML for JS script extraction */
    if (resp.body && resp.body_len > 0) {
        b->raw_html = (char *)malloc(resp.body_len + 1);
        if (b->raw_html) {
            memcpy(b->raw_html, resp.body, resp.body_len);
            b->raw_html[resp.body_len] = '\0';
        }
    }

    /* Parse — use heap for ParseResult; html_parse also heap-allocs links  */
    ParseResult *pr = (ParseResult *)calloc(1, sizeof(ParseResult));
    if (!pr) { net_response_free(&resp); b->load_status = BLOAD_ERROR;
               snprintf(b->error_msg, sizeof(b->error_msg), "OOM"); return; }
    html_parse(resp.body, resp.body_len, pr);
    net_response_free(&resp);

    b->load_progress = 0.8f;
    browser_debug(b, "Parsed text=%zu links=%d", pr->text_len, pr->link_count);

    /* Title */
    if (pr->title[0])
        snprintf(b->page_title, BROWSER_TITLE_LEN, "%s", pr->title);
    else
        strncpy(b->page_title, url, BROWSER_TITLE_LEN - 1);

    /* Text */
    b->page_text = pr->text;   /* ownership transfer — do NOT free pr->text */


    /* Links — resolve relative hrefs */
    b->link_count = pr->link_count < BROWSER_MAX_LINKS ? pr->link_count : BROWSER_MAX_LINKS;
    for (int i = 0; i < b->link_count; i++) {
        char resolved[BROWSER_URL_LEN];
        resolved[0] = '\0';
        resolve_url(url, pr->links[i].href, resolved, BROWSER_URL_LEN);
        snprintf(b->links[i].href, BROWSER_URL_LEN, "%s", resolved);
        snprintf(b->links[i].text, 128, "%s", pr->links[i].text);
    }

    /* Free the heap-allocated links array (text ownership was transferred) */
    if (pr->links) free(pr->links);
    free(pr);

    /* History */
    /* If we navigated away from a non-tail position, trim forward entries */
    if (b->history_pos < b->history_count - 1)
        b->history_count = b->history_pos + 1;

    if (b->history_count < BROWSER_MAX_HISTORY) {
        strncpy(b->history[b->history_count], url, BROWSER_URL_LEN - 1);
        b->history_pos  = b->history_count;
        b->history_count++;
    } else {
        /* Scroll history ring buffer */
        memmove(b->history[0], b->history[1],
                (BROWSER_MAX_HISTORY - 1) * BROWSER_URL_LEN);
        strncpy(b->history[BROWSER_MAX_HISTORY - 1], url, BROWSER_URL_LEN - 1);
        b->history_pos = BROWSER_MAX_HISTORY - 1;
    }

    b->load_progress = 1.0f;
    b->load_status   = BLOAD_DONE;
    browser_debug(b, "Done title='%.30s' textlen=%zu", b->page_title,
                  b->page_text ? strlen(b->page_text) : 0);

    /* Debug: record diagnostic in error_msg (cleared on success) so we can
     * display it even when page_text is empty. */
    snprintf(b->error_msg, sizeof(b->error_msg),
             "OK %d  text:%zu  links:%d",
             (int)resp.status, b->page_text ? strlen(b->page_text) : 0,
             b->link_count);
    /* Keep load_status as DONE — error_msg is informational only here. */
    b->error_msg[0] = '\0';   /* comment out this line to keep the debug msg */
}

/* =========================================================================
 * Public API — lifecycle
 * ========================================================================= */

void browser_debug(BrowserCtx *b, const char *fmt, ...)
{
    if (!b) return;
    int slot = b->debug_count % BROWSER_DEBUG_LINES;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b->debug_log[slot], BROWSER_DEBUG_LINE_LEN, fmt, ap);
    va_end(ap);
    b->debug_count++;
    printf("[DBG] %s\n", b->debug_log[slot]);
}

void browser_init(BrowserCtx *b)
{
    memset(b, 0, sizeof(*b));
    b->load_status = BLOAD_IDLE;
}

void browser_exit(BrowserCtx *b)
{
    page_free(b);
}

/* =========================================================================
 * Public API — navigation
 * ========================================================================= */

void browser_navigate(BrowserCtx *b, const char *url)
{
    strncpy(b->current_url, url, BROWSER_URL_LEN - 1);
    b->load_status   = BLOAD_PENDING;
    b->load_progress = 0.0f;
}

void browser_cancel(BrowserCtx *b)
{
    b->load_status = BLOAD_IDLE;
}

/*
 * browser_poll — called every frame while in STATE_LOADING.
 * Performs the actual fetch on the first call after browser_navigate().
 */
BrowserLoadStatus browser_poll(BrowserCtx *b)
{
    if (b->load_status == BLOAD_PENDING)
        do_load(b, b->current_url);
    return b->load_status;
}

bool browser_back(BrowserCtx *b)
{
    if (b->history_pos <= 0) return false;
    b->history_pos--;
    do_load(b, b->history[b->history_pos]);
    /* do_load appends to history; undo that */
    if (b->history_count > 0) b->history_count--;
    return true;
}

void browser_forward(BrowserCtx *b)
{
    if (b->history_pos >= b->history_count - 1) return;
    b->history_pos++;
    const char *url = b->history[b->history_pos];
    do_load(b, url);
    if (b->history_count > 0) b->history_count--;
}

void browser_reload(BrowserCtx *b)
{
    if (b->current_url[0]) {
        char url[BROWSER_URL_LEN];
        snprintf(url, BROWSER_URL_LEN, "%s", b->current_url);
        do_load(b, url);
    }
}

/* =========================================================================
 * Public API — scrolling
 * ========================================================================= */

void browser_scroll(BrowserCtx *b, int delta)
{
    b->scroll_y += delta;
    if (b->scroll_y < 0) b->scroll_y = 0;
}

int browser_max_scroll(BrowserCtx *b, int viewport_h)
{
    int ms = b->rendered_height - viewport_h;
    return ms > 0 ? ms : 0;
}

/* =========================================================================
 * Public API — links
 * ========================================================================= */

int browser_link_count(BrowserCtx *b)
{
    return b->link_count;
}

const char *browser_link_text(BrowserCtx *b, int idx)
{
    if (idx < 0 || idx >= b->link_count) return "";
    return b->links[idx].text;
}

const char *browser_selected_href(BrowserCtx *b)
{
    if (b->selected_link < 0 || b->selected_link >= b->link_count) return NULL;
    return b->links[b->selected_link].href;
}

/* =========================================================================
 * Public API — bookmarks
 * ========================================================================= */

void browser_add_bookmark(BrowserCtx *b, const char *title, const char *url)
{
    /* Deduplicate */
    for (int i = 0; i < b->bookmark_count; i++)
        if (!strcmp(b->bookmarks[i].url, url)) return;

    if (b->bookmark_count >= BROWSER_MAX_BOOKMARKS) return;
    Bookmark *bk = &b->bookmarks[b->bookmark_count++];
    strncpy(bk->title, title && title[0] ? title : url, BROWSER_TITLE_LEN - 1);
    strncpy(bk->url,   url,  BROWSER_URL_LEN   - 1);
}

void browser_delete_bookmark(BrowserCtx *b, int idx)
{
    if (idx < 0 || idx >= b->bookmark_count) return;
    memmove(&b->bookmarks[idx], &b->bookmarks[idx + 1],
            (b->bookmark_count - idx - 1) * sizeof(Bookmark));
    b->bookmark_count--;
}

/* =========================================================================
 * Public API — history
 * ========================================================================= */

void browser_clear_history(BrowserCtx *b)
{
    b->history_count = 0;
    b->history_pos   = 0;
}

/* =========================================================================
 * Public API — citro2d rendering

/* =========================================================================
 * Styled renderer
 *
 * Marker bytes from html_parse:
 *   \x01 = <h1>   \x02 = <h2>   \x03 = <h3/h4>   \x04 = <a href>
 * ========================================================================= */
#define MARK_H1   '\x01'
#define MARK_H2   '\x02'
#define MARK_H3   '\x03'
#define MARK_LINK '\x04'

typedef struct {
    float scale;
    float line_h;
    u32   color;
    float indent;
    float pre_gap;
} LineStyle;

static int marker_pri(char m)
{
    if (m == MARK_H1)   return 4;
    if (m == MARK_H2)   return 3;
    if (m == MARK_H3)   return 2;
    if (m == MARK_LINK) return 1;
    return 0;
}

static LineStyle get_lstyle(char marker, float bs, float blh)
{
    LineStyle s;
    switch (marker) {
    case MARK_H1:
        s.scale   = bs * 2.2f;  s.line_h = blh * 2.4f;
        s.color   = 0xFFFFFFFF; s.indent = 0; s.pre_gap = blh * 0.8f; break;
    case MARK_H2:
        s.scale   = bs * 1.6f;  s.line_h = blh * 1.8f;
        s.color   = 0xFF66CCFF; s.indent = 0; s.pre_gap = blh * 0.6f; break;
    case MARK_H3:
        s.scale   = bs * 1.3f;  s.line_h = blh * 1.45f;
        s.color   = 0xFF88FFBB; s.indent = 0; s.pre_gap = blh * 0.3f; break;
    case MARK_LINK:
        s.scale   = bs * 0.95f; s.line_h = blh * 1.1f;
        s.color   = 0xFFFF9933; s.indent = 10; s.pre_gap = 0; break;
    default:
        s.scale   = bs;         s.line_h = blh;
        s.color   = 0xFFCCCCCC; s.indent = 0; s.pre_gap = 0; break;
    }
    return s;
}

void browser_draw(BrowserCtx *b, C2D_Font font, C2D_TextBuf buf,
                  float x, float y, float w, float h,
                  int scroll_y, float bs, float blh)
{
    if (!b->page_text) return;

    const char *p     = b->page_text;
    float cur_y = y;
    float bot_y = y + h;
    int   pix_y = 0;
    int   max_h = 0;
    char  line_buf[128];

    while (*p) {
        /* Consume all leading marker bytes; keep highest priority */
        char marker = 0;
        while (*p && (unsigned char)*p < 0x10) {
            if (marker_pri(*p) > marker_pri(marker)) marker = *p;
            p++;
        }
        if (!*p) break;

        LineStyle ls = get_lstyle(marker, bs, blh);

        /* Pre-gap before headings */
        if (ls.pre_gap > 0 && pix_y > 0) {
            if (pix_y >= scroll_y) cur_y += ls.pre_gap;
            pix_y += (int)ls.pre_gap;
        }

        /* Logical line */
        const char *nl   = strchr(p, '\n');
        size_t      span = nl ? (size_t)(nl - p) : strlen(p);

        /* Skip blank lines (render as small gap) */
        bool all_sp = true;
        for (size_t si = 0; si < span; si++)
            if (p[si] != ' ') { all_sp = false; break; }
        if (all_sp && span > 0) {
            float gap = ls.line_h * 0.4f;
            if (pix_y >= scroll_y) cur_y += gap;
            pix_y += (int)gap;
            p += span + (nl ? 1 : 0);
            if (!nl) break;
            continue;
        }

        /* Word-wrap */
        int max_ch = (int)((w - ls.indent) / (ls.scale * 7.2f));
        if (max_ch < 8)   max_ch = 8;
        if (max_ch > 120) max_ch = 120;

        size_t done = 0;
        bool first_chunk = true;
        while (done < span) {
            size_t chunk = span - done;
            if (chunk > (size_t)max_ch) {
                chunk = (size_t)max_ch;
                while (chunk > 1 && p[done+chunk] != ' ') chunk--;
            }

            if (pix_y >= scroll_y && cur_y < bot_y) {
                int clen = (chunk < 127) ? (int)chunk : 127;
                memcpy(line_buf, p + done, clen);
                line_buf[clen] = '\0';
                const char *dp = line_buf;
                if (!first_chunk && *dp == ' ') dp++;

                if (*dp) {
                    /* Link bullet */
                    if (marker == MARK_LINK && first_chunk) {
                        C2D_Text bt;
                        C2D_TextFontParse(&bt, font, buf, ">");
                        C2D_TextOptimize(&bt);
                        C2D_DrawText(&bt, C2D_WithColor, x+1, cur_y, 0.5f,
                                     ls.scale*0.8f, ls.scale*0.8f, 0xFFFF6600);
                    }

                    C2D_Text t;
                    C2D_TextFontParse(&t, font, buf, dp);
                    C2D_TextOptimize(&t);
                    float tx = x + ls.indent;
                    C2D_DrawText(&t, C2D_WithColor, tx, cur_y, 0.5f,
                                 ls.scale, ls.scale, ls.color);

                    /* H1: thick underline */
                    if (marker == MARK_H1 && first_chunk) {
                        float tw=0,th=0;
                        C2D_TextGetDimensions(&t, ls.scale, ls.scale, &tw, &th);
                        C2D_DrawRectSolid(tx, cur_y+th+1, 0.5f, w-ls.indent, 2, 0xFF4488FF);
                    }
                    /* H2: thin accent */
                    else if (marker == MARK_H2 && first_chunk) {
                        float tw=0,th=0;
                        C2D_TextGetDimensions(&t, ls.scale, ls.scale, &tw, &th);
                        C2D_DrawRectSolid(tx, cur_y+th+1, 0.5f, tw*0.6f, 1, 0xFF4499FF);
                    }
                    /* Link underline */
                    else if (marker == MARK_LINK) {
                        float tw=0,th=0;
                        C2D_TextGetDimensions(&t, ls.scale, ls.scale, &tw, &th);
                        C2D_DrawRectSolid(tx, cur_y+th, 0.5f, tw, 1, 0xAAFF8800);
                    }
                }
            }

            if (pix_y >= scroll_y) cur_y += ls.line_h;
            pix_y += (int)ls.line_h;
            if (pix_y > max_h) max_h = pix_y;
            done += chunk;
            first_chunk = false;
        }

        p += span + (nl ? 1 : 0);
        if (!nl) break;
    }

    b->rendered_height = max_h;
    int ms = browser_max_scroll(b, (int)h);
    if (b->scroll_y > ms) b->scroll_y = ms;
}

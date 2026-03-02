/*
 * html.c - HTML-to-text parser + script extractor
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "html.h"

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static int ci_starts(const char* s, const char* pre) {
    while (*pre) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*pre)) return 0;
        s++; pre++;
    }
    return 1;
}

/* Decode common HTML entities in-place */
static void decode_entities(char* s) {
    char* r = s, *w = s;
    while (*r) {
        if (*r == '&') {
            if      (ci_starts(r, "&amp;"))  { *w++ = '&';  r += 5; }
            else if (ci_starts(r, "&lt;"))   { *w++ = '<';  r += 4; }
            else if (ci_starts(r, "&gt;"))   { *w++ = '>';  r += 4; }
            else if (ci_starts(r, "&nbsp;")) { *w++ = ' ';  r += 6; }
            else if (ci_starts(r, "&quot;")) { *w++ = '"';  r += 6; }
            else if (ci_starts(r, "&#"))     {
                /* numeric entity &#NNN; or &#xNN; */
                r += 2;
                int val = 0;
                if (*r == 'x' || *r == 'X') {
                    r++;
                    while (isxdigit((unsigned char)*r)) {
                        val = val * 16 + (isdigit((unsigned char)*r)
                              ? *r - '0' : tolower((unsigned char)*r) - 'a' + 10);
                        r++;
                    }
                } else {
                    while (isdigit((unsigned char)*r)) { val = val * 10 + (*r - '0'); r++; }
                }
                if (*r == ';') r++;
                /* Only output printable ASCII */
                *w++ = (val >= 32 && val < 128) ? (char)val : ' ';
            } else {
                *w++ = *r++;
            }
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* Extract content between two tags (case-insensitive), returns malloc'd string */
static char* extract_between(const char* html, const char* open_tag,
                               const char* close_tag) {
    const char* p = html;
    size_t olen = strlen(open_tag), clen = strlen(close_tag);
    while (*p) {
        if (ci_starts(p, open_tag)) {
            /* skip to end of opening tag */
            const char* start = p + olen;
            while (*start && *start != '>') start++;
            if (*start == '>') start++;
            const char* end = start;
            while (*end) {
                if (ci_starts(end, close_tag)) {
                    size_t n = (size_t)(end - start);
                    char* out = (char*)malloc(n + 1);
                    if (!out) return NULL;
                    memcpy(out, start, n);
                    out[n] = '\0';
                    return out;
                }
                end++;
            }
        }
        p++;
    }
    return NULL;
}

/* ── Main parser ─────────────────────────────────────────────────────────── */

int html_parse(const char* html, size_t len, HtmlDoc* doc) {
    if (!html || !doc) return -1;
    memset(doc, 0, sizeof(*doc));

    doc->text   = (char*)calloc(1, HTML_MAX_TEXT + 1);
    doc->scripts = (char**)calloc(HTML_MAX_SCRIPTS, sizeof(char*));
    doc->links   = (HtmlLink*)calloc(HTML_MAX_LINKS, sizeof(HtmlLink));
    if (!doc->text || !doc->scripts || !doc->links) {
        html_doc_free(doc);
        return -1;
    }

    /* Extract <title> */
    doc->title = extract_between(html, "<title", "</title>");
    if (doc->title) decode_entities(doc->title);

    /* Extract all <script>…</script> blocks (inline JS only, not src=) */
    const char* p = html;
    while (*p && doc->script_count < HTML_MAX_SCRIPTS) {
        if (ci_starts(p, "<script")) {
            /* Check if it has a src= attribute – skip those */
            const char* tag_end = p + 7;
            int has_src = 0;
            while (*tag_end && *tag_end != '>') {
                if (ci_starts(tag_end, "src=")) { has_src = 1; break; }
                tag_end++;
            }
            while (*tag_end && *tag_end != '>') tag_end++;
            if (*tag_end == '>') tag_end++;

            /* Find </script> */
            const char* end = tag_end;
            while (*end) {
                if (ci_starts(end, "</script")) break;
                end++;
            }
            if (!has_src && end > tag_end) {
                size_t n = (size_t)(end - tag_end);
                char* sc = (char*)malloc(n + 1);
                if (sc) {
                    memcpy(sc, tag_end, n);
                    sc[n] = '\0';
                    doc->scripts[doc->script_count++] = sc;
                }
            }
            p = end;
        }
        p++;
    }

    /* Strip tags → plain text */
    char*       w       = doc->text;
    char*       w_end   = doc->text + HTML_MAX_TEXT;
    const char* r       = html;
    int         in_tag  = 0;
    int         in_style= 0;
    int         in_script= 0;
    int         last_space = 1;  /* suppress leading spaces */

    while (*r && w < w_end - 2) {
        if (in_style) {
            if (ci_starts(r, "</style")) { in_style = 0; while (*r && *r != '>') r++; }
            r++; continue;
        }
        if (in_script) {
            if (ci_starts(r, "</script")) { in_script = 0; while (*r && *r != '>') r++; }
            r++; continue;
        }
        if (*r == '<') {
            /* Detect block-level tags → newline */
            if (ci_starts(r+1, "br") || ci_starts(r+1, "/p") ||
                ci_starts(r+1, "p ") || ci_starts(r+1, "p>") ||
                ci_starts(r+1, "div") || ci_starts(r+1, "/div") ||
                ci_starts(r+1, "li") || ci_starts(r+1, "tr") ||
                ci_starts(r+1, "h1") || ci_starts(r+1, "h2") ||
                ci_starts(r+1, "h3") || ci_starts(r+1, "h4")) {
                if (w > doc->text && *(w-1) != '\n') *w++ = '\n';
                last_space = 1;
            }
            if (ci_starts(r+1, "style")) { in_style  = 1; }
            if (ci_starts(r+1, "script")){ in_script = 1; }

            /* Extract href from <a> tags */
            if ((r[1] == 'a' || r[1] == 'A') && (r[2] == ' ' || r[2] == '\t')) {
                if (doc->link_count < HTML_MAX_LINKS) {
                    const char* ap = r;
                    while (*ap && !ci_starts(ap, "href=")) ap++;
                    if (*ap) {
                        ap += 5;
                        char q = (*ap == '"' || *ap == '\'') ? *ap++ : 0;
                        const char* hs = ap;
                        while (*ap && (q ? *ap != q : *ap != ' ' && *ap != '>')) ap++;
                        size_t hn = (size_t)(ap - hs);
                        if (hn > 0 && hn < 511) {
                            memcpy(doc->links[doc->link_count].href, hs, hn);
                            doc->links[doc->link_count].href[hn] = '\0';
                            doc->link_count++;
                        }
                    }
                }
            }

            in_tag = 1;
            r++;
            continue;
        }
        if (*r == '>') { in_tag = 0; r++; continue; }
        if (in_tag) { r++; continue; }

        /* Content char */
        if (*r == '\n' || *r == '\r' || *r == '\t' || *r == ' ') {
            if (!last_space) { *w++ = ' '; last_space = 1; }
            r++;
        } else {
            *w++ = *r++;
            last_space = 0;
        }
    }
    *w = '\0';
    doc->text_len = (size_t)(w - doc->text);
    decode_entities(doc->text);

    /* Compact multiple blank lines */
    char* src2 = doc->text;
    char* dst2 = (char*)malloc(doc->text_len + 2);
    if (dst2) {
        char* dw = dst2;
        int blanks = 0;
        while (*src2) {
            if (*src2 == '\n') {
                blanks++;
                if (blanks <= 2) *dw++ = '\n';
            } else {
                blanks = 0;
                *dw++ = *src2;
            }
            src2++;
        }
        *dw = '\0';
        memcpy(doc->text, dst2, (size_t)(dw - dst2) + 1);
        doc->text_len = (size_t)(dw - dst2);
        free(dst2);
    }

    return 0;
}

void html_doc_free(HtmlDoc* doc) {
    if (!doc) return;
    free(doc->text);    doc->text = NULL;
    free(doc->title);   doc->title = NULL;
    free(doc->links);   doc->links = NULL;
    if (doc->scripts) {
        for (int i = 0; i < doc->script_count; i++) free(doc->scripts[i]);
        free(doc->scripts);
        doc->scripts = NULL;
    }
    doc->script_count = 0;
    doc->link_count   = 0;
}

/* ── URL resolution ──────────────────────────────────────────────────────── */

void html_resolve_url(const char* base, const char* rel, char* out, size_t out_sz) {
    if (!rel || !out) return;
    /* Already absolute */
    if (strncmp(rel, "http://", 7) == 0 || strncmp(rel, "https://", 8) == 0) {
        strncpy(out, rel, out_sz - 1); out[out_sz-1] = '\0'; return;
    }
    if (!base) { strncpy(out, rel, out_sz - 1); out[out_sz-1] = '\0'; return; }

    /* Protocol-relative */
    if (rel[0] == '/' && rel[1] == '/') {
        const char* proto_end = strstr(base, "://");
        if (proto_end) {
            size_t plen = (size_t)(proto_end - base) + 1; /* include : */
            if (plen < out_sz) {
                memcpy(out, base, plen);
                strncpy(out + plen, rel + 1, out_sz - plen - 1);
                out[out_sz-1] = '\0';
            }
        }
        return;
    }
    /* Root-relative */
    if (rel[0] == '/') {
        const char* proto_end = strstr(base, "://");
        if (proto_end) {
            const char* host_end = strchr(proto_end + 3, '/');
            size_t hlen = host_end ? (size_t)(host_end - base) : strlen(base);
            if (hlen < out_sz - 1) {
                memcpy(out, base, hlen);
                strncpy(out + hlen, rel, out_sz - hlen - 1);
                out[out_sz-1] = '\0';
            }
        }
        return;
    }
    /* Relative – append to base directory */
    const char* last_slash = strrchr(base, '/');
    size_t base_dir_len = last_slash ? (size_t)(last_slash - base + 1) : strlen(base);
    if (base_dir_len + strlen(rel) < out_sz - 1) {
        memcpy(out, base, base_dir_len);
        strcpy(out + base_dir_len, rel);
    } else {
        strncpy(out, rel, out_sz - 1); out[out_sz-1] = '\0';
    }
}

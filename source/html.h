/*
 * html.h - Simple HTML-to-text parser for FoxSearch
 */
#pragma once
#include <stddef.h>

#define HTML_MAX_TEXT    (64 * 1024)
#define HTML_MAX_SCRIPTS 32
#define HTML_MAX_LINKS   128

typedef struct {
    char  href[512];
    char  text[128];
} HtmlLink;

typedef struct {
    char*    text;           /* cleaned readable text (caller frees) */
    size_t   text_len;
    char*    title;          /* <title> content (caller frees) */

    char**   scripts;        /* array of <script> bodies (caller frees each + array) */
    int      script_count;

    HtmlLink* links;         /* array of <a href> links */
    int       link_count;
} HtmlDoc;

/* Parse HTML into a readable HtmlDoc. Returns 0 on success. */
int  html_parse(const char* html, size_t len, HtmlDoc* doc);
void html_doc_free(HtmlDoc* doc);

/* Resolve a relative URL against a base */
void html_resolve_url(const char* base, const char* rel, char* out, size_t out_sz);

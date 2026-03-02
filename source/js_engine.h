/*
 * js_engine.h - QuickJS JavaScript engine API
 *
 * Provides a per-session JS context with browser and fetch bindings.
 * js_run_page_scripts() extracts and runs <script> tags from the loaded page.
 * js_eval_in_page() runs arbitrary JS entered via the console.
 */
#pragma once
#include <stdbool.h>
#include "browser.h"

/* Initialise the QuickJS runtime. Call once at startup. */
void js_init(void);
void js_exit(void);

/* Returns true if the engine initialised successfully. */
bool js_is_ready(void);

/*
 * Run all <script>...</script> blocks found in the current page's raw HTML.
 * (Stored by the browser after a successful load.)
 */
void js_run_page_scripts(BrowserCtx *b);

/*
 * Evaluate a JS expression entered by the user.
 * The string result (or error) is written into out_buf[out_sz].
 */
void js_eval_in_page(BrowserCtx *b,
                     const char *input,
                     char *out_buf, int out_sz);

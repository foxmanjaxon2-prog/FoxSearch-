/*
 * js_engine.c — QuickJS with DOM stubs, running in a dedicated thread
 *
 * JS execution happens on a worker thread with a 512 KB stack so that
 * QuickJS's deep recursion can't overflow the main thread's stack.
 * Communication uses a simple shared struct protected by a LightEvent.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <3ds.h>
#include "js_engine.h"
#include "browser.h"
#include "net.h"
#include "quickjs/quickjs.h"
#include "quickjs/quickjs-libc.h"

/* ── Worker thread stack & handles ──────────────────────────────────── */
#define JS_THREAD_STACK   (512 * 1024)   /* 512 KB for QuickJS recursion */
#define JS_HEAP_LIMIT     (8  * 1024 * 1024)
#define JS_STACK_LIMIT    (384 * 1024)
#define JS_MAX_SCRIPT_LEN (16 * 1024)    /* skip scripts larger than 16 KB */
#define JS_MAX_SCRIPTS    4

static Thread      g_thread;
static LightEvent  g_req_event;   /* main  → worker: "work to do"   */
static LightEvent  g_done_event;  /* worker → main:  "work done"    */
static volatile bool g_thread_running = false;
static volatile bool g_exit_requested = false;

/* ── Shared work descriptor ──────────────────────────────────────────── */
typedef enum { JOB_NONE, JOB_RUN_SCRIPTS, JOB_EVAL } JobType;

static struct {
    JobType     type;
    BrowserCtx *browser;
    char        input[512];     /* for JOB_EVAL */
    char        output[2048];
} g_job;

/* ── QuickJS state (lives on worker thread) ──────────────────────────── */
static JSRuntime  *g_rt  = NULL;
static JSContext  *g_ctx = NULL;
static BrowserCtx *g_browser = NULL;

/* document.write() buffer */
static char   g_doc_buf[8192];
static size_t g_doc_len = 0;

/* console output buffer */
static char   g_log_buf[2048];
static int    g_log_len = 0;

static void log_append(const char *pfx, const char *msg)
{
    char line[256];
    snprintf(line, sizeof(line), "%s%s", pfx ? pfx : "", msg ? msg : "");
    int ll = strlen(line);
    if (g_log_len + ll + 2 < (int)sizeof(g_log_buf)) {
        memcpy(g_log_buf + g_log_len, line, ll);
        g_log_len += ll;
        g_log_buf[g_log_len++] = '\n';
        g_log_buf[g_log_len]   = '\0';
    }
    printf("[JS] %s\n", line);
}

/* =========================================================================
 * JS Bindings
 * ========================================================================= */

static JSValue js_log_impl(JSContext *ctx, JSValueConst tv,
                           int argc, JSValueConst *argv, const char *pfx)
{
    char line[256] = {0}; int pos = 0;
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (s) {
            if (i && pos < 254) line[pos++] = ' ';
            int sl = strlen(s); if (pos+sl > 254) sl = 254-pos;
            memcpy(line+pos, s, sl); pos += sl;
            JS_FreeCString(ctx, s);
        }
    }
    line[pos] = '\0';
    log_append(pfx, line);
    return JS_UNDEFINED;
}
static JSValue js_console_log(JSContext *c,JSValueConst t,int a,JSValueConst *v)
{ return js_log_impl(c,t,a,v,""); }
static JSValue js_console_err(JSContext *c,JSValueConst t,int a,JSValueConst *v)
{ return js_log_impl(c,t,a,v,"[ERR] "); }
static JSValue js_console_warn(JSContext *c,JSValueConst t,int a,JSValueConst *v)
{ return js_log_impl(c,t,a,v,"[WARN] "); }

static JSValue js_fetch(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "fetch: need URL");
    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;
    NetResponse resp = {0};
    NetResult rc = net_fetch(url, &resp);
    JS_FreeCString(ctx, url);
    if (rc != NET_OK) return JS_ThrowTypeError(ctx, "fetch failed");
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "status", JS_NewInt32(ctx, resp.status));
    JS_SetPropertyStr(ctx, obj, "ok", JS_NewBool(ctx, resp.status < 300));
    JS_SetPropertyStr(ctx, obj, "body",
        resp.body ? JS_NewStringLen(ctx, resp.body, resp.body_len)
                  : JS_NewString(ctx, ""));
    net_response_free(&resp);
    return obj;
}

static JSValue js_doc_write(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (s) {
            size_t sl = strlen(s);
            if (g_doc_len + sl < sizeof(g_doc_buf) - 1) {
                memcpy(g_doc_buf + g_doc_len, s, sl);
                g_doc_len += sl;
                g_doc_buf[g_doc_len] = '\0';
            }
            JS_FreeCString(ctx, s);
        }
    }
    return JS_UNDEFINED;
}
static JSValue js_noop(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{ return JS_UNDEFINED; }
static JSValue js_noop_null(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{ return JS_NULL; }
static JSValue js_navigate(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{
    if (g_browser && argc > 0) {
        const char *url = JS_ToCString(ctx, argv[0]);
        if (url) { browser_navigate(g_browser, url); JS_FreeCString(ctx, url); }
    }
    return JS_UNDEFINED;
}

/* Minimal element stub — enough to survive feature-detection scripts */
static JSValue make_element(JSContext *ctx)
{
    JSValue el = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, el, "innerHTML",        JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, el, "textContent",      JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, el, "className",        JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, el, "id",               JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, el, "style",            JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, el, "classList",        JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, el, "children",         JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, el, "setAttribute",     JS_NewCFunction(ctx, js_noop, "setAttribute", 2));
    JS_SetPropertyStr(ctx, el, "getAttribute",     JS_NewCFunction(ctx, js_noop_null, "getAttribute", 1));
    JS_SetPropertyStr(ctx, el, "addEventListener", JS_NewCFunction(ctx, js_noop, "addEventListener", 2));
    JS_SetPropertyStr(ctx, el, "removeEventListener", JS_NewCFunction(ctx, js_noop, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, el, "appendChild",      JS_NewCFunction(ctx, js_noop_null, "appendChild", 1));
    JS_SetPropertyStr(ctx, el, "removeChild",      JS_NewCFunction(ctx, js_noop_null, "removeChild", 1));
    JS_SetPropertyStr(ctx, el, "querySelector",    JS_NewCFunction(ctx, js_noop_null, "querySelector", 1));
    JS_SetPropertyStr(ctx, el, "querySelectorAll", JS_NewCFunction(ctx, js_noop_null, "querySelectorAll", 1));
    JS_SetPropertyStr(ctx, el, "getBoundingClientRect",
        JS_NewCFunction(ctx, js_noop_null, "getBoundingClientRect", 0));
    return el;
}
static JSValue js_get_element(JSContext *ctx, JSValueConst tv, int argc, JSValueConst *argv)
{ return make_element(ctx); }

static void register_globals(JSContext *ctx)
{
    JSValue g = JS_GetGlobalObject(ctx);

    /* console */
    JSValue con = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, con, "log",   JS_NewCFunction(ctx, js_console_log,  "log",   1));
    JS_SetPropertyStr(ctx, con, "error", JS_NewCFunction(ctx, js_console_err,  "error", 1));
    JS_SetPropertyStr(ctx, con, "warn",  JS_NewCFunction(ctx, js_console_warn, "warn",  1));
    JS_SetPropertyStr(ctx, con, "info",  JS_NewCFunction(ctx, js_console_log,  "info",  1));
    JS_SetPropertyStr(ctx, con, "debug", JS_NewCFunction(ctx, js_console_log,  "debug", 1));
    JS_SetPropertyStr(ctx, g, "console", con);

    /* timers — no-ops */
    JS_SetPropertyStr(ctx, g, "setTimeout",           JS_NewCFunction(ctx, js_noop_null, "setTimeout",           2));
    JS_SetPropertyStr(ctx, g, "setInterval",          JS_NewCFunction(ctx, js_noop_null, "setInterval",          2));
    JS_SetPropertyStr(ctx, g, "clearTimeout",         JS_NewCFunction(ctx, js_noop,      "clearTimeout",         1));
    JS_SetPropertyStr(ctx, g, "clearInterval",        JS_NewCFunction(ctx, js_noop,      "clearInterval",        1));
    JS_SetPropertyStr(ctx, g, "requestAnimationFrame",JS_NewCFunction(ctx, js_noop_null, "requestAnimationFrame",1));
    JS_SetPropertyStr(ctx, g, "cancelAnimationFrame", JS_NewCFunction(ctx, js_noop,      "cancelAnimationFrame", 1));
    JS_SetPropertyStr(ctx, g, "queueMicrotask",       JS_NewCFunction(ctx, js_noop,      "queueMicrotask",       1));

    /* fetch */
    JS_SetPropertyStr(ctx, g, "fetch", JS_NewCFunction(ctx, js_fetch, "fetch", 1));

    /* navigator */
    JSValue nav = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, nav, "userAgent",      JS_NewString(ctx, "Mozilla/5.0 (Nintendo 3DS) FoxSearch/0.1"));
    JS_SetPropertyStr(ctx, nav, "language",       JS_NewString(ctx, "en-US"));
    JS_SetPropertyStr(ctx, nav, "cookieEnabled",  JS_NewBool(ctx, 0));
    JS_SetPropertyStr(ctx, nav, "onLine",         JS_NewBool(ctx, 1));
    JS_SetPropertyStr(ctx, nav, "platform",       JS_NewString(ctx, "Nintendo 3DS"));
    JS_SetPropertyStr(ctx, nav, "maxTouchPoints", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, nav, "hardwareConcurrency", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, nav, "geolocation",    JS_NULL);
    JS_SetPropertyStr(ctx, nav, "serviceWorker",  JS_UNDEFINED);
    JS_SetPropertyStr(ctx, g, "navigator", nav);

    /* location */
    JSValue loc = JS_NewObject(ctx);
    const char *cur_url = (g_browser && g_browser->current_url[0])
                          ? g_browser->current_url : "http://localhost/";
    JS_SetPropertyStr(ctx, loc, "href",     JS_NewString(ctx, cur_url));
    JS_SetPropertyStr(ctx, loc, "hostname", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, loc, "pathname", JS_NewString(ctx, "/"));
    JS_SetPropertyStr(ctx, loc, "protocol", JS_NewString(ctx, "http:"));
    JS_SetPropertyStr(ctx, loc, "search",   JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, loc, "hash",     JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, loc, "assign",   JS_NewCFunction(ctx, js_navigate, "assign",  1));
    JS_SetPropertyStr(ctx, loc, "replace",  JS_NewCFunction(ctx, js_navigate, "replace", 1));
    JS_SetPropertyStr(ctx, g, "location", loc);

    /* history stub */
    JSValue hist = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, hist, "pushState",    JS_NewCFunction(ctx, js_noop, "pushState",    3));
    JS_SetPropertyStr(ctx, hist, "replaceState", JS_NewCFunction(ctx, js_noop, "replaceState", 3));
    JS_SetPropertyStr(ctx, hist, "back",         JS_NewCFunction(ctx, js_noop, "back",         0));
    JS_SetPropertyStr(ctx, hist, "length",       JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, g, "history", hist);

    /* screen */
    JSValue scr = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, scr, "width",       JS_NewInt32(ctx, 400));
    JS_SetPropertyStr(ctx, scr, "height",      JS_NewInt32(ctx, 240));
    JS_SetPropertyStr(ctx, scr, "colorDepth",  JS_NewInt32(ctx, 16));
    JS_SetPropertyStr(ctx, g, "screen", scr);

    /* window */
    JS_SetPropertyStr(ctx, g, "window",         JS_DupValue(ctx, g));
    JS_SetPropertyStr(ctx, g, "self",           JS_DupValue(ctx, g));
    JS_SetPropertyStr(ctx, g, "top",            JS_DupValue(ctx, g));
    JS_SetPropertyStr(ctx, g, "frames",         JS_DupValue(ctx, g));
    JS_SetPropertyStr(ctx, g, "innerWidth",     JS_NewInt32(ctx, 400));
    JS_SetPropertyStr(ctx, g, "innerHeight",    JS_NewInt32(ctx, 240));
    JS_SetPropertyStr(ctx, g, "devicePixelRatio", JS_NewFloat64(ctx, 1.0));
    JS_SetPropertyStr(ctx, g, "scrollX",        JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, g, "scrollY",        JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, g, "addEventListener",    JS_NewCFunction(ctx, js_noop, "addEventListener", 2));
    JS_SetPropertyStr(ctx, g, "removeEventListener", JS_NewCFunction(ctx, js_noop, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, g, "dispatchEvent",       JS_NewCFunction(ctx, js_noop, "dispatchEvent", 1));
    JS_SetPropertyStr(ctx, g, "getComputedStyle",    JS_NewCFunction(ctx, js_get_element, "getComputedStyle", 1));
    JS_SetPropertyStr(ctx, g, "matchMedia",          JS_NewCFunction(ctx, js_get_element, "matchMedia", 1));
    JS_SetPropertyStr(ctx, g, "scrollTo",            JS_NewCFunction(ctx, js_noop, "scrollTo", 2));
    JS_SetPropertyStr(ctx, g, "scrollBy",            JS_NewCFunction(ctx, js_noop, "scrollBy", 2));
    JS_SetPropertyStr(ctx, g, "alert",               JS_NewCFunction(ctx, js_console_log, "alert", 1));
    JS_SetPropertyStr(ctx, g, "confirm",             JS_NewCFunction(ctx, js_noop_null, "confirm", 1));
    JS_SetPropertyStr(ctx, g, "prompt",              JS_NewCFunction(ctx, js_noop_null, "prompt", 1));
    JS_SetPropertyStr(ctx, g, "localStorage",        JS_NULL);
    JS_SetPropertyStr(ctx, g, "sessionStorage",      JS_NULL);
    JS_SetPropertyStr(ctx, g, "crypto",              JS_NewObject(ctx));
    JS_SetPropertyStr(ctx, g, "performance",         JS_NewObject(ctx));

    /* document */
    JSValue doc = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, doc, "readyState",   JS_NewString(ctx, "complete"));
    JS_SetPropertyStr(ctx, doc, "title",        JS_NewString(ctx, g_browser ? g_browser->page_title : ""));
    JS_SetPropertyStr(ctx, doc, "URL",          JS_NewString(ctx, cur_url));
    JS_SetPropertyStr(ctx, doc, "domain",       JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, doc, "cookie",       JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, doc, "referrer",     JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, doc, "charset",      JS_NewString(ctx, "UTF-8"));
    JS_SetPropertyStr(ctx, doc, "write",        JS_NewCFunction(ctx, js_doc_write, "write",   1));
    JS_SetPropertyStr(ctx, doc, "writeln",      JS_NewCFunction(ctx, js_doc_write, "writeln", 1));
    JS_SetPropertyStr(ctx, doc, "getElementById",
        JS_NewCFunction(ctx, js_get_element,   "getElementById",    1));
    JS_SetPropertyStr(ctx, doc, "querySelector",
        JS_NewCFunction(ctx, js_get_element,   "querySelector",     1));
    JS_SetPropertyStr(ctx, doc, "querySelectorAll",
        JS_NewCFunction(ctx, js_noop_null,     "querySelectorAll",  1));
    JS_SetPropertyStr(ctx, doc, "createElement",
        JS_NewCFunction(ctx, js_get_element,   "createElement",     1));
    JS_SetPropertyStr(ctx, doc, "createTextNode",
        JS_NewCFunction(ctx, js_get_element,   "createTextNode",    1));
    JS_SetPropertyStr(ctx, doc, "createDocumentFragment",
        JS_NewCFunction(ctx, js_get_element,   "createDocumentFragment", 0));
    JS_SetPropertyStr(ctx, doc, "addEventListener",
        JS_NewCFunction(ctx, js_noop,          "addEventListener",  2));
    JS_SetPropertyStr(ctx, doc, "removeEventListener",
        JS_NewCFunction(ctx, js_noop,          "removeEventListener", 2));
    JS_SetPropertyStr(ctx, doc, "dispatchEvent",
        JS_NewCFunction(ctx, js_noop,          "dispatchEvent",     1));
    JS_SetPropertyStr(ctx, doc, "body",            make_element(ctx));
    JS_SetPropertyStr(ctx, doc, "head",            make_element(ctx));
    JS_SetPropertyStr(ctx, doc, "documentElement", make_element(ctx));
    JS_SetPropertyStr(ctx, g, "document", doc);

    /* browser (custom API) */
    JSValue bobj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, bobj, "navigate",  JS_NewCFunction(ctx, js_navigate, "navigate", 1));
    JS_SetPropertyStr(ctx, g, "browser", bobj);

    JS_FreeValue(ctx, g);
}

/* =========================================================================
 * Worker thread
 * ========================================================================= */

static void js_worker(void *arg)
{
    (void)arg;

    g_rt = JS_NewRuntime();
    if (!g_rt) { g_thread_running = false; return; }
    JS_SetMemoryLimit(g_rt, JS_HEAP_LIMIT);
    JS_SetMaxStackSize(g_rt, JS_STACK_LIMIT);

    g_ctx = JS_NewContext(g_rt);
    if (!g_ctx) { JS_FreeRuntime(g_rt); g_rt = NULL; g_thread_running = false; return; }

    js_std_add_helpers(g_ctx, 0, NULL);
    register_globals(g_ctx);

    printf("[js] worker ready\n");

    while (!g_exit_requested) {
        LightEvent_Wait(&g_req_event);
        if (g_exit_requested) break;

        if (g_job.type == JOB_RUN_SCRIPTS) {
            /* ── Extract & run inline <script> blocks ─────────────────── */
            BrowserCtx *b = g_job.browser;
            g_browser = b;
            g_doc_len = 0;
            g_doc_buf[0] = '\0';

            /* Update location/document with current page info */
            JSValue gg  = JS_GetGlobalObject(g_ctx);
            JSValue loc = JS_GetPropertyStr(g_ctx, gg, "location");
            JSValue doc = JS_GetPropertyStr(g_ctx, gg, "document");
            JS_SetPropertyStr(g_ctx, loc, "href",  JS_NewString(g_ctx, b->current_url));
            JS_SetPropertyStr(g_ctx, doc, "title", JS_NewString(g_ctx, b->page_title));
            JS_SetPropertyStr(g_ctx, doc, "URL",   JS_NewString(g_ctx, b->current_url));
            JS_FreeValue(g_ctx, doc);
            JS_FreeValue(g_ctx, loc);
            JS_FreeValue(g_ctx, gg);

            if (!b->raw_html) { LightEvent_Signal(&g_done_event); continue; }

            const char *p = b->raw_html;
            int scripts_run = 0;

            while ((p = strstr(p, "<script")) != NULL && scripts_run < JS_MAX_SCRIPTS) {
                const char *tag_end = strchr(p, '>');
                if (!tag_end) break;
                bool has_src = (strstr(p, "src=") != NULL &&
                                strstr(p, "src=") < tag_end);
                p = tag_end + 1;
                if (has_src) continue;

                const char *close = strstr(p, "</script>");
                if (!close) break;
                size_t slen = (size_t)(close - p);
                if (slen == 0 || slen > JS_MAX_SCRIPT_LEN) { p = close+9; continue; }

                char *script = (char *)malloc(slen + 1);
                if (!script) { p = close+9; continue; }
                memcpy(script, p, slen);
                script[slen] = '\0';

                char sname[32];
                snprintf(sname, sizeof(sname), "<script[%d]>", scripts_run);
                JSValue val = JS_Eval(g_ctx, script, slen, sname, JS_EVAL_TYPE_GLOBAL);
                if (JS_IsException(val)) {
                    JSValue exc = JS_GetException(g_ctx);
                    const char *msg = JS_ToCString(g_ctx, exc);
                    browser_debug(b, "JS[%d] %.50s", scripts_run, msg ? msg : "?");
                    if (msg) JS_FreeCString(g_ctx, msg);
                    JS_FreeValue(g_ctx, exc);
                }
                JS_FreeValue(g_ctx, val);
                free(script);
                scripts_run++;
                p = close + 9;
            }

            browser_debug(b, "Ran %d scripts doc.write=%zu", scripts_run, g_doc_len);

            /* Append document.write() output to page_text */
            if (g_doc_len > 0 && b->page_text) {
                size_t orig = strlen(b->page_text);
                char *nt = (char *)realloc(b->page_text, orig + g_doc_len + 2);
                if (nt) {
                    b->page_text = nt;
                    bool in_t = false;
                    for (size_t i = 0; i < g_doc_len; i++) {
                        char c = g_doc_buf[i];
                        if (c == '<') { in_t = true; continue; }
                        if (c == '>') { in_t = false; nt[orig++] = ' '; continue; }
                        if (!in_t) nt[orig++] = c;
                    }
                    nt[orig] = '\0';
                }
            }
            g_job.output[0] = '\0';

        } else if (g_job.type == JOB_EVAL) {
            /* ── REPL eval ─────────────────────────────────────────────── */
            g_log_len = 0; g_log_buf[0] = '\0';
            JSValue val = JS_Eval(g_ctx, g_job.input, strlen(g_job.input),
                                  "<console>", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(val)) {
                JSValue exc = JS_GetException(g_ctx);
                const char *msg = JS_ToCString(g_ctx, exc);
                log_append("[exception] ", msg ? msg : "?");
                if (msg) JS_FreeCString(g_ctx, msg);
                JS_FreeValue(g_ctx, exc);
            } else if (!JS_IsUndefined(val)) {
                const char *res = JS_ToCString(g_ctx, val);
                if (res) { log_append("= ", res); JS_FreeCString(g_ctx, res); }
            }
            JS_FreeValue(g_ctx, val);
            snprintf(g_job.output, sizeof(g_job.output), "%s",
                     g_log_len > 0 ? g_log_buf : "(no output)\n");
            g_log_len = 0; g_log_buf[0] = '\0';
        }

        g_job.type = JOB_NONE;
        LightEvent_Signal(&g_done_event);
    }

    JS_FreeContext(g_ctx); g_ctx = NULL;
    JS_FreeRuntime(g_rt);  g_rt  = NULL;
    g_thread_running = false;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void js_init(void)
{
    LightEvent_Init(&g_req_event,  RESET_ONESHOT);
    LightEvent_Init(&g_done_event, RESET_ONESHOT);
    g_job.type = JOB_NONE;
    g_thread_running = true;
    g_exit_requested = false;

    /* Priority 0x30 = low, below main thread (0x30 is user-space normal).
     * -1 for cpu_id = default core.                                       */
    /* libctru signature: Thread threadCreate(entrypoint, arg, stack, prio, cpu, detached) */
    g_thread = threadCreate(js_worker, NULL, JS_THREAD_STACK, 0x30, -1, false);
    if (g_thread == NULL) {
        printf("[js] threadCreate failed\n");
        g_thread_running = false;
        return;
    }
    /* Give the worker a moment to initialise QuickJS */
    svcSleepThread(50000000LL); /* 50 ms */
    printf("[js] thread started\n");
}

void js_exit(void)
{
    if (!g_thread_running) return;
    g_exit_requested = true;
    LightEvent_Signal(&g_req_event);
    threadJoin(g_thread, U64_MAX);
    threadFree(g_thread);
    g_thread_running = false;
}

bool js_is_ready(void) { return g_thread_running && g_ctx != NULL; }

void js_run_page_scripts(BrowserCtx *b)
{
    if (!g_thread_running || !b || !b->raw_html) return;

    g_job.type    = JOB_RUN_SCRIPTS;
    g_job.browser = b;
    LightEvent_Signal(&g_req_event);
    /* Wait up to 5 seconds for scripts to finish */
    LightEvent_WaitTimeout(&g_done_event, 5000000000ULL);
}

void js_eval_in_page(BrowserCtx *b, const char *input,
                     char *out_buf, int out_sz)
{
    if (!out_buf || out_sz <= 0) return;
    if (!g_thread_running) {
        snprintf(out_buf, out_sz, "JS engine not ready.\n");
        return;
    }

    g_browser = b;
    g_job.type = JOB_EVAL;
    snprintf(g_job.input, sizeof(g_job.input), "%s", input);
    g_job.output[0] = '\0';

    LightEvent_Signal(&g_req_event);
    LightEvent_WaitTimeout(&g_done_event, 5000000000ULL);

    snprintf(out_buf, out_sz, "%s", g_job.output[0] ? g_job.output : "(no output)\n");
}

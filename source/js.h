/*
 * js.h - QuickJS wrapper for FoxSearch
 */
#pragma once
#include <stddef.h>
#include <stdbool.h>

#define JS_MAX_OUTPUT  (8 * 1024)   /* capture up to 8KB of console output */
#define JS_MAX_SCRIPTS 32

typedef struct {
    char   console_log[JS_MAX_OUTPUT];  /* captured console.log output */
    size_t log_len;
    int    error_count;
    char   last_error[512];
} JsResult;

/* One-time init / exit */
void js_init(void);
void js_exit(void);

/* Run one or more scripts in a fresh context.
   scripts[]  = array of JS source strings
   count      = number of scripts
   result     = output (caller provides) */
bool js_run(const char** scripts, int count, JsResult* result);

/* Run a single expression and return its string value */
bool js_eval_string(const char* expr, char* out, size_t out_sz);

/*
 * net.c - HTTP/HTTPS fetching via libctru httpc service
 *
 * Download pattern copied from the official devkitPro 3ds-examples:
 * https://github.com/devkitPro/3ds-examples/blob/master/network/http/source/main.c
 *
 * Key rules from libctru docs:
 *  - httpcInit(0) is correct for GET (sharedmem only needed for POST)
 *  - The *entire* content must be downloaded before httpcCloseContext()
 *    otherwise httpcCloseContext() will hang
 *  - Use realloc-growing loop: start 4KB, grow by 4KB each DOWNLOADPENDING
 */
#include <3ds.h>
#include <3ds/services/soc.h>
#include <3ds/services/sslc.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "net.h"

#define SOC_BUFFER_SIZE (256 * 1024)
#define DL_CHUNK        0x1000   /* 4 KB — same as official example */

static u32  *g_soc_buf     = NULL;
static bool  g_soc_ready   = false;
static bool  g_ssl_ready   = false;
static bool  g_httpc_ready = false;

void net_init(void)
{
    Result rc;

    /* Step 0: AC — must be initialised first to bring up the Wi-Fi interface */
    rc = acInit();
    if (R_SUCCEEDED(rc)) printf("[net] AC ready\n");
    else printf("[net] acInit: 0x%08lX (non-fatal)\n", rc);

    /* Step 1: SOC */
    g_soc_buf = (u32 *)memalign(0x1000, SOC_BUFFER_SIZE);
    if (g_soc_buf) {
        rc = socInit(g_soc_buf, SOC_BUFFER_SIZE);
        if (R_SUCCEEDED(rc)) {
            g_soc_ready = true;
            printf("[net] SOC ready\n");
        } else {
            printf("[net] socInit: 0x%08lX\n", rc);
            free(g_soc_buf); g_soc_buf = NULL;
        }
    }

    /* Step 2: SSL */
    rc = sslcInit(0);
    if (R_SUCCEEDED(rc)) { g_ssl_ready = true; printf("[net] SSL ready\n"); }
    else printf("[net] sslcInit: 0x%08lX (non-fatal)\n", rc);

    /* Step 3: httpc — sharedmem=0 is correct for GET requests */
    rc = httpcInit(0);
    if (R_SUCCEEDED(rc)) { g_httpc_ready = true; printf("[net] httpc ready\n"); }
    else printf("[net] httpcInit: 0x%08lX\n", rc);

}

void net_exit(void)
{
    if (g_httpc_ready) { httpcExit();  g_httpc_ready = false; }
    if (g_ssl_ready)   { sslcExit();   g_ssl_ready   = false; }
    if (g_soc_ready)   { socExit();    g_soc_ready   = false; }
    if (g_soc_buf)     { free(g_soc_buf); g_soc_buf  = NULL;  }
    acExit();  /* AC last */
}

/*
 * net_fetch_once — single request, no redirect following.
 * Returns the httpc Result in *out_rc for diagnostics.
 */
static NetResult net_fetch_once(const char *url, NetResponse *resp, Result *out_rc)
{
    httpcContext ctx;
    Result rc;
    bool is_https = (strncmp(url, "https://", 8) == 0);

    *out_rc = 0;

    rc = httpcOpenContext(&ctx, HTTPC_METHOD_GET, url, 0);
    if (R_FAILED(rc)) { *out_rc = rc; return NET_ERR_OPEN; }

    httpcAddRequestHeaderField(&ctx, "User-Agent",
        "Mozilla/5.0 (Nintendo 3DS; CFW) FoxSearch/0.1");
    httpcAddRequestHeaderField(&ctx, "Accept",
        "text/html,text/plain,*/*;q=0.9");
    httpcAddRequestHeaderField(&ctx, "Accept-Language", "en-US,en;q=0.5");
    httpcAddRequestHeaderField(&ctx, "Connection", "close");

    if (is_https) {
        /* Disable cert verification — the handshake will still use TLS
         * but skip the certificate chain check                          */
        httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify);

        /* SSLCOPT_DisableVerify skips cert chain validation.
         * httpcRootCertChainCreate is not available in this libctru version.
         * For sites that fail TLS handshake (0xD8A0A03C), use http:// instead. */
    }

    rc = httpcBeginRequest(&ctx);
    if (R_FAILED(rc)) {
        *out_rc = rc;
        httpcCancelConnection(&ctx);
        httpcCloseContext(&ctx);
        return NET_ERR_REQUEST;
    }

    u32 statusCode = 0;
    rc = httpcGetResponseStatusCode(&ctx, &statusCode);
    if (R_FAILED(rc)) {
        *out_rc = rc;
        httpcCancelConnection(&ctx);
        httpcCloseContext(&ctx);
        return NET_ERR_DOWNLOAD;
    }
    resp->status = (int)statusCode;

    httpcGetResponseHeader(&ctx, "Content-Type",
                           resp->content_type, sizeof(resp->content_type) - 1);
    if (statusCode >= 300 && statusCode < 400)
        httpcGetResponseHeader(&ctx, "Location",
                               resp->final_url, NET_MAX_URL - 1);

    /* ── Download body using the official realloc-growing pattern ─── */
    u8    *buf      = (u8 *)malloc(DL_CHUNK);
    if (!buf) { httpcCloseContext(&ctx); return NET_ERR_NOMEM; }
    u8    *lastbuf  = NULL;
    u32    size     = 0;
    u32    readsize = 0;

    do {
        rc = httpcDownloadData(&ctx, buf + size, DL_CHUNK, &readsize);
        size += readsize;

        if (rc == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) {
            if (size >= NET_MAX_BODY) break;  /* cap at max body size */
            lastbuf = buf;
            buf = (u8 *)realloc(buf, size + DL_CHUNK);
            if (!buf) {
                free(lastbuf);
                httpcCloseContext(&ctx);
                return NET_ERR_NOMEM;
            }
        }
    } while (rc == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING);

    /* rc == 0 means done; anything else is an error */
    if (rc != 0 && size == 0) {
        *out_rc = rc;
        free(buf);
        httpcCloseContext(&ctx);
        return NET_ERR_DOWNLOAD;
    }

    /* Shrink to actual size and NUL-terminate */
    lastbuf = buf;
    buf = (u8 *)realloc(buf, size + 1);
    if (!buf) buf = lastbuf;
    buf[size] = '\0';

    resp->body     = (char *)buf;
    resp->body_len = size;
    strncpy(resp->final_url, url, NET_MAX_URL - 1);

    httpcCloseContext(&ctx);
    return NET_OK;
}

NetResult net_fetch(const char *url, NetResponse *resp)
{
    if (!resp) return NET_ERR_INIT;
    memset(resp, 0, sizeof(*resp));

    char cur_url[NET_MAX_URL];
    strncpy(cur_url, url, NET_MAX_URL - 1);

    for (int redirects = 0; redirects < 5; redirects++) {
        Result httpc_rc = 0;
        NetResult nr = net_fetch_once(cur_url, resp, &httpc_rc);

        if (nr != NET_OK) {
            /* Store the raw httpc result code in final_url for debug */
            snprintf(resp->final_url, NET_MAX_URL,
                     "[httpc_rc=0x%08lX url=%s]", httpc_rc, cur_url);
            return nr;
        }

        /* Follow redirect */
        if (resp->status >= 300 && resp->status < 400 && resp->final_url[0]) {
            char next[NET_MAX_URL];
            strncpy(next, resp->final_url, NET_MAX_URL - 1);
            net_response_free(resp);
            strncpy(cur_url, next, NET_MAX_URL - 1);
            continue;
        }

        /* If no body was downloaded (3xx without Location, etc.) that's ok */
        strncpy(resp->final_url, cur_url, NET_MAX_URL - 1);
        return NET_OK;
    }

    return NET_ERR_OPEN; /* too many redirects */
}

void net_response_free(NetResponse *resp)
{
    if (resp && resp->body) {
        free(resp->body);
        resp->body = NULL;
        resp->body_len = 0;
    }
}

const char *net_error_str(NetResult r)
{
    switch (r) {
        case NET_OK:           return "OK";
        case NET_ERR_INIT:     return "Init failed";
        case NET_ERR_OPEN:     return "Could not open URL";
        case NET_ERR_REQUEST:  return "Request failed";
        case NET_ERR_STATUS:   return "Bad HTTP status";
        case NET_ERR_DOWNLOAD: return "Download error";
        case NET_ERR_TOOBIG:   return "Page too large";
        case NET_ERR_NOMEM:    return "Out of memory";
        default:               return "Unknown error";
    }
}

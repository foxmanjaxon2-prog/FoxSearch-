/*
 * net.h - HTTP/HTTPS fetching via libctru httpc service
 *
 * HTTPS initialisation order (handled inside net_init):
 *   1. socInit()   — raw socket layer (WiFi must be connected in HOME menu)
 *   2. sslcInit()  — 3DS SSL/TLS service
 *   3. httpcInit() — HTTP(S) client; TLS handshake is transparent for https://
 */
#pragma once
#include <stddef.h>

#define NET_MAX_URL   512
#define NET_MAX_BODY  (512 * 1024)

typedef enum {
    NET_OK = 0,
    NET_ERR_INIT,
    NET_ERR_OPEN,
    NET_ERR_REQUEST,
    NET_ERR_STATUS,
    NET_ERR_DOWNLOAD,
    NET_ERR_TOOBIG,
    NET_ERR_NOMEM,
} NetResult;

typedef struct {
    int    status;
    char   content_type[128];
    char  *body;               /* heap-allocated; call net_response_free()  */
    size_t body_len;
    char   final_url[NET_MAX_URL];
} NetResponse;

/* Initialise full network stack (SOC -> SSL -> httpc). Call once at start. */
void        net_init(void);
void        net_exit(void);

/* Synchronous GET. Fills *resp on NET_OK; caller must net_response_free(). */
NetResult   net_fetch(const char *url, NetResponse *resp);
void        net_response_free(NetResponse *resp);
const char *net_error_str(NetResult r);

#pragma once
#include <stdbool.h>
#define BM_MAX       64
#define BM_URL_LEN   512
#define BM_TITLE_LEN 128
#define BM_FILE      "/3ds/FoxSearch/bookmarks.txt"
#define HIST_FILE    "/3ds/FoxSearch/history.txt"
#define HIST_MAX     64

typedef struct { char url[BM_URL_LEN]; char title[BM_TITLE_LEN]; } Bookmark;

extern Bookmark bm_list[BM_MAX];
extern int      bm_count;
extern Bookmark hist_list[HIST_MAX];
extern int      hist_count;

void bm_load(void);
void bm_save(void);
bool bm_add(const char* url, const char* title);
bool bm_remove(int idx);
bool bm_exists(const char* url);

void hist_load(void);
void hist_save(void);
void hist_push(const char* url, const char* title);

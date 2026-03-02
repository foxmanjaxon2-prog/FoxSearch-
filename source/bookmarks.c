#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "bookmarks.h"

Bookmark bm_list[BM_MAX];   int bm_count   = 0;
Bookmark hist_list[HIST_MAX]; int hist_count = 0;

static void ensure_dir(void) {
    mkdir("/3ds", 0777);
    mkdir("/3ds/FoxSearch", 0777);
}

void bm_load(void) {
    ensure_dir();
    bm_count = 0;
    FILE* f = fopen(BM_FILE, "r");
    if (!f) return;
    char line[BM_URL_LEN + BM_TITLE_LEN + 4];
    while (bm_count < BM_MAX && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        char* tab = strchr(line, '\t');
        if (tab) {
            *tab = '\0';
            strncpy(bm_list[bm_count].url,   line,    BM_URL_LEN - 1);
            strncpy(bm_list[bm_count].title, tab + 1, BM_TITLE_LEN - 1);
        } else {
            strncpy(bm_list[bm_count].url, line, BM_URL_LEN - 1);
            strncpy(bm_list[bm_count].title, line, BM_TITLE_LEN - 1);
        }
        bm_count++;
    }
    fclose(f);
}

void bm_save(void) {
    ensure_dir();
    FILE* f = fopen(BM_FILE, "w");
    if (!f) return;
    for (int i = 0; i < bm_count; i++)
        fprintf(f, "%s\t%s\n", bm_list[i].url, bm_list[i].title);
    fclose(f);
}

bool bm_add(const char* url, const char* title) {
    if (bm_count >= BM_MAX || bm_exists(url)) return false;
    strncpy(bm_list[bm_count].url,   url,   BM_URL_LEN - 1);
    strncpy(bm_list[bm_count].title, title ? title : url, BM_TITLE_LEN - 1);
    bm_count++;
    bm_save();
    return true;
}

bool bm_remove(int idx) {
    if (idx < 0 || idx >= bm_count) return false;
    memmove(&bm_list[idx], &bm_list[idx+1],
            (size_t)(bm_count - idx - 1) * sizeof(Bookmark));
    bm_count--;
    bm_save();
    return true;
}

bool bm_exists(const char* url) {
    for (int i = 0; i < bm_count; i++)
        if (strcmp(bm_list[i].url, url) == 0) return true;
    return false;
}

void hist_load(void) {
    ensure_dir();
    hist_count = 0;
    FILE* f = fopen(HIST_FILE, "r");
    if (!f) return;
    char line[BM_URL_LEN + BM_TITLE_LEN + 4];
    while (hist_count < HIST_MAX && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        char* tab = strchr(line, '\t');
        if (tab) { *tab = '\0';
            strncpy(hist_list[hist_count].url,   line,    BM_URL_LEN - 1);
            strncpy(hist_list[hist_count].title, tab + 1, BM_TITLE_LEN - 1);
        } else {
            strncpy(hist_list[hist_count].url, line, BM_URL_LEN - 1);
        }
        hist_count++;
    }
    fclose(f);
}

void hist_save(void) {
    ensure_dir();
    FILE* f = fopen(HIST_FILE, "w");
    if (!f) return;
    for (int i = 0; i < hist_count; i++)
        fprintf(f, "%s\t%s\n", hist_list[i].url, hist_list[i].title);
    fclose(f);
}

void hist_push(const char* url, const char* title) {
    /* Remove duplicate if exists */
    for (int i = 0; i < hist_count; i++) {
        if (strcmp(hist_list[i].url, url) == 0) {
            memmove(&hist_list[i], &hist_list[i+1],
                    (size_t)(hist_count - i - 1) * sizeof(Bookmark));
            hist_count--;
            break;
        }
    }
    /* Shift and prepend */
    if (hist_count >= HIST_MAX) hist_count = HIST_MAX - 1;
    memmove(&hist_list[1], &hist_list[0],
            (size_t)hist_count * sizeof(Bookmark));
    strncpy(hist_list[0].url,   url,   BM_URL_LEN - 1);
    strncpy(hist_list[0].title, title ? title : url, BM_TITLE_LEN - 1);
    hist_count++;
    hist_save();
}

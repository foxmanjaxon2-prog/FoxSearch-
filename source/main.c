/*
 * FoxSearch – 3DS Homebrew Browser with QuickJS JavaScript Engine
 */
#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <sys/stat.h>
#include "browser.h"
#include "js_engine.h"
#include "net.h"

#define TOP_W       400
#define BOT_W       320
#define SCR_H       240
#define TOP_CX      200.0f
#define BOT_CX      160.0f
#define MAX_SPRITES 2048

#define RGBA8(r,g,b,a) ((u32)(r)|((u32)(g)<<8)|((u32)(b)<<16)|((u32)(a)<<24))
#define COL_BG     RGBA8(15,  15,  25,  255)
#define COL_BAR    RGBA8(22,  22,  40,  255)
#define COL_CARD   RGBA8(30,  30,  55,  255)
#define COL_BORDER RGBA8(50,  50,  100, 255)
#define COL_ACCENT RGBA8(255, 120,  0,  255)
#define COL_BLUE   RGBA8(80,  160, 255, 255)
#define COL_TEXT   RGBA8(230, 230, 230, 255)
#define COL_MUTED  RGBA8(130, 130, 160, 255)
#define COL_GREEN  RGBA8(80,  220, 120, 255)
#define COL_RED    RGBA8(220,  60,  60, 255)
#define COL_LINK   RGBA8(100, 180, 255, 255)

typedef enum {
    STATE_HOME, STATE_LOADING, STATE_PAGE,
    STATE_BOOKMARKS, STATE_HISTORY, STATE_JS_CONSOLE,
    STATE_DEBUG,
} AppState;

static AppState          gState  = STATE_HOME;
static C3D_RenderTarget* gTop    = NULL;
static C3D_RenderTarget* gBot    = NULL;
static C2D_TextBuf       gBuf    = NULL;
static C2D_Font          gFont   = NULL;
static BrowserCtx        gBrowser;

/* ── Draw helpers ──────────────────────────────────────────────────────── */
static void rect(float x,float y,float w,float h,u32 c){
    C2D_DrawRectSolid(x,y,0.5f,w,h,c);
}
static void txt(float x,float y,float s,u32 c,const char*str){
    if(!str||!*str)return;
    C2D_Text t;
    C2D_TextFontParse(&t,gFont,gBuf,str);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t,C2D_WithColor,x,y,0.6f,s,s,c);
}
static void txtf(float x,float y,float s,u32 c,const char*fmt,...){
    char b[512]; va_list a; va_start(a,fmt);
    vsnprintf(b,sizeof(b),fmt,a); va_end(a);
    txt(x,y,s,c,b);
}
static void txtc(float cx,float y,float s,u32 c,const char*str){
    if(!str||!*str)return;
    C2D_Text t; float w=0,h=0;
    C2D_TextFontParse(&t,gFont,gBuf,str);
    C2D_TextOptimize(&t);
    C2D_TextGetDimensions(&t,s,s,&w,&h);
    C2D_DrawText(&t,C2D_WithColor,cx-w/2,y,0.6f,s,s,c);
}
static void txtmc(float x,float y,float maxy,float s,u32 c,float lh,const char*text){
    if(!text)return;
    char line[256]; int li=0;
    for(const char*p=text;;p++){
        if(*p=='\n'||*p=='\0'){
            line[li]='\0';
            if(li>0&&y<maxy)txt(x,y,s,c,line);
            y+=lh; li=0;
            if(*p=='\0')break;
        } else if(li<255) line[li++]=*p;
    }
}

/* ── URL bar ───────────────────────────────────────────────────────────── */
static void drawURLBar(const char*url,bool loading){
    float bY=SCR_H-26;
    rect(0,bY,BOT_W,26,COL_BAR);
    rect(0,bY,BOT_W,1,COL_BORDER);
    u32 dot=loading?COL_ACCENT:COL_GREEN;
    C2D_DrawCircleSolid(10,bY+13,0.5f,5,dot);
    char d[64];
    snprintf(d,sizeof(d),"%.55s",url?url:"");
    txt(20,bY+7,0.38f,loading?COL_ACCENT:COL_MUTED,d);
}

/* ── HOME ──────────────────────────────────────────────────────────────── */
static void drawHomeTop(void){
    rect(0,0,TOP_W,SCR_H,COL_BG);
    rect(0,0,TOP_W,38,COL_BAR);
    rect(0,38,TOP_W,2,COL_ACCENT);
    txt(14,7,0.75f,COL_ACCENT,"Fox"); txt(68,7,0.75f,COL_TEXT,"Search");
    txtc(TOP_CX,50,0.40f,COL_MUTED,"A real 3DS browser with JavaScript");
    rect(40,68,TOP_W-80,26,COL_CARD);
    rect(40,68,TOP_W-80,1,COL_BORDER);
    txtc(TOP_CX,75,0.42f,COL_MUTED,"Press [A] to enter URL or search");
    const char*ql[]={"Wikipedia","en.m.wikipedia.org",
                     "HackerNews","news.ycombinator.com",
                     "GBAtemp","gbatemp.net",
                     "Example","example.com"};
    txt(14,108,0.40f,COL_MUTED,"Quick Links:");
    for(int i=0;i<4;i++){
        float ry=122+i*18;
        rect(14,ry-1,TOP_W-28,16,COL_CARD);
        txt(20,ry,0.40f,COL_LINK,ql[i*2]);
        txt(180,ry,0.36f,COL_MUTED,ql[i*2+1]);
    }
    rect(0,SCR_H-18,TOP_W,18,COL_BAR);
    txt(10,SCR_H-14,0.36f,COL_MUTED,
        "[A] Navigate  [Y] Bookmarks  [X] History  [SELECT] JS Console");
}
static void drawHomeBot(void){
    rect(0,0,BOT_W,SCR_H,COL_BG);
    rect(10,10,BOT_W-20,44,COL_CARD);
    rect(10,10,BOT_W-20,2,COL_ACCENT);
    txtc(BOT_CX,14,0.42f,COL_ACCENT,"JavaScript Engine");
    bool ok=js_is_ready();
    txtc(BOT_CX,30,0.38f,ok?COL_GREEN:COL_RED,
         ok?"QuickJS Ready":"JS Not Available");
    rect(10,62,BOT_W-20,26,COL_CARD);
    txtc(BOT_CX,68,0.38f,COL_MUTED,"HTTP: libctru httpc");
    rect(10,96,BOT_W-20,60,COL_CARD);
    txt(16,100,0.38f,COL_TEXT,"Bookmarks:");
    txtf(130,100,0.38f,COL_ACCENT,"%d",gBrowser.bookmark_count);
    txt(16,116,0.38f,COL_TEXT,"History:");
    txtf(130,116,0.38f,COL_ACCENT,"%d",gBrowser.history_count);
    txt(16,132,0.38f,COL_TEXT,"JS Execs:");
    txtf(130,132,0.38f,COL_ACCENT,"%d",gBrowser.js_exec_count);
    drawURLBar(NULL,false);
}

/* ── LOADING ────────────────────────────────────────────────────────────── */
static int gAnim=0;
static void drawLoadingTop(void){
    rect(0,0,TOP_W,SCR_H,COL_BG);
    rect(0,0,TOP_W,38,COL_BAR);
    rect(0,38,TOP_W,2,COL_ACCENT);
    txt(14,7,0.75f,COL_ACCENT,"Fox"); txt(68,7,0.75f,COL_TEXT,"Search");
    const char*fr[]={"-","\\","|","/"};
    gAnim=(gAnim+1)%20;
    txtc(TOP_CX,80,0.60f,COL_ACCENT,"Loading...");
    txtc(TOP_CX,108,0.80f,COL_TEXT,fr[gAnim/5]);
    char d[60]; snprintf(d,sizeof(d),"%.57s",gBrowser.current_url);
    txtc(TOP_CX,140,0.36f,COL_MUTED,d);
    float p=gBrowser.load_progress;
    rect(40,168,TOP_W-80,8,COL_CARD);
    rect(40,168,(TOP_W-80)*p,8,COL_ACCENT);
}
static void drawLoadingBot(void){
    rect(0,0,BOT_W,SCR_H,COL_BG);
    txtc(BOT_CX,80,0.42f,COL_MUTED,"Fetching page...");
    txtc(BOT_CX,100,0.38f,COL_MUTED,"Press [B] to cancel");
    drawURLBar(gBrowser.current_url,true);
}

/* ── PAGE ───────────────────────────────────────────────────────────────── */
static void drawPageTop(void){
    rect(0,0,TOP_W,SCR_H,COL_BG);
    rect(0,0,TOP_W,22,COL_BAR);
    rect(0,22,TOP_W,1,COL_BORDER);
    char title[56]; snprintf(title,sizeof(title),"%.53s",
        gBrowser.page_title[0]?gBrowser.page_title:gBrowser.current_url);
    txt(6,4,0.40f,COL_TEXT,title);
    int ms=browser_max_scroll(&gBrowser,SCR_H-23);
    if(ms>0){
        float pct=(float)gBrowser.scroll_y/ms;
        float bh=((SCR_H-23.0f)/(gBrowser.rendered_height+1))*(SCR_H-23);
        if(bh<20)bh=20;
        float by=23+pct*(SCR_H-23-bh);
        rect(TOP_W-4,23,4,SCR_H-23,COL_BAR);
        rect(TOP_W-4,by,4,bh,COL_ACCENT);
    }
    browser_draw(&gBrowser,gFont,gBuf,6,23,TOP_W-10,SCR_H-23,
                 gBrowser.scroll_y,0.38f,13.0f);
}
static void drawPageBot(void){
    rect(0,0,BOT_W,SCR_H,COL_BG);
    rect(0,0,BOT_W,22,COL_BAR);
    rect(0,22,BOT_W,1,COL_BORDER);
    bool cb=(gBrowser.history_pos>0);
    txt( 8,4,0.40f,cb?COL_TEXT:COL_MUTED,"[B] Back");
    txt(90,4,0.40f,COL_MUTED,"[L] Fwd");
    txt(150,4,0.40f,COL_ACCENT,"[A] URL");
    txt(218,4,0.40f,COL_MUTED,"[R] Reload");
    int lc=browser_link_count(&gBrowser);
    if(lc>0){
        txtc(BOT_CX,28,0.38f,COL_MUTED,"Links:");
        int show=lc>7?7:lc;
        for(int i=0;i<show;i++){
            const char*lt=browser_link_text(&gBrowser,i);
            float ry=40+i*18;
            bool sel=(i==gBrowser.selected_link);
            if(sel){rect(4,ry-1,BOT_W-8,16,COL_CARD);rect(4,ry-1,3,16,COL_ACCENT);}
            char tr[40]; snprintf(tr,sizeof(tr),"%.38s",lt?lt:"");
            txt(12,ry,0.36f,sel?COL_LINK:COL_MUTED,tr);
        }
        if(lc>7)txtc(BOT_CX,172,0.34f,COL_MUTED,"[L/R] scroll links");
    } else {
        rect(10,80,BOT_W-20,50,COL_CARD);
        txtc(BOT_CX,86,0.38f,COL_MUTED,"[SELECT] Debug Log");
        txtc(BOT_CX,104,0.36f,COL_MUTED,"[Y] Bookmark this page");
    }
    drawURLBar(gBrowser.current_url,false);
}

/* ── BOOKMARKS ──────────────────────────────────────────────────────────── */
static int gBkSel=0;
static void drawBookmarksTop(void){
    rect(0,0,TOP_W,SCR_H,COL_BG);
    rect(0,0,TOP_W,24,COL_BAR); rect(0,24,TOP_W,1,COL_ACCENT);
    txtc(TOP_CX,5,0.50f,COL_ACCENT,"Bookmarks");
    if(!gBrowser.bookmark_count){
        txtc(TOP_CX,110,0.44f,COL_MUTED,"No bookmarks yet.");
        txtc(TOP_CX,130,0.40f,COL_MUTED,"Press [Y] on a page to add.");
        return;
    }
    int start=gBkSel>=8?gBkSel-7:0;
    for(int i=0;i<8&&(i+start)<gBrowser.bookmark_count;i++){
        int idx=i+start; float ry=30+i*24; bool sel=(idx==gBkSel);
        if(sel){rect(4,ry,TOP_W-8,22,COL_CARD);rect(4,ry,3,22,COL_ACCENT);}
        char t[42],u[42];
        snprintf(t,sizeof(t),"%.40s",gBrowser.bookmarks[idx].title);
        snprintf(u,sizeof(u),"%.40s",gBrowser.bookmarks[idx].url);
        txt(12,ry+2, 0.40f,sel?COL_TEXT:COL_MUTED,t);
        txt(12,ry+13,0.33f,COL_BLUE,u);
    }
}
static void drawBookmarksBot(void){
    rect(0,0,BOT_W,SCR_H,COL_BG);
    rect(0,0,BOT_W,22,COL_BAR);
    txt(8,4,0.40f,COL_TEXT,"[A] Open  [X] Delete  [B] Back");
    if(gBkSel<gBrowser.bookmark_count){
        rect(10,30,BOT_W-20,60,COL_CARD);
        char t[40],u[40];
        snprintf(t,sizeof(t),"%.38s",gBrowser.bookmarks[gBkSel].title);
        snprintf(u,sizeof(u),"%.38s",gBrowser.bookmarks[gBkSel].url);
        txt(16,34,0.40f,COL_ACCENT,"Selected:");
        txt(16,50,0.40f,COL_TEXT,t);
        txt(16,66,0.36f,COL_BLUE,u);
    }
    txtc(BOT_CX,180,0.36f,COL_MUTED,"[UP/DN] Navigate");
}

/* ── HISTORY ────────────────────────────────────────────────────────────── */
static int gHiSel=0;
static void drawHistoryTop(void){
    rect(0,0,TOP_W,SCR_H,COL_BG);
    rect(0,0,TOP_W,24,COL_BAR); rect(0,24,TOP_W,1,COL_ACCENT);
    txtc(TOP_CX,5,0.50f,COL_ACCENT,"History");
    if(!gBrowser.history_count){
        txtc(TOP_CX,110,0.44f,COL_MUTED,"No history yet."); return;
    }
    int start=gHiSel>=8?gHiSel-7:0;
    for(int i=0;i<8&&(i+start)<gBrowser.history_count;i++){
        int idx=i+start; float ry=30+i*24; bool sel=(idx==gHiSel);
        if(sel){rect(4,ry,TOP_W-8,22,COL_CARD);rect(4,ry,3,22,COL_ACCENT);}
        char u[52]; snprintf(u,sizeof(u),"%.50s",gBrowser.history[idx]);
        txt(12,ry+5,0.38f,sel?COL_LINK:COL_MUTED,u);
    }
}
static void drawHistoryBot(void){
    rect(0,0,BOT_W,SCR_H,COL_BG);
    rect(0,0,BOT_W,22,COL_BAR);
    txt(8,4,0.40f,COL_TEXT,"[A] Open  [X] Clear All  [B] Back");
    txtc(BOT_CX,180,0.36f,COL_MUTED,"[UP/DN] Navigate");
}

/* ── JS CONSOLE ─────────────────────────────────────────────────────────── */
static char gJsIn[256]  = "";
static char gJsOut[512] = "Type JS to run on current page.\n";

static void drawJSTop(void){
    rect(0,0,TOP_W,SCR_H,COL_BG);
    rect(0,0,TOP_W,22,COL_BAR); rect(0,22,TOP_W,1,COL_ACCENT);
    txt(8,4,0.46f,COL_ACCENT,"> JS Console");
    txt(TOP_W-90,4,0.36f,COL_MUTED,js_is_ready()?"[QuickJS]":"[NO JS]");
    rect(4,26,TOP_W-8,SCR_H-48,COL_CARD);
    txtmc(10,30,SCR_H-26,0.37f,COL_GREEN,13.0f,gJsOut);
}
static void drawJSBot(void){
    rect(0,0,BOT_W,SCR_H,COL_BG);
    rect(0,0,BOT_W,22,COL_BAR);
    txt(8,4,0.36f,COL_MUTED,"[A] Type  [B] Back  [X] Clear");
    rect(4,26,BOT_W-8,20,COL_CARD); rect(4,26,BOT_W-8,1,COL_ACCENT);
    char d[58]; snprintf(d,sizeof(d),"> %.52s_",gJsIn);
    txt(8,29,0.38f,COL_TEXT,d);
    if(gJsOut[0]){
        rect(4,50,BOT_W-8,BOT_W-56,COL_CARD);
        txtmc(8,54,200,0.36f,COL_GREEN,13.0f,gJsOut);
    }
}


/* ── DEBUG LOG ──────────────────────────────────────────────────────────── */
static void drawDebugTop(void){
    rect(0,0,TOP_W,SCR_H,COL_BG);
    rect(0,0,TOP_W,22,COL_BAR); rect(0,22,TOP_W,1,COL_ACCENT);
    txtc(TOP_CX,4,0.46f,COL_ACCENT,"Debug Log");
    int total=gBrowser.debug_count;
    int start=(total>BROWSER_DEBUG_LINES)?(total%BROWSER_DEBUG_LINES):0;
    int show=(total<BROWSER_DEBUG_LINES)?total:BROWSER_DEBUG_LINES;
    for(int i=0;i<show;i++){
        int idx=(start+i)%BROWSER_DEBUG_LINES;
        txtf(4,26+i*13,0.36f,COL_GREEN,"%s",gBrowser.debug_log[idx]);
    }
    if(total==0) txtc(TOP_CX,110,0.40f,COL_MUTED,"No debug messages yet.");
}
static void drawDebugBot(void){
    rect(0,0,BOT_W,SCR_H,COL_BG);
    rect(0,0,BOT_W,22,COL_BAR);
    txtc(BOT_CX,4,0.40f,COL_MUTED,"[B] Back  [X] Clear log");
    /* Stats row */
    rect(4,26,BOT_W-8,30,COL_CARD);
    txtf(8,28,0.36f,COL_TEXT,"HTTP:%d  bytes:%zu  txt:%zu  lnk:%d",
         gBrowser.raw_status,
         gBrowser.raw_bytes,
         gBrowser.page_text?strlen(gBrowser.page_text):0,
         gBrowser.link_count);
    /* Raw body preview */
    if(gBrowser.raw_preview[0]){
        rect(4,60,BOT_W-8,155,COL_CARD);
        rect(4,60,BOT_W-8,1,COL_ACCENT);
        txt(8,62,0.34f,COL_MUTED,"Raw body preview:");
        /* Split preview into ~37-char lines */
        const char *p=gBrowser.raw_preview;
        int ry=74; int left=(int)strlen(p);
        while(left>0&&ry<210){
            char seg[38]={0};
            int take=left>37?37:left;
            memcpy(seg,p,take);
            txt(8,ry,0.33f,COL_GREEN,seg);
            p+=take; left-=take; ry+=12;
        }
    } else {
        txtc(BOT_CX,100,0.38f,COL_MUTED,"No response yet.");
    }
}

/* ── Navigation ─────────────────────────────────────────────────────────── */
static void showKbd(const char*hint,char*out,int maxLen){
    SwkbdState sw;
    swkbdInit(&sw,SWKBD_TYPE_NORMAL,2,maxLen-1);
    swkbdSetHintText(&sw,hint);
    swkbdSetValidation(&sw,SWKBD_ANYTHING,0,0);
    swkbdInputText(&sw,out,maxLen);
}

static void navTo(const char*url){
    char full[512];
    if(strncmp(url,"http://",7)&&strncmp(url,"https://",8)){
        if(strchr(url,'.')){
            snprintf(full,sizeof(full),"https://%s",url);
        } else {
            char enc[384]; int ei=0;
            for(int i=0;url[i]&&ei<380;i++)
                enc[ei++]=(url[i]==' ')?'+':url[i];
            enc[ei]='\0';
            snprintf(full,sizeof(full),
                "https://html.duckduckgo.com/html/?q=%s",enc);
        }
    } else snprintf(full,sizeof(full),"%s",url);
    browser_navigate(&gBrowser,full);
    gState=STATE_LOADING;
}

/* ── Input ──────────────────────────────────────────────────────────────── */
static void inputHome(u32 kd){
    if(kd&KEY_A){char u[512]="";showKbd("URL or search",u,sizeof(u));if(u[0])navTo(u);}
    if(kd&KEY_Y)gState=STATE_BOOKMARKS;
    if(kd&KEY_X)gState=STATE_HISTORY;
    if(kd&KEY_SELECT)gState=STATE_JS_CONSOLE;
}
static void inputLoading(u32 kd){
    if(kd&KEY_B){browser_cancel(&gBrowser);gState=STATE_HOME;}
    BrowserLoadStatus s=browser_poll(&gBrowser);
    if(s==BLOAD_DONE){
        js_run_page_scripts(&gBrowser);
        gState=STATE_PAGE;
    } else if(s==BLOAD_ERROR) gState=STATE_PAGE;
}
static void inputPage(u32 kd,u32 kh){
    if(kd&KEY_A){
        const char*hr=browser_selected_href(&gBrowser);
        if(hr&&hr[0]) navTo(hr);
        else {char u[512]="";showKbd("URL or search",u,sizeof(u));if(u[0])navTo(u);}
    }
    if(kd&KEY_B){if(!browser_back(&gBrowser))gState=STATE_HOME;}
    if(kd&KEY_L)browser_forward(&gBrowser);
    if(kd&KEY_R){browser_reload(&gBrowser);gState=STATE_LOADING;}
    if(kd&KEY_Y)browser_add_bookmark(&gBrowser,gBrowser.page_title,gBrowser.current_url);
    if(kd&KEY_X)gState=STATE_HISTORY;
    if(kd&KEY_SELECT)gState=STATE_DEBUG;
    int sp=(kh&KEY_R)?30:8;
    if(kh&KEY_UP)  browser_scroll(&gBrowser,-sp);
    if(kh&KEY_DOWN)browser_scroll(&gBrowser, sp);
    if(kd&KEY_DLEFT) {gBrowser.selected_link--;if(gBrowser.selected_link<0)gBrowser.selected_link=0;}
    if(kd&KEY_DRIGHT){gBrowser.selected_link++;int lc=browser_link_count(&gBrowser);if(gBrowser.selected_link>=lc&&lc>0)gBrowser.selected_link=lc-1;}
}
static void inputBookmarks(u32 kd){
    if(kd&KEY_UP&&gBkSel>0)gBkSel--;
    if(kd&KEY_DOWN&&gBkSel<gBrowser.bookmark_count-1)gBkSel++;
    if(kd&KEY_A&&gBrowser.bookmark_count>0)navTo(gBrowser.bookmarks[gBkSel].url);
    if(kd&KEY_X&&gBrowser.bookmark_count>0){
        browser_delete_bookmark(&gBrowser,gBkSel);
        if(gBkSel>=gBrowser.bookmark_count&&gBkSel>0)gBkSel--;
    }
    if(kd&KEY_B)gState=STATE_HOME;
}
static void inputHistory(u32 kd){
    if(kd&KEY_UP&&gHiSel>0)gHiSel--;
    if(kd&KEY_DOWN&&gHiSel<gBrowser.history_count-1)gHiSel++;
    if(kd&KEY_A&&gBrowser.history_count>0)navTo(gBrowser.history[gHiSel]);
    if(kd&KEY_X)browser_clear_history(&gBrowser);
    if(kd&KEY_B)gState=STATE_HOME;
}
static void inputDebug(u32 kd){
    if(kd&KEY_B)gState=(gBrowser.current_url[0]?STATE_PAGE:STATE_HOME);
    if(kd&KEY_X){memset(gBrowser.debug_log,0,sizeof(gBrowser.debug_log));gBrowser.debug_count=0;}
}
static void inputJS(u32 kd){
    if(kd&KEY_A){
        showKbd("Enter JavaScript",gJsIn,sizeof(gJsIn));
        if(gJsIn[0]){
            js_eval_in_page(&gBrowser,gJsIn,gJsOut,sizeof(gJsOut));
            gBrowser.js_exec_count++;
        }
    }
    if(kd&KEY_X){memset(gJsIn,0,sizeof(gJsIn));snprintf(gJsOut,sizeof(gJsOut),"Cleared.\n");}
    if(kd&KEY_B)gState=(gBrowser.current_url[0]?STATE_PAGE:STATE_HOME);
}

/* ── Main ───────────────────────────────────────────────────────────────── */
int main(void){
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(MAX_SPRITES);
    C2D_Prepare();
    gTop=C2D_CreateScreenTarget(GFX_TOP,GFX_LEFT);
    gBot=C2D_CreateScreenTarget(GFX_BOTTOM,GFX_LEFT);
    gFont=C2D_FontLoadSystem(CFG_REGION_USA);
    gBuf=C2D_TextBufNew(8192);

    browser_init(&gBrowser);
    js_init();
    net_init();   /* SOC -> SSL -> httpc in correct order */

    while(aptMainLoop()){
        hidScanInput();
        u32 kd=hidKeysDown(), kh=hidKeysHeld();
        if(kd&KEY_START){if(gState!=STATE_HOME)gState=STATE_HOME;else break;}

        switch(gState){
            case STATE_HOME:       inputHome(kd);        break;
            case STATE_LOADING:    inputLoading(kd);     break;
            case STATE_PAGE:       inputPage(kd,kh);     break;
            case STATE_BOOKMARKS:  inputBookmarks(kd);   break;
            case STATE_HISTORY:    inputHistory(kd);     break;
            case STATE_JS_CONSOLE: inputJS(kd);          break;
            case STATE_DEBUG:      inputDebug(kd);     break;
        }

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TextBufClear(gBuf);

        C2D_TargetClear(gTop,COL_BG); C2D_SceneBegin(gTop);
        switch(gState){
            case STATE_HOME:       drawHomeTop();       break;
            case STATE_LOADING:    drawLoadingTop();    break;
            case STATE_PAGE:       drawPageTop();       break;
            case STATE_BOOKMARKS:  drawBookmarksTop();  break;
            case STATE_HISTORY:    drawHistoryTop();    break;
            case STATE_JS_CONSOLE: drawJSTop();         break;
            case STATE_DEBUG:      drawDebugTop();     break;
        }

        C2D_TargetClear(gBot,COL_BG); C2D_SceneBegin(gBot);
        switch(gState){
            case STATE_HOME:       drawHomeBot();       break;
            case STATE_LOADING:    drawLoadingBot();    break;
            case STATE_PAGE:       drawPageBot();       break;
            case STATE_BOOKMARKS:  drawBookmarksBot();  break;
            case STATE_HISTORY:    drawHistoryBot();    break;
            case STATE_JS_CONSOLE: drawJSBot();         break;
            case STATE_DEBUG:      drawDebugBot();     break;
        }
        C3D_FrameEnd(0);
    }

    js_exit();
    browser_exit(&gBrowser);
    net_exit();   /* shuts down httpc -> SSL -> SOC */
    C2D_FontFree(gFont);
    C2D_TextBufDelete(gBuf);
    C2D_Fini(); C3D_Fini(); gfxExit();
    return 0;
}

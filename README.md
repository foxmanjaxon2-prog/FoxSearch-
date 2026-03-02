# FoxSearch 
### A real 3DS browser with a JavaScript engine

FoxSearch is a homebrew browser for the Nintendo 3DS featuring:
- **QuickJS** — a full ES2020 JavaScript engine (by Fabrice Bellard)
- **HTTP/HTTPS** fetching via libctru's httpc service
- **HTML parser** that extracts text, links, and `<script>` tags
- **JS Console** — type and run JavaScript on any page
- **Bookmarks** saved to SD card
- **History** with back/forward navigation
- **Dual-screen UI** — content on top, controls + links on bottom

---

## Building

### Prerequisites
- devkitPro with `3ds-dev` and `3ds-citro2d` installed
- git, gcc, g++, make

### Steps

```bash
# 1. Clone QuickJS into the source directory (required!)
git clone --depth 1 https://github.com/bellard/quickjs.git source/quickjs

# 2. Build the CIA
make cia
```

The Makefile will:
1. Generate the icon and banner via Python
2. Compile QuickJS + FoxSearch with devkitARM
3. Build makerom from source (works on aarch64, x86_64, etc.)
4. Pack everything into FoxSearch.cia

### Install
Copy `FoxSearch.cia` to your SD card and install with FBI.

---

## Controls

| Button | Action |
|--------|--------|
| A | Enter URL / follow selected link |
| B | Back / cancel |
| L | Forward |
| R | Reload / fast scroll |
| Y | Bookmark current page |
| X | History |
| SELECT | JS Console |
| UP/DOWN | Scroll page |
| D-Left/Right | Select link |
| START | Home screen |

---

## JavaScript Support

FoxSearch includes **QuickJS**, a complete ES2020 interpreter. It runs page
scripts automatically after load, and you can open the JS Console (SELECT) to
run arbitrary JavaScript on any page.

**Available browser APIs:**
- `console.log/warn/error/info`
- `document.title`, `document.URL`
- `document.getElementById/querySelector/querySelectorAll`
- `document.createElement`, `document.addEventListener` (stub)
- `alert()`, `confirm()` (confirm always returns false)
- `setTimeout/clearTimeout` (stubs — no event loop on 3DS)
- `navigator.userAgent/platform/language/onLine`
- `window.location.href/hostname/protocol/pathname`

**Memory limit:** 4MB for the JS heap (to protect 3DS RAM)

Most simple scripts work. React/Vue/Angular will not — they require a real
DOM and event loop. Wikipedia's reader mode, basic news sites, and text-only
pages work well.

---

## Architecture

```
source/
  main.c          UI, input, state machine
  browser.c/h     HTTP fetch (threaded), HTML stripper, page renderer
  js_engine.c/h   QuickJS wrapper + browser DOM stubs
  net.c/h         Low-level httpc helpers
  bookmarks.c/h   Bookmark persistence
  html.c/h        Additional HTML utilities
  quickjs/        QuickJS source (clone separately — see Building above)
```

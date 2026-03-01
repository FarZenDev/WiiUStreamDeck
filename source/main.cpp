// =============================================================================
// WiiU Stream Deck  –  Final Version
// Rendering : OSScreenClearBufferEx + OSScreenPutFontEx ONLY (no tearing)
// Touch    : VPAD, coords NOT inverted (Y=0 at top)
// IP entry : nn::swkbd system keyboard
// =============================================================================

#include <whb/proc.h>
#include <whb/log.h>
#include <whb/log_udp.h>
#include <vpad/input.h>
#include <coreinit/thread.h>
#include <coreinit/mutex.h>
#include <coreinit/cache.h>
#include <coreinit/screen.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <malloc.h>
#include <curl/curl.h>
#include "json.hpp"
#include "font.h"
#include <tuple>

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <cstdlib>
#include <cmath>

static const int SCR_W    = 854;
static const int SCR_H    = 480;
static const int CHAR_W   = 14;
static const int CHAR_H   = 16;
static const int COLS     = SCR_W / CHAR_W;  // 61
static const int ROWS     = SCR_H / CHAR_H;  // 30

static SDL_Window*   gWindow   = nullptr;
static SDL_Renderer* gRenderer = nullptr;
static TTF_Font*     gFont     = nullptr;
static void*         s_screenBuf = nullptr;

static void screenFlip();

static bool sdlInit() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) return false;
    if (TTF_Init() == -1) return false;
    
    gWindow = SDL_CreateWindow("Stream Deck", 0, 0, SCR_W, SCR_H, 0);
    if (!gWindow) return false;
    gRenderer = SDL_CreateRenderer(gWindow, -1, SDL_RENDERER_ACCELERATED); 
    if (!gRenderer) return false;
    SDL_SetRenderDrawBlendMode(gRenderer, SDL_BLENDMODE_BLEND);

    // Initialisation fallback OSScreen pour la console si besoin (logs errors)
    OSScreenInit();
    size_t sz = OSScreenGetBufferSizeEx(SCREEN_DRC);
    s_screenBuf = memalign(0x100, sz);
    OSScreenSetBufferEx(SCREEN_DRC, s_screenBuf);
    OSScreenEnableEx(SCREEN_DRC, 1);
    
    // Clear legacy buffers
    OSScreenClearBufferEx(SCREEN_DRC, 0);
    DCFlushRange(s_screenBuf, sz);
    OSScreenFlipBuffersEx(SCREEN_DRC);
    
    SDL_RWops* rw = SDL_RWFromConstMem(font_ttf, font_ttf_len);
    gFont = TTF_OpenFontRW(rw, 1, 22);
    if (!gFont) {
        WHBLogPrintf("TTF_OpenFontRW failed: %s", TTF_GetError());
    }

    WHBLogPrintf("Using 100%% SDL2 Rendering with SDL2_ttf.");
    return true;
}

static void sdlShutdown() {
    if (gFont) TTF_CloseFont(gFont);
    TTF_Quit();
    if (gRenderer) SDL_DestroyRenderer(gRenderer);
    if (gWindow) SDL_DestroyWindow(gWindow);
    SDL_Quit();
    // buffer free
    if (s_screenBuf) {
        free(s_screenBuf);
        s_screenBuf = nullptr;
    }
}

static void renderText(int x, int y, const char* str, SDL_Color color, int align = 0) {
    if(!gFont) return;
    SDL_Surface* srf = TTF_RenderUTF8_Blended(gFont, str, color);
    if(!srf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(gRenderer, srf);
    if(tex) {
        SDL_Rect dst = {x, y, srf->w, srf->h};
        if (align == 1) dst.x -= dst.w / 2;      // Center
        else if (align == 2) dst.x -= dst.w;     // Right
        SDL_RenderCopy(gRenderer, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_FreeSurface(srf);
}

// ===================================================================
// CONFIG
// ===================================================================
static const char* CFG_PATH = "sd:/wiiu/apps/WiiUStreamDeck/config.ini";

struct Layout { int cols, rows; const char* name; };
static const Layout LAYOUTS[] = {
    {3,2,"3x2"},{4,3,"4x3"},{5,3,"5x3"},{6,4,"6x4"},{7,5,"7x5"}
};
static const int LAYOUT_N = 5;

struct Cfg {
    std::string host       = "192.168.1.50";
    int         port       = 8080;
    int         layoutIdx  = 2;
    bool        showLabels = true;
};
static Cfg cfg;

// ===================================================================
// DATA MODEL
// ===================================================================
struct Button { std::string id,label,color; int row,col,actionType; std::string actionValue; };
struct Page   { std::string id,name;  int rows,cols; std::vector<Button> buttons; };

enum State { S_LOADING, S_PAGELIST, S_GRID, S_SETTINGS };
static State             gState    = S_SETTINGS;
static std::vector<Page> gPages;
static Page              gPage;
static int               gSettRow  = 0;
static std::string       gStatus;
static OSMutex           gPagesMutex;
static int               gPageSel  = 0;   // selected page in list

static void saveCfg() {
    FILE* f = fopen(CFG_PATH,"w");
    if (!f) return;
    fprintf(f,"host=%s\nport=%d\nlayout=%d\nlabels=%d\n",
            cfg.host.c_str(),cfg.port,cfg.layoutIdx,(int)cfg.showLabels);
    fclose(f);
}
static void loadCfg() {
    FILE* f = fopen(CFG_PATH,"r");
    if (!f) return;
    char ln[256];
    while (fgets(ln,sizeof(ln),f)) {
        std::string s(ln);
        s.erase(std::remove(s.begin(),s.end(),'\n'),s.end());
        s.erase(std::remove(s.begin(),s.end(),'\r'),s.end());
        auto eq=s.find('=');
        if (eq==std::string::npos) continue;
        std::string k=s.substr(0,eq),v=s.substr(eq+1);
        if      (k=="host")   cfg.host=v;
        else if (k=="port")   cfg.port=std::atoi(v.c_str());
        else if (k=="layout") cfg.layoutIdx=std::clamp(std::atoi(v.c_str()),0,LAYOUT_N-1);
        else if (k=="labels") cfg.showLabels=std::atoi(v.c_str())!=0;
    }
    fclose(f);
}

// ===================================================================
// HTTP
// ===================================================================
static std::string baseUrl() {
    char b[80]; snprintf(b,sizeof(b),"http://%s:%d",cfg.host.c_str(),cfg.port);
    return b;
}
struct CBuf { std::string data; };
static size_t curlCb(void* p,size_t s,size_t n,void* ud){
    static_cast<CBuf*>(ud)->data.append(static_cast<char*>(p),s*n);
    return s*n;
}
static std::string httpGet(const std::string& url) {
    CBuf buf;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL* c=curl_easy_init(); if(!c){curl_global_cleanup();return "";}
    curl_easy_setopt(c,CURLOPT_URL,url.c_str());
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,curlCb);
    curl_easy_setopt(c,CURLOPT_WRITEDATA,&buf);
    curl_easy_setopt(c,CURLOPT_TIMEOUT,5L);
    curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,4L);
    curl_easy_perform(c); curl_easy_cleanup(c);
    curl_global_cleanup(); return buf.data;
}
static void httpPost(const std::string& url,const std::string& body) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL* c=curl_easy_init(); if(!c){curl_global_cleanup();return;}
    curl_slist* h=curl_slist_append(nullptr,"Content-Type: application/json");
    curl_easy_setopt(c,CURLOPT_URL,url.c_str());
    curl_easy_setopt(c,CURLOPT_HTTPHEADER,h);
    curl_easy_setopt(c,CURLOPT_POSTFIELDS,body.c_str());
    curl_easy_setopt(c,CURLOPT_TIMEOUT,5L);
    curl_easy_perform(c); curl_slist_free_all(h); curl_easy_cleanup(c);
    curl_global_cleanup();
}

static void syncLayoutToPC() {
    if (gPages.empty()) return;
    if (gPageSel < 0 || gPageSel >= (int)gPages.size()) return;
    std::string pid = gPages[gPageSel].id;
    int rows = LAYOUTS[cfg.layoutIdx].rows;
    int cols = LAYOUTS[cfg.layoutIdx].cols;
    std::string url = baseUrl() + "/api/layout";
    std::string body = "{\"pageId\":\"" + pid + "\",\"rows\":" + std::to_string(rows) + ",\"cols\":" + std::to_string(cols) + "}";
    httpPost(url, body);
}

// ===================================================================
// JSON helpers (nlohmann)
// ===================================================================
static Page parsePage(const std::string& jstr){
    Page pg;
    try {
        auto j = nlohmann::json::parse(jstr);
        pg.id   = j.value("id", "");
        pg.name = j.value("name", "");
        pg.rows = j.value("rows", 0);
        if(pg.rows <= 0) pg.rows = LAYOUTS[cfg.layoutIdx].rows;
        pg.cols = j.value("cols", 0);
        if(pg.cols <= 0) pg.cols = LAYOUTS[cfg.layoutIdx].cols;

        if (j.contains("buttons") && j["buttons"].is_array()) {
            for (auto& bo : j["buttons"]) {
                Button b;
                b.id         = bo.value("id", "");
                b.label      = bo.value("label", "");
                b.color      = bo.value("color", "#333333");
                b.row        = bo.value("row", 0);
                b.col        = bo.value("col", 0);
                
                // Action is nested
                if (bo.contains("action") && bo["action"].is_object()) {
                    b.actionType = bo["action"].value("type", 0);
                    b.actionValue = bo["action"].value("value", "");
                } else {
                    b.actionType = 0;
                }

                if (!b.id.empty()) {
                    pg.buttons.push_back(b);
                }
            }
        }
    } catch(...) {
        WHBLogPrintf("JSON Parse Error in parsePage!");
    }
    return pg;
}

static std::vector<Page> parsePages(const std::string& jstr) {
    std::vector<Page> v;
    try {
        auto jarr = nlohmann::json::parse(jstr);
        if (jarr.is_array()) {
            for (auto& o : jarr) {
                Page pg;
                pg.id   = o.value("id", "");
                pg.name = o.value("name", "");
                if (!pg.id.empty()) {
                    v.push_back(pg);
                }
            }
        }
    } catch(...) {
        WHBLogPrintf("JSON Parse Error in parsePages!");
    }
    return v;
}


// ===================================================================
// GRAPHICS PRIMITIVES (SDL2 Texture Cache & Render Geometry)
// ===================================================================
static inline int distSq(int dx, int dy) { return dx*dx + dy*dy; }

struct RRTexKey {
    int w, h, radius;
    uint32_t topCol, botCol;
    bool operator<(const RRTexKey& o) const {
        return std::tie(w, h, radius, topCol, botCol) < std::tie(o.w, o.h, o.radius, o.topCol, o.botCol);
    }
};
static std::map<RRTexKey, SDL_Texture*> gTexCache;

static SDL_Texture* getRRTexture(int w, int h, int radius, uint32_t topColor, uint32_t botColor) {
    if (w<=0 || h<=0) return nullptr;
    RRTexKey key = {w, h, radius, topColor, botColor};
    auto it = gTexCache.find(key);
    if (it != gTexCache.end()) return it->second;

    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if(!s) return nullptr;
    
    uint8_t r1 = topColor>>24, g1 = (topColor>>16)&0xFF, b1 = (topColor>>8)&0xFF, a1 = topColor&0xFF;
    uint8_t r2 = botColor>>24, g2 = (botColor>>16)&0xFF, b2 = (botColor>>8)&0xFF, a2 = botColor&0xFF;
    int rsq = radius * radius;
    
    SDL_LockSurface(s);
    uint32_t* pixels = (uint32_t*)s->pixels;
    int pitch = s->pitch / 4;
    
    uint32_t transp = SDL_MapRGBA(s->format, 0, 0, 0, 0);

    for(int iy=0; iy<h; ++iy) {
        float t = (float)iy / (float)std::max(1, h-1);
        uint8_t cr = r1 + (r2-r1)*t;
        uint8_t cg = g1 + (g2-g1)*t;
        uint8_t cb = b1 + (b2-b1)*t;
        uint8_t ca = a1 + (a2-a1)*t;
        uint32_t color = SDL_MapRGBA(s->format, cr, cg, cb, ca);
        
        for(int ix=0; ix<w; ++ix) {
            bool inside = true;
            if(ix < radius && iy < radius) {
                if(distSq(radius-ix, radius-iy) >= rsq) inside = false;
            } else if(ix > w-1-radius && iy < radius) {
                if(distSq(ix-(w-1-radius), radius-iy) >= rsq) inside = false;
            } else if(ix < radius && iy > h-1-radius) {
                if(distSq(radius-ix, iy-(h-1-radius)) >= rsq) inside = false;
            } else if(ix > w-1-radius && iy > h-1-radius) {
                if(distSq(ix-(w-1-radius), iy-(h-1-radius)) >= rsq) inside = false;
            }
            pixels[iy * pitch + ix] = inside ? color : transp;
        }
    }
    SDL_UnlockSurface(s);
    SDL_Texture* tex = SDL_CreateTextureFromSurface(gRenderer, s);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(s);
    
    gTexCache[key] = tex;
    return tex;
}

static void gradientRoundRect(int x, int y, int w, int h, int radius, uint32_t topColor, uint32_t botColor) {
    SDL_Texture* tex = getRRTexture(w, h, radius, topColor, botColor);
    if(tex) {
        SDL_Rect r = {x, y, w, h};
        SDL_RenderCopy(gRenderer, tex, nullptr, &r);
    }
}
static void fillRoundRect(int x, int y, int w, int h, int radius, uint32_t color) {
    gradientRoundRect(x, y, w, h, radius, color, color);
}

static void fillRect(int x, int y, int w, int h, uint32_t color) {
    uint8_t r = color>>24, g = (color>>16)&0xFF, b = (color>>8)&0xFF, a = color&0xFF;
    SDL_SetRenderDrawColor(gRenderer, r, g, b, a);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderFillRect(gRenderer, &rect);
}

static uint32_t parseHexColor(const std::string& hex, uint32_t fallback = 0x555555FF) {
    if (hex.length() == 7 && hex[0] == '#') {
        int r, g, b;
        if (std::sscanf(hex.c_str() + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
            return (r << 24) | (g << 16) | (b << 8) | 0xFF;
        }
    }
    return fallback;
}

// ===================================================================
// DRAW – 100% SDL2 HB App Store Rendering Engine
// ===================================================================

static void screenFlip() {
    SDL_RenderPresent(gRenderer);
}

// Helpers mapping character grid to pixels using SDL_ttf
static void T(int col, int row, const char* s, SDL_Color color = {255, 255, 255, 255}, int align = 0) {
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    renderText(col * CHAR_W, row * CHAR_H, s, color, align);
}
static void Tf(int cx, int cy, const char* fmt, ...) {
    char buf[256]; va_list a; va_start(a,fmt); vsnprintf(buf,sizeof(buf),fmt,a); va_end(a);
    T(cx, cy, buf);
}

static void drawAppBackground() {
    // Solid dark navy background matching the image
    fillRect(0, 0, SCR_W, SCR_H, 0x0A0F1DFF);
}

static void drawHeader(const char* title) {
    drawAppBackground();
    
    // Header gradient (dark blue to navy)
    gradientRoundRect(0, 0, SCR_W, 40, 0, 0x141E30FF, 0x0A0F1DFF);
    
    // Cyan separator line
    fillRect(0, 40, SCR_W, 2, 0x00E5FFFF);
    
    char fullTitle[128];
    if (title && strlen(title) > 0) {
        snprintf(fullTitle, sizeof(fullTitle), "Stream Deck | %s", title);
    } else {
        snprintf(fullTitle, sizeof(fullTitle), "Stream Deck");
    }
    
    // Centered White Text
    renderText(SCR_W / 2, 7, fullTitle, {255, 255, 255, 255}, 1);
}

static void drawFooter() {
    // Only Centered IP Text, no background box or line
    char ipText[64];
    snprintf(ipText, sizeof(ipText), "IP: %s:%d", cfg.host.c_str(), cfg.port);
    renderText(SCR_W / 2, SCR_H - 26, ipText, {180, 190, 200, 255}, 1);
}

static void drawLoading() {
    drawAppBackground();
    renderText(SCR_W/2, SCR_H/2, ">>> Connexion au PC... <<<", {200, 220, 255, 255}, 1);
    screenFlip();
}

// ===================================================================
// IP EDITOR  –  D-pad character-by-character IP entry
// UP/DOWN = change char, LEFT/RIGHT = move cursor, A=OK, B=Cancel
// ===================================================================
static std::string openIPEditor(VPADStatus& vpad, VPADReadError& verr) {
    std::string ip = cfg.host;
    // Pad to 15 chars for easy indexing
    while (ip.size() < 15) ip += ' ';
    ip.resize(15);
    int cursor = 0;
    // Valid chars: digits 0-9, '.' and backspace
    const char CHARS[] = "0123456789. ";
    const int  NCHARS  = 12;

    auto findChar = [&](char c) -> int {
        for (int i = 0; i < NCHARS; ++i) if (CHARS[i]==c) return i;
        return 0;
    };

    OSTime lastBtn = 0;
    bool done = false;

    while (!done && WHBProcIsRunning()) {
        VPADRead(VPAD_CHAN_0, &vpad, 1, &verr);
        if (verr == VPAD_READ_SUCCESS) {
            OSTime now = OSGetTime();
            bool rep = vpad.trigger || ((uint64_t)(now-lastBtn)>OSMillisecondsToTicks(200) && vpad.hold);
            if (rep) {
                lastBtn = now;
                if (vpad.trigger & VPAD_BUTTON_B) { return ""; }  // Cancel
                if (vpad.trigger & VPAD_BUTTON_A) {               // OK
                    // Trim ALL spaces from the string so valid shorter IPs don't have gaps
                    ip.erase(std::remove(ip.begin(), ip.end(), ' '), ip.end());
                    
                    // Also ensure we trim any trailing garbage if any
                    auto endPos = ip.find_last_not_of(" \t\r\n");
                    if (endPos != std::string::npos) ip = ip.substr(0, endPos + 1);
                    else ip = "";
                    
                    return ip;
                }
                if (vpad.hold & VPAD_BUTTON_LEFT)  cursor = std::max(0, cursor-1);
                if (vpad.hold & VPAD_BUTTON_RIGHT) cursor = std::min(14, cursor+1);
                if (vpad.hold & VPAD_BUTTON_UP) {
                    int ci = findChar(ip[cursor]);
                    ip[cursor] = CHARS[(ci+1)%NCHARS];
                }
                if (vpad.hold & VPAD_BUTTON_DOWN) {
                    int ci = findChar(ip[cursor]);
                    ip[cursor] = CHARS[(ci+NCHARS-1)%NCHARS];
                }
            }
        }

        // Draw the editor
        drawAppBackground();
        fillRoundRect(SCR_W/2 - 300, 40, 600, 400, 15, 0x1A2534FF);
        
        renderText(SCR_W/2, 60, "Entree de l'IP (haut/bas=changer, g/d=deplacer)", {200, 200, 220, 255}, 1);
        renderText(SCR_W/2, 90, "A = Valider          B = Annuler", {120, 130, 150, 255}, 1);
        fillRect(SCR_W/2 - 280, 130, 560, 2, 0x24334AFF);
        
        renderText(SCR_W/2 - 250, 180, "IP:", {255, 255, 255, 255});
        
        // Show IP with cursor marker
        char display[32];
        snprintf(display, sizeof(display), " %s", ip.c_str());
        renderText(SCR_W/2 - 200, 180, display, {0, 255, 255, 255});
        
        // Arrow under cursor
        char arrow[32] = {};
        memset(arrow, ' ', 1 + cursor);
        arrow[1+cursor] = '^';
        renderText(SCR_W/2 - 200, 205, arrow, {0, 200, 255, 255});
        
        fillRect(SCR_W/2 - 280, 260, 560, 2, 0x24334AFF);
        renderText(SCR_W/2, 300, "Chars disponibles: 0-9  .  (espace=effacer)", {150, 150, 150, 255}, 1);
        
        screenFlip();
    }
    return "";
}

// ===================================================================
// FETCH (Initial manual load)
// ===================================================================
static void doFetch() {
    gState = S_LOADING;
    drawAppBackground();
    renderText(SCR_W/2, SCR_H/2, "Connexion au PC en cours...", {200, 220, 255, 255}, 1);
    screenFlip();

    std::string body = httpGet(baseUrl()+"/api/pages");
    if (body.empty()){ gStatus="Erreur: PC injoignable"; gState=S_SETTINGS; return; }
    
    std::vector<Page> pgs = parsePages(body);
    if (pgs.empty()){ gStatus="Aucune page trouvee"; gState=S_SETTINGS; return; }
    
    Page pg;
    if (pgs.size()==1){
        pg = parsePage(httpGet(baseUrl()+"/api/page/"+pgs[0].id));
    }

    OSLockMutex(&gPagesMutex);
    gPages = pgs;
    if (gPages.size()==1) {
        gPage = pg;
        gState = S_GRID;
    } else {
        gState = S_PAGELIST;
    }
    gStatus.clear();
    OSUnlockMutex(&gPagesMutex);
}

// ===================================================================
// BACKGROUND POLLING THREAD
// ===================================================================
static OSThread s_pollThread;
static uint8_t s_pollThreadStack[0x8000] __attribute__((aligned(32)));
static bool s_pollRunning = false;

static std::string urlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (char c : value) {
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << std::uppercase << '%' << std::setw(2) << int((unsigned char)c) << std::nouppercase;
        }
    }
    return escaped.str();
}

struct PendingIcon {
    std::string path;
    std::vector<uint8_t> data;
};
static std::vector<PendingIcon> gIconQueue;
static OSMutex gIconMutex;
static std::map<std::string, SDL_Texture*> gAppIcons;
static std::vector<std::string> gRequestedIcons;

static int pollThreadFunc(int argc, const char** argv) {
    std::string lastJson = "";
    while (s_pollRunning) {
        OSSleepTicks(OSMillisecondsToTicks(1500));
        
        // Fast unlocked check
        if (gState == S_LOADING || cfg.host.empty() || cfg.port == 0) continue;
        
        std::string body = httpGet(baseUrl() + "/api/pages");
        if (!body.empty() && body != "[]" && body != lastJson) {
            lastJson = body;
            std::vector<Page> newPages = parsePages(body);
            if (!newPages.empty()) {
                Page newPage;
                int sel = gPageSel;
                if (sel >= 0 && sel < (int)newPages.size()) {
                    newPage = parsePage(httpGet(baseUrl()+"/api/page/"+newPages[sel].id));
                } else if (newPages.size() == 1) {
                    newPage = parsePage(httpGet(baseUrl()+"/api/page/"+newPages[0].id));
                }
                
                OSLockMutex(&gPagesMutex);
                gPages = newPages;
                if (!newPage.id.empty()) gPage = newPage;
                OSUnlockMutex(&gPagesMutex);
            }
        }
        
        // Background Icon Fetching
        std::vector<std::string> toFetch;
        OSLockMutex(&gPagesMutex);
        if (gState == S_GRID) {
            for (auto& btn : gPage.buttons) {
                // ActionType 1 = RunApp
                if (btn.actionType == 1 && !btn.actionValue.empty()) {
                    if (std::find(gRequestedIcons.begin(), gRequestedIcons.end(), btn.actionValue) == gRequestedIcons.end()) {
                        gRequestedIcons.push_back(btn.actionValue);
                        toFetch.push_back(btn.actionValue);
                    }
                }
            }
        }
        OSUnlockMutex(&gPagesMutex);
        
        for (const auto& path : toFetch) {
            std::string iconData = httpGet(baseUrl() + "/api/icon?path=" + urlEncode(path));
            if (iconData.size() > 8) {
                OSLockMutex(&gIconMutex);
                PendingIcon pi;
                pi.path = path;
                pi.data = std::vector<uint8_t>(iconData.begin(), iconData.end());
                gIconQueue.push_back(pi);
                OSUnlockMutex(&gIconMutex);
            }
        }
    }
    return 0;
}

static void initPollThread() {
    OSInitMutex(&gPagesMutex);
    OSInitMutex(&gIconMutex);
    s_pollRunning = true;
    OSCreateThread(&s_pollThread, pollThreadFunc, 0, nullptr, s_pollThreadStack + sizeof(s_pollThreadStack), sizeof(s_pollThreadStack), 16, OS_THREAD_ATTRIB_AFFINITY_ANY);
    OSResumeThread(&s_pollThread);
}
static void shutdownPollThread() {
    s_pollRunning = false;
    OSJoinThread(&s_pollThread, nullptr);
}





static void drawPageList() {
    drawHeader("Choisissez une page");
    T(20, 70, "Touchez ou appuyez A pour selectionner une page:", {200, 200, 220, 255});
    T(20, 100, "(utilisez haut/bas pour naviguer, A pour ouvrir)", {120, 130, 150, 255});
    
    int startY = 140;
    int spacing = 50;

    for (int i=0;i<(int)gPages.size();i++){
        bool sel=(i==gPageSel);
        int py = startY + i * spacing;
        
        if (sel) {
            fillRoundRect(20, py, 814, 40, 8, 0x004A80FF);
            fillRect(20, py + 38, 814, 2, 0x00A0E9FF);
            renderText(35, py+6, ">", {0, 255, 255, 255});
        } else {
            fillRoundRect(20, py, 814, 40, 8, 0x1A2534FF);
            fillRoundRect(22, py+2, 810, 36, 6, 0x0E141CFF);
        }
        
        char lbl[128];
        snprintf(lbl, sizeof(lbl), "%d. %s", i+1, gPages[i].name.c_str());
        renderText(60, py+6, lbl, sel ? SDL_Color{255, 255, 255, 255} : SDL_Color{180, 180, 180, 255});
    }
    drawFooter();
    screenFlip();
}

static std::map<std::string, float> gButtonScales;
static void drawGrid(const std::string& hoveredBtn) {
    (void)hoveredBtn;
    drawHeader(gPage.name.empty()?"Grille":gPage.name.c_str());

    int rows = gPage.rows>0?gPage.rows:LAYOUTS[cfg.layoutIdx].rows;
    int cols = gPage.cols>0?gPage.cols:LAYOUTS[cfg.layoutIdx].cols;

    int usableW = SCR_W - 80;
    int usableH = SCR_H - 42 - 40;
    int gapX = 20;
    int gapY = 20;
    
    // Base Calculation
    int btnW = (usableW - (cols - 1) * gapX) / cols;
    int btnH = (usableH - (rows - 1) * gapY) / rows;
    
    // Max constraints
    if (btnW > 170) btnW = 170;
    if (btnH > 110) btnH = 110;
    
    // ENFORCE PERFECT SQUARE TILES
    // To be perfectly square (W = H), we must clamp both dimensions to the smallest possible value
    int size = std::min(btnW, btnH);
    btnW = size;
    btnH = size;
    
    // Calculate new total grid dimensions with the square constraints
    int totalGridW = cols * btnW + (cols - 1) * gapX;
    int totalGridH = rows * btnH + (rows - 1) * gapY;
    
    // Perfectly center the new grid onto the screen
    int startX = (SCR_W - totalGridW) / 2;
    int startY = 42 + ((SCR_H - 42 - 40) - totalGridH) / 2;
    
    int cornerRad = 8;
    
    for (int r=0;r<rows;r++){
        for (int c=0;c<cols;c++){
            Button* btn=nullptr;
            for (auto& b:gPage.buttons) if(b.row==r&&b.col==c){btn=&b;break;}

            int px = startX + c * (btnW + gapX);
            int py = startY + r * (btnH + gapY);
            int cw = btnW;
            int ch = btnH;
        
                    float curScale = 1.0f;
                    if (btn) {
                        float target = (btn->id == hoveredBtn) ? 1.08f : 1.0f;
                        if (gButtonScales.find(btn->id) == gButtonScales.end()) gButtonScales[btn->id] = 1.0f;
                        gButtonScales[btn->id] += (target - gButtonScales[btn->id]) * 0.2f;
                        curScale = gButtonScales[btn->id];
                    }
        
                    int scaledW = cw * curScale;
                    int scaledH = ch * curScale;
                    px -= (scaledW - cw) / 2;
                    py -= (scaledH - ch) / 2;
        
                    if (!btn){
                        // Empty slot: subtle translucent glass
                        fillRoundRect(px, py, cw, ch, cornerRad, 0x1A253444);
                        gradientRoundRect(px+2, py+2, cw-4, ch-4, cornerRad-2, 0xFFFFFF08, 0xFFFFFF01);
                    } else {
                        uint32_t accent = parseHexColor(btn->color, 0x555555FF);
        
                        // 1. Drop shadow (offset)
                        fillRoundRect(px + 3, py + 6, scaledW, scaledH, cornerRad, 0x00000088);
                        
                        // 2. Active hover glow (overriding shadow if pressed)
                        if (curScale > 1.01f) {
                            fillRoundRect(px - 4, py - 4, scaledW + 8, scaledH + 8, cornerRad + 2, 0x00E5FFFF);
                        }
                        
                        // 3. Main Glass Border (Outer Stroke)
                        fillRoundRect(px, py, scaledW, scaledH, cornerRad, 0xFFFFFF44); // Semi-transparent white rim
                        
                        // 4. Inner Glass Body (Glossy Gradient)
                        // Gives the "gel" or "glass" curved look: Lighter top, darker bottom
                        gradientRoundRect(px + 2, py + 2, scaledW - 4, scaledH - 4, cornerRad - 1, 0x3A455CFF, 0x151B29FF);
                        
                        // 5. Classic Wii U "Gloss Bulb" (Top half reflection)
                        gradientRoundRect(px + 2, py + 2, scaledW - 4, (scaledH - 4) / 2, cornerRad - 1, 0xFFFFFF15, 0xFFFFFF00);
                        
                        bool hasIcon = false;
                        if (btn->actionType == 1 && !btn->actionValue.empty()) {
                            auto it = gAppIcons.find(btn->actionValue);
                            if (it != gAppIcons.end() && it->second) hasIcon = true;
                        }

                        if (hasIcon) {
                            SDL_Texture* tex = gAppIcons[btn->actionValue];
                            int tw = 32, th = 32;
                            SDL_QueryTexture(tex, nullptr, nullptr, &tw, &th);
                            
                            int ribbonHeight = cfg.showLabels ? 26 : 8; 
                            
                            // Native PC Icon
                            int iconMax = (scaledH - ribbonHeight - 8); 
                            if (iconMax > 64) iconMax = 64;
                            float sc = std::min((float)iconMax/(float)tw, (float)iconMax/(float)th);
                            int iw = tw * sc; 
                            int ih = th * sc;
                            int ix = px + (scaledW - iw) / 2;
                            int iy = py + 2 + (scaledH - ribbonHeight - ih) / 2;
                            SDL_Rect dstRect = { ix, iy, iw, ih };
                            SDL_RenderCopy(gRenderer, tex, nullptr, &dstRect);
                            
                            // 6. Color Tag Ribbon (Taller to fit text)
                            int ribbonY = py + scaledH - ribbonHeight - 2;
                            fillRoundRect(px + 2, ribbonY, scaledW - 4, ribbonHeight, cornerRad - 1, accent);
                            fillRect(px + 2, ribbonY, scaledW - 4, 8, accent);
                            gradientRoundRect(px + 2, ribbonY, scaledW - 4, ribbonHeight, cornerRad - 1, 0xFFFFFF33, 0x00000000);
                            
                            // 7. Label Text Centered INSIDE the ribbon
                            if (cfg.showLabels) {
                                int textPxX = px + scaledW / 2;
                                int textPxY = ribbonY + (ribbonHeight - 16) / 2; // roughly center font
                                renderText(textPxX + 1, textPxY + 1, btn->label.c_str(), {0, 0, 0, 150}, 1);
                                renderText(textPxX, textPxY, btn->label.c_str(), {255, 255, 255, 255}, 1);
                            }
                        } else {
                            // 6. Color Tag Ribbon (Slim)
                            int ribbonHeight = 16;
                            int ribbonY = py + scaledH - ribbonHeight - 2;
                            fillRoundRect(px + 2, ribbonY, scaledW - 4, ribbonHeight, cornerRad - 1, accent);
                            fillRect(px + 2, ribbonY, scaledW - 4, 8, accent);
                            gradientRoundRect(px + 2, ribbonY, scaledW - 4, ribbonHeight, cornerRad - 1, 0xFFFFFF33, 0x00000000);
                            
                            // 7. Label Text Centered in the glass
                            if (cfg.showLabels) {
                                int textPxX = px + scaledW / 2;
                                int textPxY = py + (scaledH - ribbonHeight - 12) / 2;
                                renderText(textPxX + 2, textPxY + 2, btn->label.c_str(), {0, 0, 0, 150}, 1);
                                renderText(textPxX, textPxY, btn->label.c_str(), {255, 255, 255, 255}, 1);
                            }
                        }
                    }
                }
            }
    
    screenFlip();
}

static void drawSettings() {
    drawHeader("Parametres");
    
    bool connectBtnSel = (gSettRow==4);
    fillRoundRect(SCR_W - 160, 10, 140, 40, 8, connectBtnSel ? 0x00A0E9FF : 0x2A325AFF);
    renderText(SCR_W - 145, 17, "CONNECTER", {255, 255, 255, 255});
    
    T(20, 70, "Navigation: Haut/Bas. Modification: Gauche/Droite. Action: A", {150, 150, 150, 255});

    const char* rowLbl[4]={"IP du PC","Port","Grille","Etiquettes"};
    int startY = 120;
    int spacing = 55;

    for(int i=0;i<4;i++){
        int py = startY + i*spacing;
        bool sel=(i==gSettRow);

        if (sel) {
            fillRoundRect(20, py, 814, 45, 8, 0x004A80FF);
            fillRect(20, py + 43, 814, 2, 0x00A0E9FF);
            renderText(30, py + 8, ">", {0, 255, 255, 255});
        } else {
            fillRoundRect(20, py, 814, 45, 8, 0x1A2534FF);
            fillRoundRect(22, py+2, 810, 41, 6, 0x0E141CFF);
        }

        char val[60];
        switch(i){
            case 0: snprintf(val,sizeof(val),"%s",cfg.host.c_str()); break;
            case 1: snprintf(val,sizeof(val),"%d",cfg.port); break;
            case 2: snprintf(val,sizeof(val),"%s",LAYOUTS[cfg.layoutIdx].name); break;
            case 3: snprintf(val,sizeof(val),"%s",cfg.showLabels?"OUI":"NON"); break;
        }
        
        SDL_Color textC = sel ? SDL_Color{255, 255, 255, 255} : SDL_Color{200, 200, 210, 255};
        renderText(60, py + 8, rowLbl[i], textC);
        renderText(300, py + 8, ":", textC);
        renderText(330, py + 8, val, sel ? SDL_Color{0, 255, 255, 255} : SDL_Color{150, 180, 255, 255});
    }
    
    int ry = 380;
    int bx = SCR_W/2 - 200, bw = 400, bh = 45;
    fillRoundRect(bx, ry, bw, bh, 8, connectBtnSel ? 0x004A80FF : 0x1A2534FF);
    if (connectBtnSel) fillRect(bx, ry+bh-2, bw, 2, 0x00A0E9FF);
    else fillRoundRect(bx+2, ry+2, bw-4, bh-4, 6, 0x0E141CFF);
    renderText(bx + 30, ry + 8, connectBtnSel ? ">>  CONNECTER ET SAUVEGARDER  <<" : "    CONNECTER ET SAUVEGARDER    ", connectBtnSel ? SDL_Color{255,255,255,255} : SDL_Color{150,150,150,255});

    drawFooter();
    screenFlip();
}

// ===================================================================
// TOUCH HANDLER
// VPAD: tx in [0,4095], ty in [0,4095], Y=0 at TOP
// ===================================================================
static void remapTouch(const VPADTouchData& td, int& px, int& py) {
    VPADTouchData calibrated;
    VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_854X480, &calibrated, &td);
    px = calibrated.x;
    py = calibrated.y;
}

static void touchGrid(int px, int py) {
    int rows = gPage.rows>0 ? gPage.rows : LAYOUTS[cfg.layoutIdx].rows;
    int cols = gPage.cols>0 ? gPage.cols : LAYOUTS[cfg.layoutIdx].cols;
    
    int usableW = SCR_W - 80;
    int usableH = SCR_H - 42 - 40;
    int gapX = 20;
    int gapY = 20;
    
    // Base Calculation
    int btnW = (usableW - (cols - 1) * gapX) / cols;
    int btnH = (usableH - (rows - 1) * gapY) / rows;
    
    // Max constraints
    if (btnW > 170) btnW = 170;
    if (btnH > 110) btnH = 110;
    
    // ENFORCE PERFECT SQUARE TILES
    int size = std::min(btnW, btnH);
    btnW = size;
    btnH = size;
    
    // Calculate new total grid dimensions with the square constraints
    int totalGridW = cols * btnW + (cols - 1) * gapX;
    int totalGridH = rows * btnH + (rows - 1) * gapY;
    
    // Perfectly center the new grid onto the screen
    int startX = (SCR_W - totalGridW) / 2;
    int startY = 42 + ((SCR_H - 42 - 40) - totalGridH) / 2;
    
    // Check hit on each button
    for (int r=0;r<rows;r++){
        for (int c=0;c<cols;c++){
            int bx = startX + c * (btnW + gapX);
            int by = startY + r * (btnH + gapY);
            if (px >= bx && px <= bx + btnW && py >= by && py <= by + btnH) {
                for(auto& b:gPage.buttons){
                    if(b.row==r && b.col==c){
                        std::string url=baseUrl()+"/api/trigger";
                        std::string body="{\"pageId\":\""+gPage.id+"\",\"buttonId\":\""+b.id+"\"}";
                        httpPost(url,body);
                        return;
                    }
                }
            }
        }
    }
}

static void touchPageList(int px, int py) {
    int startY = 140;
    int spacing = 50;
    for(int i=0;i<(int)gPages.size();i++){
        int by = startY + i * spacing;
        if(px >= 20 && px <= 20+814 && py >= by && py <= by + 40){
            gPageSel=i;
            gPage=parsePage(httpGet(baseUrl()+"/api/page/"+gPages[i].id));
            gState=S_GRID;
            return;
        }
    }
}

static void touchSettings(int px, int py) {
    // Top-right Connecter in header -> fillRoundRect(SCR_W - 160, 10, 140, 40, 8, ...
    if(px >= SCR_W - 160 && px <= SCR_W - 20 && py >= 10 && py <= 50){ gSettRow=4; saveCfg(); doFetch(); return; }
    
    // Bottom big connect button -> ry=380, bx=SCR_W/2-200, bw=400, bh=45
    int bx = SCR_W/2 - 200;
    if(px >= bx && px <= bx + 400 && py >= 380 && py <= 380 + 45){ gSettRow=4; saveCfg(); doFetch(); return; }
    
    // Rows
    int startY = 120;
    int spacing = 55;
    for(int i=0;i<4;i++){
        int by = startY + i * spacing;
        if(px >= 20 && px <= 20+814 && py >= by && py <= by + 45){
            gSettRow=i;
            switch(i){
                case 3: cfg.showLabels=!cfg.showLabels; break;
                default: break;
            }
            return;
        }
    }
}

// ===================================================================
// MAIN
// ===================================================================
int main(int, char**) {
    WHBProcInit();
    WHBLogUdpInit();
    loadCfg();
    
    if (!sdlInit()) {
        WHBLogPrintf("SDL Init Failed!");
        return -1;
    }
    initPollThread();

    // Boot splash
    drawAppBackground();
    renderText(SCR_W/2, 200, "Stream Deck pour Wii U", {255, 255, 255, 255}, 1);
    renderText(SCR_W/2, 250, "Demarrage...", {150, 180, 255, 255}, 1);
    screenFlip();
    OSSleepTicks(OSMillisecondsToTicks(600));

    gState    = S_SETTINGS;
    gSettRow  = 0;

    VPADStatus    vpad;
    VPADReadError verr;
    OSTime        lastTouch = 0;

    while (WHBProcIsRunning()) {
        VPADRead(VPAD_CHAN_0, &vpad, 1, &verr);
        
        OSLockMutex(&gPagesMutex);

        if (verr == VPAD_READ_SUCCESS) {
            // ── Global shortcuts ──────────────────────────
            if (vpad.trigger & VPAD_BUTTON_PLUS) gState = S_SETTINGS;
            if (vpad.trigger & VPAD_BUTTON_MINUS) {
                if      (gState==S_GRID)  gState=(gPages.size()==1)?S_SETTINGS:S_PAGELIST;
                else if (gState==S_SETTINGS&&!gPages.empty())
                         gState=(gPages.size()==1)?S_GRID:S_PAGELIST;
            }

            // ── Settings navigation ───────────────────────
            if (gState==S_SETTINGS) {
                if (vpad.trigger & VPAD_BUTTON_UP)
                    gSettRow=(gSettRow+4)%5;
                if (vpad.trigger & VPAD_BUTTON_DOWN)
                    gSettRow=(gSettRow+1)%5;

                if (vpad.trigger & (VPAD_BUTTON_LEFT|VPAD_BUTTON_RIGHT)){
                    int dir=(vpad.trigger&VPAD_BUTTON_RIGHT)?1:-1;
                    switch(gSettRow){
                        case 1: cfg.port=std::clamp(cfg.port+dir*100,1,65535); break;
                        case 2: 
                            cfg.layoutIdx=std::clamp(cfg.layoutIdx+dir,0,LAYOUT_N-1); 
                            syncLayoutToPC();
                            break;
                        case 3: cfg.showLabels=!cfg.showLabels; break;
                        default: break;
                    }
                }

                if (vpad.trigger & VPAD_BUTTON_A){
                    if (gSettRow==0){
                        // System keyboard for IP entry
                        std::string newIP = openIPEditor(vpad, verr);
                        if (!newIP.empty()) cfg.host = newIP;
                    }
                    else if (gSettRow==4){ saveCfg(); doFetch(); }
                }
            }

            // ── Page list navigation ──────────────────────
            if (gState==S_PAGELIST){
                if(vpad.trigger&VPAD_BUTTON_UP)   gPageSel=std::max(0,gPageSel-1);
                if(vpad.trigger&VPAD_BUTTON_DOWN)  gPageSel=std::min((int)gPages.size()-1,gPageSel+1);
                if(vpad.trigger&VPAD_BUTTON_A){
                    gPage=parsePage(httpGet(baseUrl()+"/api/page/"+gPages[gPageSel].id));
                    gState=S_GRID;
                }
            }
        }

        // Touch (350ms debounce; Y NOT inverted)
        if (verr==VPAD_READ_SUCCESS && vpad.tpNormal.touched){
            OSTime now=OSGetTime();
            if((uint64_t)(now-lastTouch)>OSMillisecondsToTicks(350)){
                lastTouch=now;
                int cx,cy; remapTouch(vpad.tpNormal,cx,cy);
                switch(gState){
                    case S_GRID:     touchGrid(cx,cy);     break;
                    case S_PAGELIST: touchPageList(cx,cy); break;
                    case S_SETTINGS: touchSettings(cx,cy); break;
                    default: break;
                }
            }
        }

        std::string hoveredId = "";
        if (gState == S_GRID && verr == VPAD_READ_SUCCESS && vpad.tpNormal.touched) {
            int cx, cy; remapTouch(vpad.tpNormal, cx, cy);
            int rows=gPage.rows>0?gPage.rows:LAYOUTS[cfg.layoutIdx].rows;
            int cols=gPage.cols>0?gPage.cols:LAYOUTS[cfg.layoutIdx].cols;
            int cellWch=(COLS-4)/cols - 1;
            int cellHch=22/rows - 1;
            int startRow=3;
            int startCol=2 + ((COLS-4) - (cellWch+1)*cols + 1) / 2;

            if (cy >= startRow) {
                int c = (cx - startCol) / (cellWch + 1);
                int r = (cy - startRow) / (cellHch + 1);
                int relX = (cx - startCol) % (cellWch + 1);
                int relY = (cy - startRow) % (cellHch + 1);
                if (relX < cellWch && relY < cellHch && c>=0 && c<cols && r>=0 && r<rows) {
                    for(auto& b:gPage.buttons){
                        if(b.row==r && b.col==c) { hoveredId = b.id; break; }
                    }
                }
            }
        }

        // Process Pending Icons on Main Thread (OpenGL/GX2 Context)
        OSLockMutex(&gIconMutex);
        if (!gIconQueue.empty()) {
            for (auto& pi : gIconQueue) {
                if (pi.data.size() > 8) {
                    // Extract Little Endian width/height safely to avoid Unaligned Access on PowerPC
                    uint32_t w_le, h_le;
                    std::memcpy(&w_le, pi.data.data(), 4);
                    std::memcpy(&h_le, pi.data.data() + 4, 4);
                    uint32_t w = SDL_SwapLE32(w_le);
                    uint32_t h = SDL_SwapLE32(h_le);

                    if (w > 0 && h > 0 && w <= 256 && h <= 256 && pi.data.size() >= 8 + w*h*4) {
                        // The stream is exactly RGBA byte array [R, G, B, A]
                        uint32_t rmask = 0xFF000000;
                        uint32_t gmask = 0x00FF0000;
                        uint32_t bmask = 0x0000FF00;
                        uint32_t amask = 0x000000FF;
                        
                        SDL_Surface* s = SDL_CreateRGBSurfaceFrom((void*)(pi.data.data() + 8), w, h, 32, w * 4, rmask, gmask, bmask, amask);
                        if (s) {
                            SDL_Texture* tex = SDL_CreateTextureFromSurface(gRenderer, s);
                            if (tex) {
                                SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                                gAppIcons[pi.path] = tex;
                            }
                            SDL_FreeSurface(s);
                        }
                    }
                }
            }
            gIconQueue.clear();
        }
        OSUnlockMutex(&gIconMutex);

        // Render
        switch(gState){
            case S_LOADING:  drawLoading();  break;
            case S_PAGELIST: drawPageList(); break;
            case S_GRID:     drawGrid(hoveredId);     break;
            case S_SETTINGS: drawSettings(); break;
        }
        
        OSUnlockMutex(&gPagesMutex);
    }

    saveCfg();
    shutdownPollThread();
    sdlShutdown();
    WHBLogUdpDeinit();
    WHBProcShutdown();
    return 0;
}

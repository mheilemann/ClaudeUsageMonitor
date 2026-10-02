/*
    Claude Usage Monitor
    C11 / Visual Studio 2022
    No third-party libraries. Native Win32 GUI, no console window.
*/

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include <winhttp.h>
#include <shellapi.h>
#include <dwmapi.h>
#include "resource.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")

#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif

#define APP_VERSION "1.0.0"
#define PAYPAL_URL "https://www.paypal.com/paypalme/MichaelHeilemann420?locale.x=en_US&country.x=US"

#define PATH_CAP 4096
#define LINE_CAP 65536
#define TIMER_ID  1

static int g_dpi = 96;
#define SX(v) MulDiv((v), g_dpi, 96)

typedef struct {
    unsigned long long input;
    unsigned long long output;
    unsigned long long cache_create;
    unsigned long long cache_read;
    unsigned long long requests;
    double context_pct;
    unsigned long long context_tokens;
    char model[128];
    char effort[32];
    char session_file[PATH_CAP];
    time_t modified;
} SessionStats;

typedef struct {
    int valid;
    double five_pct;
    double seven_pct;
    time_t five_reset;
    time_t seven_reset;
} RateStats;

static char g_root[PATH_CAP];
static SessionStats g_session;
static RateStats g_rates;
static int g_have_session;

enum { API_PENDING, API_OK, API_NO_CREDS, API_AUTH, API_NET };
static volatile LONG g_api_state = API_PENDING;

static HFONT g_font_title;
static HFONT g_font_header;
static HFONT g_font_body;

static int get_env_path(const char *name, char *out, size_t cap) {
    DWORD n = GetEnvironmentVariableA(name, out, (DWORD)cap);
    return n > 0 && n < cap;
}

static int make_dir_recursive(const char *path) {
    char tmp[PATH_CAP];
    size_t i, n = strlen(path);
    if (n >= sizeof(tmp)) return 0;
    strcpy(tmp, path);
    for (i = 3; i < n; ++i) {
        if (tmp[i] == '\\' || tmp[i] == '/') {
            char save = tmp[i];
            tmp[i] = '\0';
            CreateDirectoryA(tmp, NULL);
            tmp[i] = save;
        }
    }
    return CreateDirectoryA(tmp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int get_status_dir(char *out, size_t cap) {
    char local[PATH_CAP];
    if (!get_env_path("LOCALAPPDATA", local, sizeof(local))) return 0;
    snprintf(out, cap, "%s\\ClaudeUsageMonitor", local);
    return 1;
}

/* Reads a whole file into a NUL-terminated malloc'd buffer. Shares delete access
   so a concurrent atomic replace by the bridge is never blocked. */
static char *read_file_all(const char *path, size_t max_size, size_t *out_len) {
    HANDLE h;
    LARGE_INTEGER size;
    DWORD got = 0;
    char *data;

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || (unsigned long long)size.QuadPart > max_size) {
        CloseHandle(h);
        return NULL;
    }
    data = (char*)malloc((size_t)size.QuadPart + 1);
    if (!data) { CloseHandle(h); return NULL; }
    if (!ReadFile(h, data, (DWORD)size.QuadPart, &got, NULL) || got != (DWORD)size.QuadPart) {
        free(data); CloseHandle(h); return NULL;
    }
    CloseHandle(h);
    data[got] = '\0';
    if (out_len) *out_len = got;
    return data;
}

/* Writes to a temp file then renames over the target, so readers never see a partial file. */
static int write_file_atomic(const char *path, const char *data, size_t len) {
    char tmp[PATH_CAP];
    FILE *f;
    int i;

    snprintf(tmp, sizeof(tmp), "%s.%lu.tmp", path, (unsigned long)GetCurrentProcessId());
    f = fopen(tmp, "wb");
    if (!f) return 0;
    if (fwrite(data, 1, len, f) != len) { fclose(f); DeleteFileA(tmp); return 0; }
    fclose(f);

    for (i = 0; i < 50; ++i) {
        if (MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 1;
        Sleep(10);
    }
    DeleteFileA(tmp);
    return 0;
}

static int json_number_after(const char *p, const char *key, double *value) {
    const char *k = strstr(p, key);
    char *end;
    if (!k) return 0;
    k = strchr(k, ':');
    if (!k) return 0;
    ++k;
    while (*k == ' ' || *k == '\t') ++k;
    *value = strtod(k, &end);
    return end != k;
}

static unsigned long long json_u64_after(const char *p, const char *key) {
    double v = 0;
    if (!json_number_after(p, key, &v) || v < 0) return 0;
    return (unsigned long long)(v + 0.5);
}

static void json_string_after(const char *p, const char *key, char *out, size_t cap) {
    const char *k = strstr(p, key);
    const char *q;
    size_t n;
    if (!k) { out[0] = '\0'; return; }
    k = strchr(k, ':');
    if (!k) { out[0] = '\0'; return; }
    ++k;
    while (*k == ' ' || *k == '\t') ++k;
    if (*k != '"') { out[0] = '\0'; return; }
    ++k;
    q = strchr(k, '"');
    if (!q) { out[0] = '\0'; return; }
    n = (size_t)(q - k);
    if (n >= cap) n = cap - 1;
    memcpy(out, k, n);
    out[n] = '\0';
}

static void format_tokens(unsigned long long n, char *out, size_t cap) {
    if (n >= 1000000000ULL)
        snprintf(out, cap, "%.2fB", (double)n / 1000000000.0);
    else if (n >= 1000000ULL)
        snprintf(out, cap, "%.2fM", (double)n / 1000000.0);
    else if (n >= 1000ULL)
        snprintf(out, cap, "%.1fK", (double)n / 1000.0);
    else
        snprintf(out, cap, "%llu", n);
}

static void format_reset(time_t t, char *out, size_t cap) {
    long long diff;
    long long days, hours, mins, secs;

    if (t <= 0) { snprintf(out, cap, "--"); return; }
    diff = (long long)(t - time(NULL));
    if (diff <= 0) { snprintf(out, cap, "--"); return; }

    days = diff / 86400;
    hours = (diff % 86400) / 3600;
    mins = (diff % 3600) / 60;
    secs = diff % 60;

    if (days > 0)
        snprintf(out, cap, "%lldd %lldh %lldm", days, hours, mins);
    else if (hours > 0)
        snprintf(out, cap, "%lldh %lldm %llds", hours, mins, secs);
    else if (mins > 0)
        snprintf(out, cap, "%lldm %llds", mins, secs);
    else
        snprintf(out, cap, "%llds", secs);
}

static int read_rate_file(RateStats *r) {
    char dir[PATH_CAP], path[PATH_CAP];
    char *data;
    char *p;
    double v;

    memset(r, 0, sizeof(*r));
    if (!get_status_dir(dir, sizeof(dir))) return 0;
    snprintf(path, sizeof(path), "%s\\status.json", dir);

    data = read_file_all(path, 2 * 1024 * 1024, NULL);
    if (!data) return 0;

    p = strstr(data, "\"five_hour\"");
    if (p && json_number_after(p, "\"used_percentage\"", &v)) {
        r->five_pct = v;
        r->five_reset = (time_t)json_u64_after(p, "\"resets_at\"");
        r->valid = 1;
    }
    p = strstr(data, "\"seven_day\"");
    if (p && json_number_after(p, "\"used_percentage\"", &v)) {
        r->seven_pct = v;
        r->seven_reset = (time_t)json_u64_after(p, "\"resets_at\"");
        r->valid = 1;
    }

    free(data);
    return r->valid;
}

static int find_latest_jsonl_recursive(const char *dir, char *best, size_t cap, time_t *best_time) {
    char pattern[PATH_CAP];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int found = 0;

    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    do {
        char path[PATH_CAP];
        struct _stat st;

        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (find_latest_jsonl_recursive(path, best, cap, best_time)) found = 1;
        } else {
            const char *ext = strrchr(fd.cFileName, '.');
            if (ext && !_stricmp(ext, ".jsonl")) {
                if (_stat(path, &st) == 0 && st.st_mtime >= *best_time) {
                    *best_time = st.st_mtime;
                    strncpy(best, path, cap - 1);
                    best[cap - 1] = '\0';
                    found = 1;
                }
            }
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
    return found;
}

static int scan_session(const char *path, SessionStats *s) {
    FILE *f;
    char *line;
    size_t cap = LINE_CAP;
    unsigned long long last_input = 0, last_cache_create = 0, last_cache_read = 0;
    unsigned long long last_output = 0;
    char *p;

    memset(s, 0, sizeof(*s));
    strncpy(s->session_file, path, sizeof(s->session_file)-1);

    f = fopen(path, "rb");
    if (!f) return 0;
    line = (char*)malloc(cap);
    if (!line) { fclose(f); return 0; }

    while (fgets(line, (int)cap, f)) {
        if (!strstr(line, "\"type\":\"assistant\"") &&
            !strstr(line, "\"type\": \"assistant\""))
            continue;

        p = strstr(line, "\"usage\"");
        if (!p) continue;

        last_input = json_u64_after(p, "\"input_tokens\"");
        last_output = json_u64_after(p, "\"output_tokens\"");
        last_cache_create = json_u64_after(p, "\"cache_creation_input_tokens\"");
        last_cache_read = json_u64_after(p, "\"cache_read_input_tokens\"");

        s->input += last_input;
        s->output += last_output;
        s->cache_create += last_cache_create;
        s->cache_read += last_cache_read;
        s->requests++;

        /* Keep the most recent response's context-sized input. */
        s->context_tokens = last_input + last_cache_create + last_cache_read;
        s->context_pct = (s->context_tokens / 200000.0) * 100.0;

        json_string_after(line, "\"display_name\"", s->model, sizeof(s->model));
        if (!s->model[0])
            json_string_after(line, "\"model\"", s->model, sizeof(s->model));
        json_string_after(line, "\"effort\"", s->effort, sizeof(s->effort));
    }

    free(line);
    fclose(f);
    return 1;
}

static void refresh_data(void) {
    char latest[PATH_CAP];
    time_t latest_time = 0;
    RateStats rates;

    /* Keep the last good reading if the file is momentarily unavailable. */
    if (read_rate_file(&rates)) g_rates = rates;

    latest[0] = '\0';
    if (!find_latest_jsonl_recursive(g_root, latest, sizeof(latest), &latest_time)) {
        g_have_session = 0;
        return;
    }
    if (!scan_session(latest, &g_session)) {
        g_have_session = 0;
        return;
    }
    g_have_session = 1;
}

/* ---- Drawing ---- */

static COLORREF bar_color(double pct) {
    if (pct >= 90.0) return RGB(224, 70, 70);
    if (pct >= 70.0) return RGB(224, 190, 50);
    return RGB(70, 190, 100);
}

static int g_measuring = 0;
static int g_max_x = 0;
static HDC g_measure_dc = NULL;
static RECT g_paypal_rect = { 0, 0, 0, 0 };

static void draw_bar(HDC hdc, int x, int y, int w, int h, double pct) {
    RECT track = { x, y, x + w, y + h };
    if (g_measuring) { if (x + w > g_max_x) g_max_x = x + w; return; }
    RECT fill = track;
    HBRUSH track_brush, fill_brush;
    int filled_w;

    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    filled_w = (int)((pct / 100.0) * w + 0.5);
    fill.right = x + filled_w;

    track_brush = CreateSolidBrush(RGB(45, 45, 48));
    FillRect(hdc, &track, track_brush);
    DeleteObject(track_brush);

    if (filled_w > 0) {
        fill_brush = CreateSolidBrush(bar_color(pct));
        FillRect(hdc, &fill, fill_brush);
        DeleteObject(fill_brush);
    }

    FrameRect(hdc, &track, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
}

static void text_out(HDC hdc, int x, int y, COLORREF color, HFONT font, const char *s) {
    if (g_measuring) {
        SIZE sz;
        SelectObject(g_measure_dc, font);
        GetTextExtentPoint32A(g_measure_dc, s, (int)strlen(s), &sz);
        if (x + sz.cx > g_max_x) g_max_x = x + sz.cx;
        return;
    }
    SelectObject(hdc, font);
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);
    TextOutA(hdc, x, y, s, (int)strlen(s));
}

/* Draws a small bordered button with centered text and records its rect for
   click hit-testing. x,y is the button's top-left; h is its fixed height. */
static void draw_button(HDC hdc, int x, int y, int h, HFONT font, COLORREF text_color,
    COLORREF border_color, const char *label, RECT *rect_out) {
    SIZE sz;
    HDC measure_on = g_measuring ? g_measure_dc : hdc;
    const int pad_x = SX(8);
    int w;

    SelectObject(measure_on, font);
    GetTextExtentPoint32A(measure_on, label, (int)strlen(label), &sz);
    w = sz.cx + pad_x * 2;

    if (g_measuring) {
        if (x + w > g_max_x) g_max_x = x + w;
        return;
    }

    {
        RECT r = { x, y, x + w, y + h };
        HBRUSH fill = CreateSolidBrush(RGB(28, 40, 46));
        HPEN pen = CreatePen(PS_SOLID, 1, border_color);
        HPEN old_pen = (HPEN)SelectObject(hdc, pen);
        HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));

        FillRect(hdc, &r, fill);
        DeleteObject(fill);
        Rectangle(hdc, r.left, r.top, r.right, r.bottom);
        SelectObject(hdc, old_brush);
        SelectObject(hdc, old_pen);
        DeleteObject(pen);

        SetTextColor(hdc, text_color);
        SetBkMode(hdc, TRANSPARENT);
        TextOutA(hdc, x + pad_x, y + (h - sz.cy) / 2, label, (int)strlen(label));

        if (rect_out) *rect_out = r;
    }
}

static int render(HDC hdc, RECT *client) {
    char buf[256];
    int y = SX(8);
    const int left = SX(12);
    const COLORREF white = RGB(230, 230, 230);
    const COLORREF dim = RGB(150, 150, 150);
    const COLORREF cyan = RGB(90, 200, 255);

    if (!g_measuring) {
        HBRUSH bg = CreateSolidBrush(RGB(20, 20, 22));
        FillRect(hdc, client, bg);
        DeleteObject(bg);
    }

    {
        HDC measure_on = g_measuring ? g_measure_dc : hdc;
        SIZE title_sz, btn_sz;
        const int pad_x = SX(8);
        int btn_h, btn_w, btn_x;

        SelectObject(measure_on, g_font_title);
        GetTextExtentPoint32A(measure_on, "M", 1, &title_sz);

        SelectObject(measure_on, g_font_body);
        GetTextExtentPoint32A(measure_on, "Support the App", (int)strlen("Support the App"), &btn_sz);
        btn_w = btn_sz.cx + pad_x * 2;
        btn_h = (int)(title_sz.cy * 0.50);
        btn_x = g_measuring ? left : client->right - left - btn_w;

        draw_button(hdc, btn_x, y, btn_h,
            g_font_body, cyan, cyan, "Support the App", &g_paypal_rect);

        y += btn_h;
    }
    y += SX(4);

    if (!g_have_session) {
        text_out(hdc, left, y, white, g_font_body, "No Claude Code sessions found under:");
        y += SX(16);
        text_out(hdc, left, y, dim, g_font_body, g_root);
        y += SX(22);
        text_out(hdc, left, y, white, g_font_body, "Start Claude Code and use a session;");
        y += SX(16);
        text_out(hdc, left, y, white, g_font_body, "this window will update automatically.");
        return y;
    }

    if (g_session.effort[0])
        snprintf(buf, sizeof(buf), "Model:    %s  (effort: %s)",
            g_session.model[0] ? g_session.model : "unknown", g_session.effort);
    else
        snprintf(buf, sizeof(buf), "Model:    %s", g_session.model[0] ? g_session.model : "unknown");
    text_out(hdc, left, y, white, g_font_body, buf);
    y += SX(15);

    snprintf(buf, sizeof(buf), "Requests: %llu", g_session.requests);
    text_out(hdc, left, y, white, g_font_body, buf);
    y += SX(22);

    text_out(hdc, left, y, cyan, g_font_header, "SESSION TOKEN USAGE");
    y += SX(17);
    {
        char inbuf[32], outbuf[32], cachebuf[32], totalbuf[32];
        unsigned long long total = g_session.input + g_session.output + g_session.cache_create + g_session.cache_read;
        format_tokens(g_session.input, inbuf, sizeof(inbuf));
        format_tokens(g_session.output, outbuf, sizeof(outbuf));
        format_tokens(g_session.cache_create + g_session.cache_read, cachebuf, sizeof(cachebuf));
        format_tokens(total, totalbuf, sizeof(totalbuf));

        snprintf(buf, sizeof(buf), "In:   %8s         Out: %8s", inbuf, outbuf);
        text_out(hdc, left + SX(8), y, white, g_font_body, buf);
        y += SX(15);
        snprintf(buf, sizeof(buf), "Cache:%8s         Tot: %8s", cachebuf, totalbuf);
        text_out(hdc, left + SX(8), y, white, g_font_body, buf);
        y += SX(20);
    }

    text_out(hdc, left, y, cyan, g_font_header, "LATEST CONTEXT");
    y += SX(17);
    {
        char ctxbuf[32];
        double pct = g_session.context_pct > 100 ? 100.0 : g_session.context_pct;
        format_tokens(g_session.context_tokens, ctxbuf, sizeof(ctxbuf));
        snprintf(buf, sizeof(buf), "%s / 200K tokens", ctxbuf);
        text_out(hdc, left + SX(8), y, white, g_font_body, buf);
        y += SX(16);
        draw_bar(hdc, left + SX(8), y, SX(260), SX(14), pct);
        snprintf(buf, sizeof(buf), "%.1f%%", pct);
        text_out(hdc, left + SX(8) + SX(260) + SX(8), y + SX(1), white, g_font_body, buf);
        y += SX(24);
    }

    text_out(hdc, left, y, cyan, g_font_header, "SUBSCRIPTION RATE LIMITS");
    y += SX(17);
    if (g_rates.valid) {
        char reset[32];
        const int bar_x = left + SX(50);
        const int bar_w = SX(150);

        format_reset(g_rates.five_reset, reset, sizeof(reset));
        text_out(hdc, left + SX(8), y, white, g_font_body, "5-hour");
        draw_bar(hdc, bar_x, y, bar_w, SX(14), g_rates.five_pct);
        snprintf(buf, sizeof(buf), "%.1f%% (%s)", g_rates.five_pct, reset);
        text_out(hdc, bar_x + bar_w + SX(8), y + SX(1), white, g_font_body, buf);
        y += SX(18);

        format_reset(g_rates.seven_reset, reset, sizeof(reset));
        text_out(hdc, left + SX(8), y, white, g_font_body, "7-day");
        draw_bar(hdc, bar_x, y, bar_w, SX(14), g_rates.seven_pct);
        snprintf(buf, sizeof(buf), "%.1f%% (%s)", g_rates.seven_pct, reset);
        text_out(hdc, bar_x + bar_w + SX(8), y + SX(1), white, g_font_body, buf);
        y += SX(22);
    } else {
        const char *l1 = "Fetching live 5-hour/7-day data...", *l2 = "";
        switch (g_api_state) {
        case API_NO_CREDS: l1 = "No Claude Code login found."; l2 = "Run 'claude' and log in with your plan."; break;
        case API_AUTH:     l1 = "Claude Code login token expired."; l2 = "Run Claude Code once to refresh it."; break;
        case API_NET:      l1 = "Can't reach api.anthropic.com."; l2 = "Retrying automatically."; break;
        }
        text_out(hdc, left + SX(8), y, dim, g_font_body, l1);
        y += SX(15);
        text_out(hdc, left + SX(8), y, dim, g_font_body, l2);
        y += SX(22);
    }

    return y;
}

static void on_paint(HWND hwnd) {
    PAINTSTRUCT ps;
    RECT client;
    HDC hdc, mem_hdc;
    HBITMAP mem_bmp, old_bmp;

    hdc = BeginPaint(hwnd, &ps);
    GetClientRect(hwnd, &client);

    mem_hdc = CreateCompatibleDC(hdc);
    mem_bmp = CreateCompatibleBitmap(hdc, client.right, client.bottom);
    old_bmp = (HBITMAP)SelectObject(mem_hdc, mem_bmp);

    render(mem_hdc, &client);

    BitBlt(hdc, 0, 0, client.right, client.bottom, mem_hdc, 0, 0, SRCCOPY);

    SelectObject(mem_hdc, old_bmp);
    DeleteObject(mem_bmp);
    DeleteDC(mem_hdc);

    EndPaint(hwnd, &ps);
}

static void create_fonts(void) {
    if (g_font_title) DeleteObject(g_font_title);
    if (g_font_header) DeleteObject(g_font_header);
    if (g_font_body) DeleteObject(g_font_body);

    g_font_title = CreateFontA(-SX(17), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH, "Segoe UI");
    g_font_header = CreateFontA(-SX(13), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH, "Segoe UI");
    g_font_body = CreateFontA(-SX(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH, "Consolas");
}

/* Measures the content at the current g_dpi/fonts using the same worst-case
   placeholder data the initial sizing pass uses, so the window never has to
   grow later just because real numbers are wider than "unknown"/0. */
static void measure_window_size(int *out_w, int *out_h) {
    HDC mdc = GetDC(NULL);
    int left = SX(12);
    int final_y;
    RECT dummy = { 0, 0, 0, 0 };
    SessionStats saved_session = g_session;
    RateStats saved_rates = g_rates;
    int saved_have_session = g_have_session;
    time_t now = time(NULL);

    g_have_session = 1;
    memset(&g_session, 0, sizeof(g_session));
    strncpy(g_session.model, "claude-sonnet-4-5-20250929", sizeof(g_session.model) - 1);
    strncpy(g_session.effort, "medium", sizeof(g_session.effort) - 1);
    g_session.requests = 9999;
    g_session.context_tokens = 199999;
    g_session.context_pct = 100.0;

    g_rates.valid = 1;
    g_rates.five_pct = 100.0;
    g_rates.seven_pct = 100.0;
    g_rates.five_reset = now + 4 * 3600 + 59 * 60 + 59;
    g_rates.seven_reset = now + 6 * 86400 + 23 * 3600 + 59 * 60;

    g_measure_dc = mdc;
    g_measuring = 1;
    g_max_x = 0;
    final_y = render(NULL, &dummy);
    g_measuring = 0;
    g_measure_dc = NULL;
    ReleaseDC(NULL, mdc);

    g_session = saved_session;
    g_rates = saved_rates;
    g_have_session = saved_have_session;

    *out_w = (int)((g_max_x + left + SX(16)) * 0.9);
    *out_h = (int)((final_y + SX(10)) * 0.95);
}

/* Re-fonts, re-measures and resizes/repositions the window for a new DPI.
   suggested is the RECT Windows hands back in WM_DPICHANGED's lParam, or
   NULL to keep the window's current top-left (used at initial creation). */
static void apply_dpi(HWND hwnd, int dpi, const RECT *suggested) {
    int win_w, win_h;
    DWORD style = (DWORD)GetWindowLongA(hwnd, GWL_STYLE);
    RECT wr;

    g_dpi = dpi;
    create_fonts();
    measure_window_size(&win_w, &win_h);

    wr.left = 0; wr.top = 0; wr.right = win_w; wr.bottom = win_h;
    AdjustWindowRectEx(&wr, style, FALSE, 0);
    win_w = wr.right - wr.left;
    win_h = wr.bottom - wr.top;

    if (suggested) {
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top, win_w, win_h,
            SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        SetWindowPos(hwnd, NULL, 0, 0, win_w, win_h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    }
    InvalidateRect(hwnd, NULL, TRUE);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        refresh_data();
        SetTimer(hwnd, TIMER_ID, 50, NULL);
        return 0;
    case WM_TIMER:
        refresh_data();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_DPICHANGED:
        apply_dpi(hwnd, HIWORD(wp), (const RECT*)lp);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        on_paint(hwnd);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt = { LOWORD(lp), HIWORD(lp) };
        if (PtInRect(&g_paypal_rect, pt))
            ShellExecuteA(NULL, "open", PAYPAL_URL, NULL, NULL, SW_SHOWNORMAL);
        return 0;
    }
    case WM_SETCURSOR: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (PtInRect(&g_paypal_rect, pt)) {
            SetCursor(LoadCursorA(NULL, (LPCSTR)IDC_HAND));
            return TRUE;
        }
        break;
    }
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static int bridge_mode(void) {
    char dir[PATH_CAP], path[PATH_CAP];
    char *data;
    size_t len = 0, cap = 64 * 1024, n;

    data = (char*)malloc(cap + 1);
    if (!data) return 1;
    while ((n = fread(data + len, 1, cap - len, stdin)) > 0) {
        len += n;
        if (len == cap) {
            char *grown;
            if (cap >= 8 * 1024 * 1024) break;
            cap *= 2;
            grown = (char*)realloc(data, cap + 1);
            if (!grown) break;
            data = grown;
        }
    }
    data[len] = '\0';

    /* rate_limits only appears after the first API response of a session; don't
       clobber good data from another session with a payload that lacks it. */
    if (strstr(data, "\"rate_limits\"") && get_status_dir(dir, sizeof(dir))) {
        make_dir_recursive(dir);
        snprintf(path, sizeof(path), "%s\\status.json", dir);
        write_file_atomic(path, data, len);
    }
    free(data);

    /* Claude Code expects status-line output; keep it unobtrusive. */
    printf("Usage monitor");
    return 0;
}

/* ---- statusLine auto-install ---- */


static const char *json_skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
}

static const char *json_skip_string(const char *p) {
    if (*p != '"') return NULL;
    for (++p; *p; ++p) {
        if (*p == '\\') { if (!*++p) return NULL; }
        else if (*p == '"') return p + 1;
    }
    return NULL;
}

static const char *json_skip_value(const char *p) {
    int depth = 0;
    p = json_skip_ws(p);
    if (*p == '"') return json_skip_string(p);
    if (*p != '{' && *p != '[') {
        while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
               *p != '\t' && *p != '\r' && *p != '\n') ++p;
        return p;
    }
    while (*p) {
        if (*p == '"') { p = json_skip_string(p); if (!p) return NULL; continue; }
        if (*p == '{' || *p == '[') ++depth;
        else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
        ++p;
    }
    return NULL;
}

/* Finds a top-level member's value. Returns 1 if found, 0 if absent, -1 if the document can't be parsed. */
static int json_find_member(const char *doc, const char *key, const char **val_start, const char **val_end) {
    size_t key_len = strlen(key);
    const char *p = json_skip_ws(doc);

    if (*p != '{') return -1;
    p = json_skip_ws(p + 1);
    if (*p == '}') return 0;
    for (;;) {
        const char *name = p, *name_end = json_skip_string(p), *v;
        if (!name_end) return -1;
        p = json_skip_ws(name_end);
        if (*p != ':') return -1;
        v = json_skip_ws(p + 1);
        p = json_skip_value(v);
        if (!p) return -1;
        if ((size_t)(name_end - name) == key_len + 2 && !strncmp(name + 1, key, key_len)) {
            *val_start = v;
            *val_end = p;
            return 1;
        }
        p = json_skip_ws(p);
        if (*p == '}') return 0;
        if (*p != ',') return -1;
        p = json_skip_ws(p + 1);
    }
}

/* Builds a status-line command that survives both Git Bash and PowerShell, which
   is what Claude Code uses on Windows. Forward slashes stop bash from eating
   backslashes; the 8.3 short path avoids quoting (a quoted path followed by an
   argument is a PowerShell parse error); "--bridge" avoids MSYS rewriting "/bridge". */
static int build_bridge_command(char *out, size_t cap) {
    char exe[PATH_CAP], short_path[PATH_CAP];
    const char *use = exe;
    char *c;
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));

    if (n == 0 || n >= sizeof(exe)) return 0;
    n = GetShortPathNameA(exe, short_path, sizeof(short_path));
    if (n > 0 && n < sizeof(short_path) && !strchr(short_path, ' ')) use = short_path;

    if (strchr(use, ' ')) snprintf(out, cap, "\"%s\" --bridge", use);
    else snprintf(out, cap, "%s --bridge", use);
    for (c = out; *c; ++c) if (*c == '\\') *c = '/';
    return 1;
}

static void json_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (; *in && o + 2 < cap; ++in) {
        if (*in == '"' || *in == '\\') out[o++] = '\\';
        out[o++] = *in;
    }
    out[o] = '\0';
}

/* Points Claude Code's statusLine at this exe so status.json is produced automatically.
   Replaces a previous (possibly broken) entry for this monitor, but never overwrites
   a status line belonging to something else. */
static void install_statusline(void) {
    char user[PATH_CAP], path[PATH_CAP], backup[PATH_CAP];
    char cmd[PATH_CAP], escaped[PATH_CAP * 2], value[PATH_CAP * 2 + 256];
    char *doc, *out;
    const char *vs, *ve;
    size_t doc_len = 0, out_cap;
    int found;

    if (!get_env_path("USERPROFILE", user, sizeof(user))) return;
    if (!build_bridge_command(cmd, sizeof(cmd))) return;
    json_escape(cmd, escaped, sizeof(escaped));
    snprintf(value, sizeof(value),
        "{\n    \"type\": \"command\",\n    \"command\": \"%s\",\n    \"refreshInterval\": 5\n  }", escaped);

    snprintf(path, sizeof(path), "%s\\.claude", user);
    make_dir_recursive(path);
    snprintf(path, sizeof(path), "%s\\.claude\\settings.json", user);

    doc = read_file_all(path, 16 * 1024 * 1024, &doc_len);
    if (!doc) {
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;
        snprintf(escaped, sizeof(escaped), "{\n  \"statusLine\": %s\n}\n", value);
        write_file_atomic(path, escaped, strlen(escaped));
        return;
    }

    found = json_find_member(doc, "statusLine", &vs, &ve);
    if (found < 0) { free(doc); return; }
    if (found) {
        size_t old_len = (size_t)(ve - vs);
        int ours = 0;
        char *old = (char*)malloc(old_len + 1);
        if (old) {
            memcpy(old, vs, old_len);
            old[old_len] = '\0';
            ours = strstr(old, "ClaudeUsageMonitor") || strstr(old, "--bridge");
            free(old);
        }
        if (!ours) { free(doc); return; }
        if (old_len == strlen(value) && !memcmp(vs, value, old_len)) { free(doc); return; }
    }

    out_cap = doc_len + strlen(value) + 64;
    out = (char*)malloc(out_cap);
    if (!out) { free(doc); return; }

    if (found) {
        snprintf(out, out_cap, "%.*s%s%s", (int)(vs - doc), doc, value, ve);
    } else {
        /* Insert as the last member of the top-level object. */
        char *close = strrchr(doc, '}');
        char *last = close;
        if (!close) { free(out); free(doc); return; }
        do { --last; } while (last > doc && (*last == ' ' || *last == '\t' || *last == '\r' || *last == '\n'));
        snprintf(out, out_cap, "%.*s%s\n  \"statusLine\": %s\n%s",
            (int)(last + 1 - doc), doc, *last == '{' ? "" : ",", value, close);
    }

    snprintf(backup, sizeof(backup), "%s.bak", path);
    CopyFileA(path, backup, FALSE);
    write_file_atomic(path, out, strlen(out));
    free(out);
    free(doc);
}

/* ---- Live usage poller ----
   Claude Code's status line only runs in the interactive terminal UI, not when it
   is hosted headless (IDE extensions, --print). So the monitor also asks the same
   endpoint Claude Code's /usage command uses, with Claude Code's own OAuth login,
   and writes the result to status.json in the status-line format. */

#define USAGE_POLL_MS     30000
#define USAGE_BACKOFF_MAX 300000

/* "2026-10-01T22:40:00.257300+00:00" -> Unix epoch seconds. */
static time_t parse_iso8601(const char *s) {
    struct tm tm;
    const char *p;
    time_t t;

    memset(&tm, 0, sizeof(tm));
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    t = _mkgmtime(&tm);
    if (t == (time_t)-1) return 0;

    p = s + 19;
    if (*p == '.') while (*++p >= '0' && *p <= '9') {}
    if (*p == '+' || *p == '-') {
        int oh = 0, om = 0;
        sscanf(p + 1, "%d:%d", &oh, &om);
        t += (*p == '+' ? -1 : 1) * (time_t)(oh * 3600 + om * 60);
    }
    return t;
}

/* Extracts {"utilization": N, "resets_at": "..."} for a top-level window. */
static int usage_window(const char *doc, const char *key, double *pct, time_t *reset) {
    const char *vs, *ve;
    char *seg, iso[64];
    size_t n;
    int ok;

    if (json_find_member(doc, key, &vs, &ve) != 1 || *vs != '{') return 0;
    n = (size_t)(ve - vs);
    seg = (char*)malloc(n + 1);
    if (!seg) return 0;
    memcpy(seg, vs, n);
    seg[n] = '\0';

    ok = json_number_after(seg, "\"utilization\"", pct);
    json_string_after(seg, "\"resets_at\"", iso, sizeof(iso));
    *reset = iso[0] ? parse_iso8601(iso) : 0;
    free(seg);
    return ok;
}

static char *https_get(const wchar_t *host, const wchar_t *path, const wchar_t *headers, DWORD *status) {
    HINTERNET session = NULL, conn = NULL, req = NULL;
    char *body = NULL;
    size_t len = 0;
    DWORD size = sizeof(*status), avail, got;

    *status = 0;
    session = WinHttpOpen(L"ClaudeUsageMonitor/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) goto done;
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 15000);
    conn = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) goto done;
    req = WinHttpOpenRequest(conn, L"GET", path, NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) goto done;
    if (!WinHttpSendRequest(req, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) goto done;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, status, &size, WINHTTP_NO_HEADER_INDEX);

    body = (char*)malloc(1);
    while (body && WinHttpQueryDataAvailable(req, &avail) && avail > 0 && len < 4 * 1024 * 1024) {
        char *grown = (char*)realloc(body, len + avail + 1);
        if (!grown) { free(body); body = NULL; break; }
        body = grown;
        if (!WinHttpReadData(req, body + len, avail, &got) || got == 0) break;
        len += got;
    }
    if (body) body[len] = '\0';

done:
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    return body;
}

/* Returns the delay in ms before the next poll. */
static DWORD poll_usage_once(DWORD last_delay) {
    char user[PATH_CAP], path[PATH_CAP], dir[PATH_CAP], token[4096], out[512];
    wchar_t headers[4400];
    char *creds, *body;
    DWORD status;
    double five = 0, seven = 0;
    time_t five_reset = 0, seven_reset = 0;
    int have_five, have_seven, n;

    /* Re-read every time: Claude Code rotates the token in this file. */
    if (!get_env_path("USERPROFILE", user, sizeof(user))) return USAGE_POLL_MS;
    snprintf(path, sizeof(path), "%s\\.claude\\.credentials.json", user);
    creds = read_file_all(path, 1024 * 1024, NULL);
    token[0] = '\0';
    if (creds) {
        json_string_after(creds, "\"accessToken\"", token, sizeof(token));
        SecureZeroMemory(creds, strlen(creds));
        free(creds);
    }
    if (!token[0]) { InterlockedExchange(&g_api_state, API_NO_CREDS); return USAGE_POLL_MS; }

    n = _snwprintf(headers, sizeof(headers) / sizeof(headers[0]),
        L"Authorization: Bearer %hs\r\nanthropic-beta: oauth-2025-04-20\r\nAccept: application/json\r\n", token);
    SecureZeroMemory(token, sizeof(token));
    if (n < 0) return USAGE_POLL_MS;

    body = https_get(L"api.anthropic.com", L"/api/oauth/usage", headers, &status);
    SecureZeroMemory(headers, sizeof(headers));

    if (status == 401 || status == 403) {
        free(body);
        InterlockedExchange(&g_api_state, API_AUTH);
        return USAGE_POLL_MS;
    }
    if (status != 200 || !body) {
        DWORD next = last_delay * 2;
        free(body);
        if (g_api_state != API_OK) InterlockedExchange(&g_api_state, API_NET);
        if (next < USAGE_POLL_MS) next = USAGE_POLL_MS;
        return next > USAGE_BACKOFF_MAX ? USAGE_BACKOFF_MAX : next;
    }

    have_five = usage_window(body, "five_hour", &five, &five_reset);
    have_seven = usage_window(body, "seven_day", &seven, &seven_reset);
    free(body);

    /* Same shape as Claude Code's status-line payload, so read_rate_file handles both. */
    n = snprintf(out, sizeof(out), "{\"source\":\"oauth_usage\",\"updated_at\":%lld,\"rate_limits\":{", (long long)time(NULL));
    if (have_five)
        n += snprintf(out + n, sizeof(out) - n, "\"five_hour\":{\"used_percentage\":%.1f,\"resets_at\":%lld}%s",
            five, (long long)five_reset, have_seven ? "," : "");
    if (have_seven)
        n += snprintf(out + n, sizeof(out) - n, "\"seven_day\":{\"used_percentage\":%.1f,\"resets_at\":%lld}",
            seven, (long long)seven_reset);
    snprintf(out + n, sizeof(out) - n, "}}\n");

    if (get_status_dir(dir, sizeof(dir))) {
        make_dir_recursive(dir);
        snprintf(path, sizeof(path), "%s\\status.json", dir);
        write_file_atomic(path, out, strlen(out));
    }
    InterlockedExchange(&g_api_state, API_OK);
    return USAGE_POLL_MS;
}

static DWORD WINAPI usage_poller(LPVOID arg) {
    DWORD delay = USAGE_POLL_MS;
    (void)arg;
    for (;;) {
        delay = poll_usage_once(delay);
        Sleep(delay);
    }
}

static void enable_dpi_awareness(void) {
    HMODULE u32 = LoadLibraryA("user32.dll");
    if (!u32) return;
    {
        typedef BOOL(WINAPI *SetCtxFn)(HANDLE);
        SetCtxFn set_ctx = (SetCtxFn)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (set_ctx) {
            /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
            if (set_ctx((HANDLE)(LONG_PTR)(-4))) return;
        }
    }
    {
        typedef BOOL(WINAPI *SetAwareFn)(void);
        SetAwareFn set_aware = (SetAwareFn)GetProcAddress(u32, "SetProcessDPIAware");
        if (set_aware) set_aware();
    }
}

static int get_system_dpi(void) {
    int dpi = 96;
    HDC hdc = GetDC(NULL);
    if (hdc) {
        dpi = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(NULL, hdc);
    }
    return dpi > 0 ? dpi : 96;
}

static int run_gui(HINSTANCE hinst, int show_cmd) {
    WNDCLASSEXA wc;
    HWND hwnd;
    MSG msg;
    int screen_w, screen_h, x, y, win_w, win_h;

    enable_dpi_awareness();
    g_dpi = get_system_dpi();
    create_fonts();

    /* Measure the content to size the window precisely instead of guessing pixels. */
    measure_window_size(&win_w, &win_h);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hIcon = LoadIconA(hinst, MAKEINTRESOURCEA(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = "ClaudeUsageMonitorWnd";
    RegisterClassExA(&wc);

    {
        RECT wr = { 0, 0, win_w, win_h };
        DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
        AdjustWindowRectEx(&wr, style, FALSE, 0);
        win_w = wr.right - wr.left;
        win_h = wr.bottom - wr.top;

        screen_w = GetSystemMetrics(SM_CXSCREEN);
        screen_h = GetSystemMetrics(SM_CYSCREEN);
        x = (screen_w - win_w) / 2;
        y = (screen_h - win_h) / 2;

        hwnd = CreateWindowExA(0, wc.lpszClassName, "Claude Usage Monitor v" APP_VERSION,
            style, x, y, win_w, win_h, NULL, NULL, hinst, NULL);
    }
    if (!hwnd) return 1;

    {
        COLORREF caption_color = RGB(20, 20, 22);
        COLORREF text_color = RGB(90, 200, 255);
        DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption_color, sizeof(caption_color));
        DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text_color, sizeof(text_color));
    }

    ShowWindow(hwnd, show_cmd);
    UpdateWindow(hwnd);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}

int main(int argc, char **argv) {
    char user[PATH_CAP];

    /* "/bridge" is kept for old configs; Git Bash rewrites it into a path, so new ones use "--bridge". */
    if (argc > 1 && (!_stricmp(argv[1], "--bridge") || !_stricmp(argv[1], "/bridge"))) {
        return bridge_mode();
    }

    install_statusline();
    CloseHandle(CreateThread(NULL, 0, usage_poller, NULL, 0, NULL));

    if (!get_env_path("USERPROFILE", user, sizeof(user))) {
        MessageBoxA(NULL, "Could not determine USERPROFILE.", "Claude Usage Monitor", MB_OK | MB_ICONERROR);
        return 1;
    }
    snprintf(g_root, sizeof(g_root), "%s\\.claude\\projects", user);

    return run_gui(GetModuleHandleA(NULL), SW_SHOWNORMAL);
}

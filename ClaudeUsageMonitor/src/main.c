/*
    Claude Usage Monitor
    C11 / Visual Studio 2022
    No third-party libraries. Native Win32 GUI, no console window.
*/

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>

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
    char local[PATH_CAP], path[PATH_CAP];
    FILE *f;
    long size;
    char *data;
    char *p;
    double v;

    memset(r, 0, sizeof(*r));
    if (!get_env_path("LOCALAPPDATA", local, sizeof(local))) return 0;
    snprintf(path, sizeof(path), "%s\\ClaudeUsageMonitor\\status.json", local);

    f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    if (size <= 0 || size > 2 * 1024 * 1024) { fclose(f); return 0; }
    rewind(f);

    data = (char*)malloc((size_t)size + 1);
    if (!data) { fclose(f); return 0; }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data); fclose(f); return 0;
    }
    data[size] = '\0';
    fclose(f);

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
    read_rate_file(&g_rates);
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

    text_out(hdc, left, y, cyan, g_font_title, "CLAUDE USAGE MONITOR");
    y += SX(20);
    if (!g_measuring) {
        RECT line = { left, y, client->right - left, y + SX(2) };
        HBRUSH accent = CreateSolidBrush(cyan);
        FillRect(hdc, &line, accent);
        DeleteObject(accent);
    }
    y += SX(10);

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

        snprintf(buf, sizeof(buf), "In: %8s   Out: %8s", inbuf, outbuf);
        text_out(hdc, left + SX(8), y, white, g_font_body, buf);
        y += SX(15);
        snprintf(buf, sizeof(buf), "Cache:%8s   Tot: %8s", cachebuf, totalbuf);
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
        text_out(hdc, left + SX(8), y, dim, g_font_body, "Live 5-hour/7-day data not available yet.");
        y += SX(15);
        text_out(hdc, left + SX(8), y, dim, g_font_body, "Configure Claude Code's statusLine to use /bridge.");
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
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        on_paint(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static int bridge_mode(void) {
    char local[PATH_CAP], dir[PATH_CAP], path[PATH_CAP];
    FILE *f;
    int c;

    if (!get_env_path("LOCALAPPDATA", local, sizeof(local))) return 1;
    snprintf(dir, sizeof(dir), "%s\\ClaudeUsageMonitor", local);
    make_dir_recursive(dir);
    snprintf(path, sizeof(path), "%s\\status.json", dir);

    f = fopen(path, "wb");
    if (!f) return 1;

    while ((c = getchar()) != EOF)
        fputc(c, f);
    fclose(f);

    /* Claude Code expects status-line output; keep it unobtrusive. */
    printf("Usage monitor");
    return 0;
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

    g_font_title = CreateFontA(-SX(17), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH, "Segoe UI");
    g_font_header = CreateFontA(-SX(13), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH, "Segoe UI");
    g_font_body = CreateFontA(-SX(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH, "Consolas");

    /* Measure the content to size the window precisely instead of guessing pixels. */
    {
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

        win_w = (int)((g_max_x + left + SX(16)) * 0.9);
        win_h = final_y + SX(10);
    }

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hIcon = LoadIconA(NULL, (LPCSTR)IDI_APPLICATION);
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

        hwnd = CreateWindowExA(0, wc.lpszClassName, "Claude Usage Monitor",
            style, x, y, win_w, win_h, NULL, NULL, hinst, NULL);
    }
    if (!hwnd) return 1;

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

    if (argc > 1 && !_stricmp(argv[1], "/bridge")) {
        return bridge_mode();
    }

    if (!get_env_path("USERPROFILE", user, sizeof(user))) {
        MessageBoxA(NULL, "Could not determine USERPROFILE.", "Claude Usage Monitor", MB_OK | MB_ICONERROR);
        return 1;
    }
    snprintf(g_root, sizeof(g_root), "%s\\.claude\\projects", user);

    return run_gui(GetModuleHandleA(NULL), SW_SHOWNORMAL);
}

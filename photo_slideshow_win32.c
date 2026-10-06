/*
 * photo_slideshow_win32.c
 *
 * A real Win32 GUI application (no console window, ever -- same
 * -mwindows mechanism as the earlier tools in this project).
 *
 * Lets you pick JPEGs (e.g. from a Canon Rebel T7 -- RAW/.CR2 files
 * are NOT handled here: decoding Canon RAW would require a new
 * external library, which breaks the "no extra libraries" rule.
 * Since the T7 writes a JPEG alongside every RAW by default, this
 * works from those), check which ones to include, and set EACH
 * photo's own display duration independently via ITS OWN dropdown --
 * every row gets a real, separate, always-visible ComboBox control,
 * not one shared control that moves around. Picking a value in one
 * row's dropdown never touches any other row.
 *
 * No image decoding library is used -- ffmpeg's own "concat" demuxer
 * reads the JPEGs directly and handles per-image timing itself, so
 * this needs nothing beyond the Win32 API (including its built-in
 * ListView and ComboBox common controls) and ffmpeg.exe.
 *
 * Implementation note on the per-row dropdowns: each one is a real,
 * separate ComboBox window, parented to the main window and
 * positioned on top of its row. A lightweight timer re-checks their
 * positions several times a second so they track the ListView if you
 * scroll it -- simple and robust, no window subclassing needed. To
 * keep startup fast even with many rows, each dropdown's full list of
 * 3600 duration options is only built the first time YOU open that
 * specific row's dropdown, not for all rows up front.
 *
 * DEPENDENCIES (already have these from the earlier tools -- nothing
 * new to download): ffmpeg.exe, next to this .exe or on PATH.
 *
 * COMPILE (same MSYS2 MinGW64 shell as the earlier tools):
 *
 *   gcc photo_slideshow_win32.c -o PhotoSlideshow.exe -mwindows -lcomdlg32 -lcomctl32
 *
 * UNVERIFIED against a real Windows build -- no Windows machine or
 * cross-compiler available here to test. Careful draft, not a
 * guaranteed first-compile success.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ID_BUTTON_ADD       3001
#define ID_LISTVIEW         3002
#define ID_BUTTON_BUILD     3003
#define ID_STATIC_STATUS    3004
#define ID_TIMER_REPOSITION 3005
#define ID_EDIT_FRAMES      3006
#define ID_BUTTON_APPLY_SEL 3007
#define ID_BUTTON_APPLY_ALL 3008

#define COL_CHECK    0
#define COL_FILENAME 1
#define COL_DURATION 2

#define MAX_PHOTOS 500
/* Output mode: HD = 1920x1080 @ 60 fps, SD = 720x480 @ 30 fps. A "frame" in
 * the duration lists is always one frame of the CURRENT mode (SD: 30 = 1s). */
static int g_fps = 60;
#define FPS g_fps
#define ID_RADIO_HD 3010
#define ID_BUTTON_REMOVE    3016
#define ID_BUTTON_NEWLIST   3017
#define ID_EDIT_RAND_MIN    3012
#define ID_EDIT_RAND_MAX    3013
#define ID_BUTTON_RAND_SEL  3014
#define ID_BUTTON_RAND_ALL  3015
#define ID_RADIO_SD 3011
#define MIN_FRAMES 1
#define MAX_FRAMES (FPS * 60) /* up to 60 seconds */

static HWND g_hStatus;
static HWND g_hListView;
static HINSTANCE g_hInstance;

static char g_photoPaths[MAX_PHOTOS][MAX_PATH];
static int  g_rowFrames[MAX_PHOTOS];   /* THE source of truth for each row's duration */
static HWND g_rowCombos[MAX_PHOTOS];   /* one real, separate ComboBox per row */
static int  g_photoCount = 0;

/* Last-applied position/visibility per row, so the reposition timer
 * can skip touching a combo that hasn't actually moved -- calling
 * MoveWindow/ShowWindow on an unchanged window every tick is exactly
 * what causes visible flicker. */
static RECT g_rowComboLastRect[MAX_PHOTOS];
static BOOL g_rowComboLastVisible[MAX_PHOTOS];
static BOOL g_rowComboRectKnown[MAX_PHOTOS];

/* While a row's dropdown list is actually open, the reposition timer
 * must leave that combo completely untouched -- forcing a z-order
 * change on a combo box while it's dropped down can make Windows
 * auto-close it, which would silently hide the very options list
 * you're trying to look at. -1 means no dropdown is currently open. */
static int g_openDropdownRow = -1;

static HWND g_hFramesEdit, g_hRandMin, g_hRandMax;
/* Set while adding many rows at once so we don't reposition every combo
 * after every single add (that's O(n^2) and very slow for hundreds). */
static BOOL g_suppressReposition = FALSE;

static void SetStatus(const char *msg)
{
    SetWindowTextA(g_hStatus, msg);
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
}

/* Runs ffmpeg hidden, sending its stdout+stderr to logPath so a failure
 * can actually be diagnosed instead of silently swallowed. */
static BOOL RunFfmpegHidden(const char *args, const char *logPath)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmdline[8192];

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE hLog = CreateFileA(logPath, GENERIC_WRITE, FILE_SHARE_READ, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (hLog != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = hLog;
        si.hStdError = hLog;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    ZeroMemory(&pi, sizeof(pi));

    snprintf(cmdline, sizeof(cmdline), "ffmpeg.exe -hide_banner -nostdin %s", args);

    BOOL ok = CreateProcessA(
        NULL, cmdline, NULL, NULL, TRUE, /* inherit handles so the log works */
        CREATE_NO_WINDOW,
        NULL, NULL, &si, &pi
    );

    if (!ok) {
        if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);
        return FALSE;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);

    return (exitCode == 0);
}

/* Copies the last non-empty line of the ffmpeg log into out. */
static void ReadLastLogLine(const char *logPath, char *out, size_t outSize)
{
    out[0] = '\0';
    FILE *f = fopen(logPath, "rb");
    if (!f) { snprintf(out, outSize, "(no ffmpeg log available)"); return; }
    char line[512], last[512] = {0};
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (n) strcpy(last, line);
    }
    fclose(f);
    snprintf(out, outSize, "%s", last[0] ? last : "(ffmpeg log was empty)");
}

static const char *GetBaseName(const char *path)
{
    const char *slash = strrchr(path, '\\');
    if (!slash) slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* Moves every row's OWN ComboBox to sit exactly on top of that row's
 * Duration cell, in the ListView's CURRENT scrolled position, and
 * hides any whose row has scrolled out of view. Called on a timer so
 * scrolling the list keeps every independent dropdown tracking its
 * own row without needing to subclass the ListView. */
static void RepositionAllRowCombos(HWND hwndMain)
{
    if (g_suppressReposition) return;

    RECT lvClient;
    GetClientRect(g_hListView, &lvClient);

    for (int row = 0; row < g_photoCount; row++) {
        HWND combo = g_rowCombos[row];
        if (!combo) continue;
        if (row == g_openDropdownRow) continue; /* leave an open dropdown completely alone */

        RECT rc;
        BOOL shouldBeVisible = TRUE;

        if (!ListView_GetSubItemRect(g_hListView, row, COL_DURATION, LVIR_BOUNDS, &rc)) {
            shouldBeVisible = FALSE;
        } else if (rc.bottom <= lvClient.top || rc.top >= lvClient.bottom) {
            shouldBeVisible = FALSE; /* scrolled out of view */
        }

        if (!shouldBeVisible) {
            if (!g_rowComboRectKnown[row] || g_rowComboLastVisible[row]) {
                ShowWindow(combo, SW_HIDE);
                g_rowComboLastVisible[row] = FALSE;
                g_rowComboRectKnown[row] = TRUE;
            }
            continue;
        }

        POINT pts[2];
        pts[0].x = rc.left;  pts[0].y = rc.top;
        pts[1].x = rc.right; pts[1].y = rc.bottom;
        MapWindowPoints(g_hListView, hwndMain, pts, 2);

        int width = pts[1].x - pts[0].x;
        int rowHeight = pts[1].y - pts[0].y;
        int comboHeight = rowHeight + 300; /* includes the dropped-down list portion */

        RECT newRect = { pts[0].x, pts[0].y, pts[0].x + width, pts[0].y + comboHeight };

        BOOL positionChanged = !g_rowComboRectKnown[row] ||
            newRect.left != g_rowComboLastRect[row].left ||
            newRect.top != g_rowComboLastRect[row].top ||
            newRect.right != g_rowComboLastRect[row].right ||
            newRect.bottom != g_rowComboLastRect[row].bottom;

        if (positionChanged) {
            /* SetWindowPos with HWND_TOP both moves/resizes AND forces
             * this combo back above the ListView in z-order, in one
             * call -- needed because the ListView common control can
             * raise itself back to the top of z-order on its own (on
             * clicks, scrolling, checkbox toggles), which would
             * otherwise silently bury our combo behind it again even
             * though nothing here changed its position. */
            SetWindowPos(combo, HWND_TOP, newRect.left, newRect.top, width, comboHeight,
                         SWP_NOACTIVATE);
            g_rowComboLastRect[row] = newRect;
        } else {
            /* Position hasn't changed, but still defend against the
             * ListView having silently re-raised itself above us
             * since the last tick -- z-order only, no move/resize, so
             * this causes no visible flicker. */
            SetWindowPos(combo, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (!g_rowComboRectKnown[row] || !g_rowComboLastVisible[row]) {
            ShowWindow(combo, SW_SHOW);
        }

        g_rowComboLastVisible[row] = TRUE;
        g_rowComboRectKnown[row] = TRUE;
    }
}

/* Adds one photo: a checked ListView row (checkbox + filename), a
 * default 1-second duration, and its OWN independent ComboBox sitting
 * on that row -- created with just a placeholder item so it displays
 * correctly right away without the cost of building all 3600 entries
 * up front for every single row. */
static void AddPhotoToList(const char *fullPath, HWND hwndMain)
{
    if (g_photoCount >= MAX_PHOTOS) {
        SetStatus("ERROR: reached the maximum number of photos this build supports.");
        return;
    }

    int row = g_photoCount;
    strncpy(g_photoPaths[row], fullPath, MAX_PATH - 1);
    g_photoPaths[row][MAX_PATH - 1] = '\0';
    g_rowFrames[row] = FPS; /* default: 1 second */
    g_photoCount++;

    LVITEMA lvi;
    ZeroMemory(&lvi, sizeof(lvi));
    lvi.mask = LVIF_TEXT | LVIF_PARAM;
    lvi.iItem = row;
    lvi.iSubItem = 0;
    lvi.pszText = "";
    lvi.lParam = (LPARAM)row;
    ListView_InsertItem(g_hListView, &lvi);

    ListView_SetItemText(g_hListView, row, COL_FILENAME, (char *)GetBaseName(fullPath));
    ListView_SetItemText(g_hListView, row, COL_DURATION, (char *)"");
    ListView_SetCheckState(g_hListView, row, TRUE);

    HWND combo = CreateWindowExA(0, "COMBOBOX", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
        0, 0, 10, 10,
        hwndMain, NULL, g_hInstance, NULL);

    SetWindowLongPtrA(combo, GWLP_USERDATA, (LONG_PTR)row);

    char defaultEntry[48];
    snprintf(defaultEntry, sizeof(defaultEntry), "%d frames (1.000s)", FPS);
    SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)defaultEntry);
    SendMessageA(combo, CB_SETCURSEL, 0, 0);

    g_rowCombos[row] = combo;

    RepositionAllRowCombos(hwndMain);
}

/* Sets one row's duration (source of truth + its own dropdown display). */
static void SetRowFrames(int row, int frames)
{
    if (frames < MIN_FRAMES) frames = MIN_FRAMES;
    if (frames > MAX_FRAMES) frames = MAX_FRAMES;
    g_rowFrames[row] = frames;

    HWND combo = g_rowCombos[row];
    if (!combo) return;

    if (SendMessageA(combo, CB_GETCOUNT, 0, 0) > 1) {
        SendMessageA(combo, CB_SETCURSEL, frames - 1, 0); /* full list already built */
    } else {
        char entry[48];
        snprintf(entry, sizeof(entry), "%d frames (%.3fs)", frames, (double)frames / (double)FPS);
        SendMessageA(combo, CB_RESETCONTENT, 0, 0);
        SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)entry);
        SendMessageA(combo, CB_SETCURSEL, 0, 0);
    }
}

static void SaveSession(void); /* defined below */

/* Removes every photo, its dropdown, and all per-row state. */
static void ClearAllPhotos(void)
{
    for (int row = 0; row < g_photoCount; row++) {
        if (g_rowCombos[row]) DestroyWindow(g_rowCombos[row]);
        g_rowCombos[row] = NULL;
    }
    ListView_DeleteAllItems(g_hListView);
    g_photoCount = 0;
    g_openDropdownRow = -1;
    memset(g_rowComboLastRect, 0, sizeof(g_rowComboLastRect));
    memset(g_rowComboLastVisible, 0, sizeof(g_rowComboLastVisible));
    memset(g_rowComboRectKnown, 0, sizeof(g_rowComboRectKnown));
}

/* "New / Clear List": start over with an empty list. */
static void OnNewListClicked(HWND hwnd)
{
    if (g_photoCount == 0) { SetStatus("List is already empty."); return; }

    char q[160];
    snprintf(q, sizeof(q), "Clear all %d photos and start a new list?\nTheir durations will be lost.", g_photoCount);
    if (MessageBoxA(hwnd, q, "New list", MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    ClearAllPhotos();
    SaveSession(); /* so the cleared list is what comes back next launch */
    SetStatus("New list. Add photos to begin.");
}

/* "Remove Selected": drops the selected rows, keeps everything else
 * (order, durations, checkboxes). Rebuilds the list from what's left,
 * because rows are indexed by position. */
static void OnRemoveSelectedClicked(HWND hwnd)
{
    static char keepPath[MAX_PHOTOS][MAX_PATH];
    static int  keepFrames[MAX_PHOTOS];
    static BOOL keepChecked[MAX_PHOTOS];
    int keep = 0, removed = 0;

    for (int row = 0; row < g_photoCount; row++) {
        if (ListView_GetItemState(g_hListView, row, LVIS_SELECTED) & LVIS_SELECTED) {
            removed++;
            continue;
        }
        strncpy(keepPath[keep], g_photoPaths[row], MAX_PATH - 1);
        keepPath[keep][MAX_PATH - 1] = '\0';
        keepFrames[keep] = g_rowFrames[row];
        keepChecked[keep] = ListView_GetCheckState(g_hListView, row) ? TRUE : FALSE;
        keep++;
    }

    if (removed == 0) {
        SetStatus("No rows selected -- click rows first (Ctrl+A selects all, Shift+click for a range).");
        return;
    }

    ClearAllPhotos();
    g_suppressReposition = TRUE;
    for (int i = 0; i < keep; i++) {
        AddPhotoToList(keepPath[i], hwnd);
        SetRowFrames(i, keepFrames[i]);
        ListView_SetCheckState(g_hListView, i, keepChecked[i]);
    }
    g_suppressReposition = FALSE;
    RepositionAllRowCombos(hwnd);
    SaveSession();

    char msg[120];
    snprintf(msg, sizeof(msg), "Removed %d photo%s. %d left.", removed, removed == 1 ? "" : "s", keep);
    SetStatus(msg);
}

/* Default random range: 0.25s to 1.5s in the current mode. */
static void SetRandDefaults(void)
{
    char a[16], b[16];
    snprintf(a, sizeof(a), "%d", g_fps / 4);
    snprintf(b, sizeof(b), "%d", g_fps * 3 / 2);
    SetWindowTextA(g_hRandMin, a);
    SetWindowTextA(g_hRandMax, b);
}

static void GetSessionPath(char *out, size_t n)
{
    const char *appdata = getenv("APPDATA");
    if (appdata && *appdata) {
        snprintf(out, n, "%s\\PhotoSlideshow_session.txt", appdata);
    } else {
        char t[MAX_PATH];
        GetTempPathA(MAX_PATH, t);
        snprintf(out, n, "%sPhotoSlideshow_session.txt", t);
    }
}

/* One line per photo: checked<TAB>frames<TAB>full path */
static void SaveSession(void)
{
    char path[MAX_PATH];
    GetSessionPath(path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "#fps\t%d\n", g_fps);
    for (int row = 0; row < g_photoCount; row++) {
        int checked = ListView_GetCheckState(g_hListView, row) ? 1 : 0;
        fprintf(f, "%d\t%d\t%s\n", checked, g_rowFrames[row], g_photoPaths[row]);
    }
    fclose(f);
}

static void LoadSession(HWND hwndMain)
{
    char path[MAX_PATH];
    GetSessionPath(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return;

    g_suppressReposition = TRUE;
    char line[MAX_PATH + 64];
    int loaded = 0, missing = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (line[0] == '#') {
            int fp = 0;
            if (sscanf(line, "#fps\t%d", &fp) == 1 && (fp == 30 || fp == 60)) {
                g_fps = fp;
                CheckRadioButton(hwndMain, ID_RADIO_HD, ID_RADIO_SD, fp == 30 ? ID_RADIO_SD : ID_RADIO_HD);
                SetWindowTextA(g_hFramesEdit, fp == 30 ? "30" : "60");
                SetRandDefaults();
            }
            continue;
        }
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        char *t2 = strchr(t1 + 1, '\t');
        if (!t2) continue;
        *t1 = '\0'; *t2 = '\0';
        int checked = atoi(line);
        int frames = atoi(t1 + 1);
        const char *photo = t2 + 1;

        if (GetFileAttributesA(photo) == INVALID_FILE_ATTRIBUTES) { missing++; continue; }
        if (g_photoCount >= MAX_PHOTOS) break;

        int row = g_photoCount;
        AddPhotoToList(photo, hwndMain);
        if (g_photoCount == row + 1) {
            SetRowFrames(row, frames);
            ListView_SetCheckState(g_hListView, row, checked ? TRUE : FALSE);
            loaded++;
        }
    }
    fclose(f);
    g_suppressReposition = FALSE;
    RepositionAllRowCombos(hwndMain);

    if (loaded > 0) {
        char msg[200];
        snprintf(msg, sizeof(msg), "Restored %d photo%s from your last session%s.",
                 loaded, loaded == 1 ? "" : "s",
                 missing ? " (some files no longer exist and were skipped)" : "");
        SetStatus(msg);
    }
}

/* Bulk-set durations from the Frames box: selected rows, or every row. */
/* Switch HD<->SD. Keeps every photo's duration in SECONDS (frames rescale). */
static void OnSetMode(int newFps)
{
    if (newFps == g_fps) return;
    int oldFps = g_fps;
    int newFrames[MAX_PHOTOS];
    for (int row = 0; row < g_photoCount; row++) {
        long f = ((long)g_rowFrames[row] * newFps + oldFps / 2) / oldFps;
        newFrames[row] = f < 1 ? 1 : (int)f;
    }
    g_fps = newFps;
    for (int row = 0; row < g_photoCount; row++) {
        if (g_rowCombos[row]) SendMessageA(g_rowCombos[row], CB_RESETCONTENT, 0, 0); /* drop old-fps list */
        SetRowFrames(row, newFrames[row]);
    }
    char t[16];
    snprintf(t, sizeof(t), "%d", g_fps);
    SetWindowTextA(g_hFramesEdit, t);
    SetRandDefaults();
    SaveSession();
    SetStatus(newFps == 30 ? "SD mode: 720x480 @ 30 fps (30 frames = 1 second)."
                           : "HD mode: 1920x1080 @ 60 fps (60 frames = 1 second).");
}

/* Gives each target photo its own random duration in [min, max] frames. */
static void OnRandomizeFrames(BOOL selectedOnly)
{
    char buf[32];
    GetWindowTextA(g_hRandMin, buf, sizeof(buf));
    int lo = atoi(buf);
    GetWindowTextA(g_hRandMax, buf, sizeof(buf));
    int hi = atoi(buf);
    if (lo > hi) { int t = lo; lo = hi; hi = t; }
    if (lo < MIN_FRAMES || hi > MAX_FRAMES) {
        char msg[140];
        snprintf(msg, sizeof(msg), "Random range must be between %d and %d frames (at %d fps).",
                 MIN_FRAMES, MAX_FRAMES, FPS);
        SetStatus(msg);
        return;
    }

    int changed = 0;
    for (int row = 0; row < g_photoCount; row++) {
        if (selectedOnly && !(ListView_GetItemState(g_hListView, row, LVIS_SELECTED) & LVIS_SELECTED))
            continue;
        /* two rand() calls so ranges beyond RAND_MAX (32767) still spread evenly */
        unsigned r = ((unsigned)rand() << 15) ^ (unsigned)rand();
        SetRowFrames(row, lo + (int)(r % (unsigned)(hi - lo + 1)));
        changed++;
    }

    if (changed == 0) {
        SetStatus(selectedOnly ? "No rows selected -- click rows (Ctrl+A selects all, Shift+click for a range)." : "No photos to change.");
        return;
    }
    SaveSession();
    char msg[200];
    snprintf(msg, sizeof(msg), "Randomized %d photo%s between %d and %d frames (%.3fs - %.3fs).",
             changed, changed == 1 ? "" : "s", lo, hi, (double)lo / FPS, (double)hi / FPS);
    SetStatus(msg);
}

static void OnApplyFrames(BOOL selectedOnly)
{
    char buf[32];
    GetWindowTextA(g_hFramesEdit, buf, sizeof(buf));
    int frames = atoi(buf);
    if (frames < MIN_FRAMES || frames > MAX_FRAMES) {
        char msg[120];
        snprintf(msg, sizeof(msg), "Enter a frame count from %d to %d (at %d fps) first.",
                 MIN_FRAMES, MAX_FRAMES, FPS);
        SetStatus(msg);
        return;
    }

    int changed = 0;
    for (int row = 0; row < g_photoCount; row++) {
        if (selectedOnly && !(ListView_GetItemState(g_hListView, row, LVIS_SELECTED) & LVIS_SELECTED))
            continue;
        SetRowFrames(row, frames);
        changed++;
    }

    if (changed == 0) {
        SetStatus(selectedOnly ? "No rows selected -- click rows (Ctrl+A selects all, Shift+click for a range)." : "No photos to change.");
        return;
    }
    SaveSession();
    char msg[160];
    snprintf(msg, sizeof(msg), "Set %d photo%s to %d frames (%.3fs).",
             changed, changed == 1 ? "" : "s", frames, (double)frames / (double)FPS);
    SetStatus(msg);
}

static void OnAddPhotosClicked(HWND hwnd)
{
    static char buffer[32768];
    ZeroMemory(buffer, sizeof(buffer));

    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "JPEG Images\0*.jpg;*.jpeg\0All Files\0*.*\0";
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = sizeof(buffer);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;

    if (!GetOpenFileNameA(&ofn)) return; /* user cancelled */

    g_suppressReposition = TRUE;

    char *p = buffer;
    char dir[MAX_PATH];
    strncpy(dir, p, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    p += strlen(p) + 1;

    if (*p == '\0') {
        AddPhotoToList(dir, hwnd); /* single file: 'dir' is already the full path */
    } else {
        while (*p) {
            char fullPath[MAX_PATH];
            snprintf(fullPath, sizeof(fullPath), "%s\\%s", dir, p);
            AddPhotoToList(fullPath, hwnd);
            p += strlen(p) + 1;
        }
    }

    g_suppressReposition = FALSE;
    RepositionAllRowCombos(hwnd);
    SaveSession();
}

/* The first time a specific row's dropdown is opened, swap its single
 * placeholder entry for the full 1-frame-to-3600-frame list and
 * re-select whatever that row's current value is -- so the expensive
 * part only happens for rows you actually interact with. */
static void EnsureComboFullyPopulated(HWND combo, int row)
{
    if (SendMessageA(combo, CB_GETCOUNT, 0, 0) > 1) return; /* already done */

    SendMessageA(combo, CB_RESETCONTENT, 0, 0);
    for (int frames = MIN_FRAMES; frames <= MAX_FRAMES; frames++) {
        char entry[48];
        double secs = (double)frames / (double)FPS;
        snprintf(entry, sizeof(entry), "%d frames (%.3fs)", frames, secs);
        SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)entry);
    }

    int frames = g_rowFrames[row];
    if (frames < MIN_FRAMES) frames = MIN_FRAMES;
    if (frames > MAX_FRAMES) frames = MAX_FRAMES;
    SendMessageA(combo, CB_SETCURSEL, frames - 1, 0);
}

/* Writes a "file '...'" line: forward slashes, and any apostrophe in the
 * path escaped as '\'' so names like "Mom's birthday.jpg" don't break the list. */
static void WriteConcatPath(FILE *f, const char *path)
{
    fputs("file '", f);
    for (const char *c = path; *c; c++) {
        if (*c == '\\') fputc('/', f);
        else if (*c == '\'') fputs("'\\''", f);
        else fputc(*c, f);
    }
    fputs("'\n", f);
    /* Without this the image demuxer snaps durations to a 1/25s grid:
     * short photos get dropped and the rest drift. */
    fprintf(f, "option framerate %d\n", FPS);
}

static BOOL WriteConcatList(const char *listPath, int *outIncludedCount, long *outTotalFrames)
{
    FILE *f = fopen(listPath, "wb");
    if (!f) return FALSE;

    int count = ListView_GetItemCount(g_hListView);
    int included = 0;
    long totalFrames = 0;
    char lastPath[MAX_PATH] = {0};

    for (int i = 0; i < count; i++) {
        if (!ListView_GetCheckState(g_hListView, i)) continue;

        LVITEMA lvi;
        ZeroMemory(&lvi, sizeof(lvi));
        lvi.mask = LVIF_PARAM;
        lvi.iItem = i;
        ListView_GetItem(g_hListView, &lvi);
        int row = (int)lvi.lParam;

        const char *path = g_photoPaths[row];
        double seconds = (double)g_rowFrames[row] / (double)FPS;

        WriteConcatPath(f, path);
        fprintf(f, "duration %.6f\n", seconds);

        strncpy(lastPath, path, sizeof(lastPath) - 1);
        totalFrames += g_rowFrames[row];
        included++;
    }

    if (included > 0) {
        WriteConcatPath(f, lastPath); /* concat-demuxer last-duration workaround */
    }

    fclose(f);
    *outIncludedCount = included;
    *outTotalFrames = totalFrames;
    return TRUE;
}

static void OnBuildClicked(HWND hwnd)
{
    if (ListView_GetItemCount(g_hListView) == 0) {
        SetStatus("Add some photos first.");
        return;
    }

    SaveSession();

    char tempDir[MAX_PATH];
    GetTempPathA(MAX_PATH, tempDir);
    char listPath[MAX_PATH];
    snprintf(listPath, sizeof(listPath), "%sslideshow_concat.txt", tempDir);

    int includedCount = 0;
    long totalFrames = 0;
    if (!WriteConcatList(listPath, &includedCount, &totalFrames)) {
        SetStatus("ERROR: could not write the temporary concat list file.");
        return;
    }
    if (includedCount == 0) {
        SetStatus("No photos are checked -- check at least one to include.");
        return;
    }

    char outPath[MAX_PATH] = {0};
    OPENFILENAMEA sfn;
    ZeroMemory(&sfn, sizeof(sfn));
    sfn.lStructSize = sizeof(sfn);
    sfn.hwndOwner = hwnd;
    sfn.lpstrFilter = "MPEG Video\0*.mpg\0All Files\0*.*\0";
    sfn.lpstrFile = outPath;
    sfn.nMaxFile = sizeof(outPath);
    sfn.Flags = OFN_OVERWRITEPROMPT;
    sfn.lpstrDefExt = "mpg";

    if (!GetSaveFileNameA(&sfn)) {
        SetStatus("Cancelled -- no output file chosen.");
        return;
    }

    SetStatus("Encoding slideshow with ffmpeg...");

    char logPath[MAX_PATH];
    snprintf(logPath, sizeof(logPath), "%sslideshow_ffmpeg.log", tempDir);

    /* Every setting here was tested against ffmpeg with 200 mixed-size photos
     * incl. 1-frame ones, in both modes:
     *  -reinit_filter 0   photos of different size/orientation don't reset the fps filter
     *  fps=N              exact 1/N-second ticks (-fps_mode cfr miscounts short photos
     *                     and can run away on duplicated frames)
     *  -frames:v total    exact length; no stray extra second from the last-photo line
     *  -maxrate ABOVE -b:v  maxrate == b:v triggers the encoder's "impossible bitrate
     *                     constraints, this will fail" error
     *  bufsize            MPEG-2 VBV limit for the level (HD 9781248, SD 1835008) */
    const char *scaleW = (g_fps == 30) ? "720" : "1920";
    const char *scaleH = (g_fps == 30) ? "480" : "1080";
    const char *rate   = (g_fps == 30) ? "-b:v 6M -maxrate 8M -bufsize 1835008 -g 15 -bf 2"
                                       : "-b:v 30M -maxrate 40M -bufsize 9781248 -g 12 -bf 2";
    char args[8192];
    snprintf(args, sizeof(args),
        "-y -reinit_filter 0 -f concat -safe 0 -i \"%s\" "
        "-vf \"scale=%s:%s:force_original_aspect_ratio=decrease,pad=%s:%s:(ow-iw)/2:(oh-ih)/2,setsar=1,fps=%d,format=yuv420p\" "
        "-fps_mode passthrough -frames:v %ld -c:v mpeg2video %s "
        "-f mpeg \"%s\"",
        listPath, scaleW, scaleH, scaleW, scaleH, FPS, totalFrames, rate, outPath);

    if (!RunFfmpegHidden(args, logPath)) {
        char lastLine[512];
        ReadLastLogLine(logPath, lastLine, sizeof(lastLine));
        char errMsg[900];
        snprintf(errMsg, sizeof(errMsg),
            "ERROR: ffmpeg failed: %s  (full log: %s)", lastLine, logPath);
        SetStatus(errMsg);
        return;
    }

    char msg[700];
    snprintf(msg, sizeof(msg), "Done. Wrote %s (%d photo%s included).",
             outPath, includedCount, includedCount == 1 ? "" : "s");
    SetStatus(msg);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_CREATE: {
            CreateWindowA("BUTTON", "Add Photos...",
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                20, 20, 150, 30,
                hwnd, (HMENU)ID_BUTTON_ADD, NULL, NULL);

            CreateWindowA("BUTTON", "Remove Selected",
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                180, 20, 140, 30,
                hwnd, (HMENU)ID_BUTTON_REMOVE, NULL, NULL);
            CreateWindowA("BUTTON", "New / Clear List",
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                330, 20, 140, 30,
                hwnd, (HMENU)ID_BUTTON_NEWLIST, NULL, NULL);

            g_hListView = CreateWindowExA(0, WC_LISTVIEWA, "",
                WS_VISIBLE | WS_CHILD | WS_BORDER | WS_CLIPSIBLINGS | LVS_REPORT | LVS_SHOWSELALWAYS,
                20, 60, 700, 380,
                hwnd, (HMENU)ID_LISTVIEW, NULL, NULL);

            ListView_SetExtendedListViewStyle(g_hListView,
                LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

            LVCOLUMNA col;
            ZeroMemory(&col, sizeof(col));
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

            col.pszText = ""; col.cx = 34; col.iSubItem = COL_CHECK;
            ListView_InsertColumn(g_hListView, COL_CHECK, &col);

            col.pszText = "Filename"; col.cx = 340; col.iSubItem = COL_FILENAME;
            ListView_InsertColumn(g_hListView, COL_FILENAME, &col);

            col.pszText = "Duration (per photo)"; col.cx = 210; col.iSubItem = COL_DURATION;
            ListView_InsertColumn(g_hListView, COL_DURATION, &col);

            CreateWindowA("BUTTON", "Build Slideshow...",
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                20, 450, 180, 35,
                hwnd, (HMENU)ID_BUTTON_BUILD, NULL, NULL);

            g_hStatus = CreateWindowA("STATIC",
                "Ready. Add photos -- each one gets its own duration dropdown. Uncheck any you want left out, then Build.",
                WS_VISIBLE | WS_CHILD,
                20, 545, 700, 50,
                hwnd, (HMENU)ID_STATIC_STATUS, NULL, NULL);

            CreateWindowA("STATIC", "Frames:",
                WS_VISIBLE | WS_CHILD,
                215, 459, 50, 20,
                hwnd, NULL, NULL, NULL);

            g_hFramesEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "60",
                WS_VISIBLE | WS_CHILD | ES_NUMBER | ES_AUTOHSCROLL,
                268, 455, 65, 26,
                hwnd, (HMENU)ID_EDIT_FRAMES, NULL, NULL);

            CreateWindowA("BUTTON", "Apply to Selected",
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                343, 450, 150, 35,
                hwnd, (HMENU)ID_BUTTON_APPLY_SEL, NULL, NULL);

            CreateWindowA("BUTTON", "Apply to All",
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                503, 450, 120, 35,
                hwnd, (HMENU)ID_BUTTON_APPLY_ALL, NULL, NULL);

            SetTimer(hwnd, ID_TIMER_REPOSITION, 100, NULL);

            CreateWindowA("STATIC", "Random frames:",
                WS_VISIBLE | WS_CHILD,
                20, 504, 100, 20,
                hwnd, NULL, NULL, NULL);
            g_hRandMin = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "15",
                WS_VISIBLE | WS_CHILD | ES_NUMBER | ES_AUTOHSCROLL,
                125, 500, 60, 26,
                hwnd, (HMENU)ID_EDIT_RAND_MIN, NULL, NULL);
            CreateWindowA("STATIC", "to",
                WS_VISIBLE | WS_CHILD,
                193, 504, 20, 20,
                hwnd, NULL, NULL, NULL);
            g_hRandMax = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "90",
                WS_VISIBLE | WS_CHILD | ES_NUMBER | ES_AUTOHSCROLL,
                218, 500, 60, 26,
                hwnd, (HMENU)ID_EDIT_RAND_MAX, NULL, NULL);
            CreateWindowA("BUTTON", "Randomize Selected",
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                343, 495, 150, 35,
                hwnd, (HMENU)ID_BUTTON_RAND_SEL, NULL, NULL);
            CreateWindowA("BUTTON", "Randomize All",
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                503, 495, 120, 35,
                hwnd, (HMENU)ID_BUTTON_RAND_ALL, NULL, NULL);
            srand((unsigned)GetTickCount());

            CreateWindowA("BUTTON", "HD 1080p60",
                WS_VISIBLE | WS_CHILD | BS_AUTORADIOBUTTON | WS_GROUP,
                635, 448, 105, 20,
                hwnd, (HMENU)ID_RADIO_HD, NULL, NULL);
            CreateWindowA("BUTTON", "SD 480p30",
                WS_VISIBLE | WS_CHILD | BS_AUTORADIOBUTTON,
                635, 470, 105, 20,
                hwnd, (HMENU)ID_RADIO_SD, NULL, NULL);
            CheckRadioButton(hwnd, ID_RADIO_HD, ID_RADIO_SD, ID_RADIO_HD);

            LoadSession(hwnd); /* bring back last session's photos, checks and durations */

            return 0;
        }
        case WM_TIMER: {
            if (wParam == ID_TIMER_REPOSITION) {
                RepositionAllRowCombos(hwnd);
            }
            return 0;
        }
        case WM_COMMAND: {
            HWND ctl = (HWND)lParam;
            UINT code = HIWORD(wParam);

            /* Route per-row ComboBox notifications by control handle,
             * not by a shared ID -- every row's combo is its own
             * distinct window, identified via GWLP_USERDATA. */
            if (ctl != NULL && (code == CBN_SELCHANGE || code == CBN_DROPDOWN ||
                                 code == CBN_CLOSEUP || code == CBN_SELENDCANCEL)) {
                int row = (int)(intptr_t)GetWindowLongPtrA(ctl, GWLP_USERDATA);
                if (row >= 0 && row < g_photoCount && g_rowCombos[row] == ctl) {
                    if (code == CBN_DROPDOWN) {
                        g_openDropdownRow = row; /* timer must not touch this combo now */
                        EnsureComboFullyPopulated(ctl, row);
                    } else if (code == CBN_CLOSEUP || code == CBN_SELENDCANCEL) {
                        if (g_openDropdownRow == row) g_openDropdownRow = -1;
                    } else { /* CBN_SELCHANGE */
                        int idx = (int)SendMessageA(ctl, CB_GETCURSEL, 0, 0);
                        if (idx >= 0) {
                            g_rowFrames[row] = idx + 1; /* independent to THIS row only */
                            SaveSession();
                        }
                    }
                }
                return 0;
            }

            switch (LOWORD(wParam)) {
                case ID_BUTTON_ADD:
                    OnAddPhotosClicked(hwnd);
                    break;
                case ID_BUTTON_BUILD:
                    OnBuildClicked(hwnd);
                    break;
                case ID_BUTTON_APPLY_SEL:
                    OnApplyFrames(TRUE);
                    break;
                case ID_BUTTON_APPLY_ALL:
                    OnApplyFrames(FALSE);
                    break;
                case ID_BUTTON_REMOVE:
                    OnRemoveSelectedClicked(hwnd);
                    break;
                case ID_BUTTON_NEWLIST:
                    OnNewListClicked(hwnd);
                    break;
                case ID_BUTTON_RAND_SEL:
                    OnRandomizeFrames(TRUE);
                    break;
                case ID_BUTTON_RAND_ALL:
                    OnRandomizeFrames(FALSE);
                    break;
                case ID_RADIO_HD:
                    OnSetMode(60);
                    break;
                case ID_RADIO_SD:
                    OnSetMode(30);
                    break;
            }
            return 0;
        }
        case WM_CLOSE:
            SaveSession(); /* then fall out to DefWindowProc, which destroys the window */
            break;
        case WM_DESTROY:
            KillTimer(hwnd, ID_TIMER_REPOSITION);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance; (void)lpCmdLine;

    g_hInstance = hInstance;

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    const char CLASS_NAME[] = "PhotoSlideshowWindowClass";

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(
        0, CLASS_NAME, "Photo Slideshow Builder",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 760, 650,
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd) return 0;

    ShowWindow(hwnd, nCmdShow);

    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}

/*
 * launcher.exe -- pick the game files and settings, then start the game.
 *
 * The onboarding path: unpack the release, run launcher.exe, point it at
 * the folder of your SSX 3 rip (default.xbe + data\), choose display
 * options, press Play. The choices are kept in launcher.ini next to the
 * launcher and passed to ssx3_recomp.exe as its RECOMP_* settings (the
 * game reads nothing else), so everything here can also be set by hand.
 *
 * Plain Win32 (no runtime to install). Controllers are listed from XInput,
 * refreshed every second; player N is XInput slot N-1, which is how the
 * game assigns them. With "Check for updates" on, one request at start asks
 * GitHub for the latest release and says when it is newer than this one.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <xinput.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include "build_info.h"           /* SSX3_BUILD_COMMIT: v0.2.4, or a commit */

#define GAME_EXE   "ssx3_recomp.exe"
#define TITLE_ID   0x4541003Cu      /* SSX 3, PAL: what this build was made from */
#define RELEASES   "https://github.com/koenhendriks/SSX3-recomp/releases/latest"
#define WM_UPDATE  (WM_APP + 1)      /* the update check's answer (lParam: malloc'd tag) */

enum {
    ID_PATH = 100, ID_BROWSE, ID_STATUS,
    ID_RES, ID_ASPECT, ID_MSAA, ID_ANISO, ID_DRAWDIST, ID_SHADOWS, ID_AUDIO, ID_HUDEDGES, ID_FULLSCREEN, ID_CRT,
    ID_MBLUR, ID_CAM, ID_FPS, ID_HDTEX,
    ID_SKIPINTRO, ID_FASTBOOT, ID_DEBUGMENU, ID_MONEY, ID_UPDATES, ID_LOGLEVEL,
    ID_PAD0, ID_PAD1, ID_PAD2, ID_PAD3, ID_KEYS,
    ID_LOGS, ID_RESET, ID_PLAY, ID_UPDATE,
    ID_TIMER = 1
};

static const struct { const char *label, *value; } k_res[] = {
    {"Auto (your monitor's height)", ""},
    {"480p (original, sharp pixels)", "480"},
    {"720p", "720"},
    {"1080p", "1080"},
    {"1440p", "1440"},
    {"2160p (4K)", "2160"},
};
static const struct { const char *label, *value; } k_aspect[] = {
    {"Auto (follows the window)", "auto"},
    {"16:9", "16:9"},
    {"21:9", "21:9"},
    {"32:9", "32:9"},
    {"4:3 (original)", "0"},
};
static const struct { const char *label, *value; } k_aniso[] = {
    {"Trilinear", "0"},
    {"4x anisotropic", "4"},
    {"8x anisotropic", "8"},
    {"16x anisotropic (sharpest)", "16"},
};
static const struct { const char *label, *value; } k_drawdist[] = {
    {"Original", "1"},
    {"Far (1.5x)", "1.5"},
    {"Maximum (2x)", "2"},
};
static const struct { const char *label, *value; } k_shadows[] = {
    {"Original (128x128)", "1"},
    {"High (512x512)", "4"},
    {"Ultra (1024x1024)", "8"},
};
static const struct { const char *label, *value; } k_audio[] = {
    {"Stereo", "2"},
    {"5.1 surround", "6"},
};
static const struct { const char *label, *value; } k_loglevel[] = {
    {"Errors only", "error"},
    {"Warnings and errors", "warn"},
    {"Normal (what you did, and problems)", "info"},
    {"Detailed (for bug reports)", "debug"},
    {"Everything (large logs)", "trace"},
};
static const struct { const char *label, *value; } k_msaa[] = {
    {"Off", "0"},
    {"2x MSAA", "2"},
    {"4x MSAA (smooth edges)", "4"},
    {"8x MSAA", "8"},
};

static HINSTANCE s_inst;
static HWND s_wnd;
static HFONT s_font, s_bold, s_title;
static int s_dpi = 96;
static char s_dir[MAX_PATH];        /* the launcher's folder */
static char s_ini[MAX_PATH];
static int s_game_ok;
static HBRUSH s_bg;

static int S(int v) { return MulDiv(v, s_dpi, 96); }

/* ---- game folder check -------------------------------------------------- */

static int file_exists(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int dir_exists(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Reads the XBE certificate: title id and name. 0 = not an XBE. */
static int read_xbe(const char *path, unsigned *title_id, char *name, size_t name_size)
{
    unsigned char hdr[0x180], cert[0xA0];
    unsigned base, cert_va;
    FILE *f = fopen(path, "rb");
    int i;
    if (!f)
        return 0;
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || memcmp(hdr, "XBEH", 4)) {
        fclose(f);
        return 0;
    }
    memcpy(&base, hdr + 0x104, 4);
    memcpy(&cert_va, hdr + 0x118, 4);
    if (cert_va < base || fseek(f, (long)(cert_va - base), SEEK_SET)
        || fread(cert, 1, sizeof cert, f) != sizeof cert) {
        fclose(f);
        return 0;
    }
    fclose(f);
    memcpy(title_id, cert + 8, 4);
    for (i = 0; i < 40 && (size_t)i + 1 < name_size; i++) {    /* UTF-16 title name */
        unsigned short c = (unsigned short)(cert[0xC + i * 2] | cert[0xD + i * 2] << 8);
        if (!c)
            break;
        name[i] = c < 128 ? (char)c : '?';
    }
    name[i] = 0;
    return 1;
}

/* Fills the status line; returns 1 when the folder can be played. */
static int check_game(const char *dir, char *msg, size_t n)
{
    char xbe[MAX_PATH], data[MAX_PATH], name[48];
    unsigned id = 0;
    if (!*dir) {
        snprintf(msg, n, "Choose the folder of your SSX 3 game files (the one with default.xbe).");
        return 0;
    }
    snprintf(xbe, sizeof xbe, "%s\\default.xbe", dir);
    snprintf(data, sizeof data, "%s\\data", dir);
    if (!file_exists(xbe)) {
        snprintf(msg, n, "No default.xbe in this folder. Pick the folder that holds default.xbe and data\\.");
        return 0;
    }
    if (!read_xbe(xbe, &id, name, sizeof name)) {
        snprintf(msg, n, "default.xbe could not be read (not an Xbox executable?).");
        return 0;
    }
    if (!dir_exists(data)) {
        snprintf(msg, n, "Found %s, but the data\\ folder next to it is missing.", name);
        return 0;
    }
    if (id != TITLE_ID) {
        snprintf(msg, n, "This is %s (title %08X). This build needs SSX 3, PAL version (4541003C).",
                 *name ? name : "another game", id);
        return 0;
    }
    snprintf(msg, n, "Found %s (PAL). Ready to play.", name);
    return 1;
}

static void update_status(void)
{
    char dir[MAX_PATH], msg[256];
    GetDlgItemTextA(s_wnd, ID_PATH, dir, sizeof dir);
    s_game_ok = check_game(dir, msg, sizeof msg);
    SetDlgItemTextA(s_wnd, ID_STATUS, msg);
    EnableWindow(GetDlgItem(s_wnd, ID_PLAY), s_game_ok);
    InvalidateRect(GetDlgItem(s_wnd, ID_STATUS), NULL, TRUE);
}

static void browse(void)
{
    IFileOpenDialog *dlg = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IFileOpenDialog, (void **)&dlg)))
        return;
    {
        DWORD opt = 0;
        IFileOpenDialog_GetOptions(dlg, &opt);
        IFileOpenDialog_SetOptions(dlg, opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        IFileOpenDialog_SetTitle(dlg, L"Choose the SSX 3 game folder (with default.xbe and data)");
    }
    if (SUCCEEDED(IFileOpenDialog_Show(dlg, s_wnd))) {
        IShellItem *it = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResult(dlg, &it))) {
            PWSTR p = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
                char dir[MAX_PATH], xbe[MAX_PATH];
                WideCharToMultiByte(CP_ACP, 0, p, -1, dir, sizeof dir, NULL, NULL);
                CoTaskMemFree(p);
                /* Picked the disc's root with the game one level down? Use it. */
                snprintf(xbe, sizeof xbe, "%s\\default.xbe", dir);
                if (!file_exists(xbe)) {
                    char sub[MAX_PATH];
                    snprintf(sub, sizeof sub, "%s\\game\\default.xbe", dir);
                    if (file_exists(sub))
                        strncat(dir, "\\game", sizeof dir - strlen(dir) - 1);
                }
                SetDlgItemTextA(s_wnd, ID_PATH, dir);
                update_status();
            }
            IShellItem_Release(it);
        }
    }
    IFileOpenDialog_Release(dlg);
}

/* ---- controllers --------------------------------------------------------- */

static void update_pads(void)
{
    DWORD i;
    for (i = 0; i < 4; i++) {
        XINPUT_STATE xs;
        char t[160];
        if (XInputGetState(i, &xs) == ERROR_SUCCESS) {
            XINPUT_CAPABILITIES cap;
            XINPUT_BATTERY_INFORMATION bat;
            const char *kind = "Controller", *power = "";
            if (XInputGetCapabilities(i, 0, &cap) == ERROR_SUCCESS) {
                switch (cap.SubType) {
                case XINPUT_DEVSUBTYPE_WHEEL:        kind = "Wheel"; break;
                case XINPUT_DEVSUBTYPE_ARCADE_STICK: kind = "Arcade stick"; break;
                case XINPUT_DEVSUBTYPE_FLIGHT_STICK: kind = "Flight stick"; break;
                case XINPUT_DEVSUBTYPE_DANCE_PAD:    kind = "Dance pad"; break;
                case XINPUT_DEVSUBTYPE_GUITAR:
                case XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE:
                case XINPUT_DEVSUBTYPE_GUITAR_BASS:  kind = "Guitar"; break;
                case XINPUT_DEVSUBTYPE_DRUM_KIT:     kind = "Drum kit"; break;
                default: break;
                }
            }
            if (XInputGetBatteryInformation(i, BATTERY_DEVTYPE_GAMEPAD, &bat) == ERROR_SUCCESS) {
                if (bat.BatteryType == BATTERY_TYPE_WIRED)
                    power = ", wired";
                else if (bat.BatteryType != BATTERY_TYPE_DISCONNECTED && bat.BatteryType != BATTERY_TYPE_UNKNOWN)
                    power = bat.BatteryLevel == BATTERY_LEVEL_FULL ? ", battery full"
                          : bat.BatteryLevel == BATTERY_LEVEL_MEDIUM ? ", battery medium"
                          : bat.BatteryLevel == BATTERY_LEVEL_LOW ? ", battery low" : ", battery empty";
            }
            snprintf(t, sizeof t, "Player %lu:  %s connected%s", (unsigned long)i + 1, kind, power);
        } else if (i == 0) {
            snprintf(t, sizeof t, "Player 1:  keyboard (no controller connected)");
        } else {
            snprintf(t, sizeof t, "Player %lu:  -", (unsigned long)i + 1);
        }
        SetDlgItemTextA(s_wnd, ID_PAD0 + i, t);
    }
}

/* ---- settings ------------------------------------------------------------ */

/* Every setting with its launcher.ini key and default: load, save and
 * "Reset to defaults" all go through this table. */
static const struct { int id, combo; const char *key; int def; } k_settings[] = {
    {ID_RES, 1, "resolution", 0},       {ID_ASPECT, 1, "aspect", 0},
    {ID_MSAA, 1, "msaa", 2},            {ID_ANISO, 1, "aniso", 3},
    {ID_DRAWDIST, 1, "draw_distance", 1}, {ID_SHADOWS, 1, "shadows", 1},
    {ID_AUDIO, 1, "audio_output", 0},   {ID_LOGLEVEL, 1, "log_level", 2},
    {ID_FULLSCREEN, 0, "fullscreen", 0}, {ID_CRT, 0, "crt", 0},
    {ID_MBLUR, 0, "motion_blur", 0},    {ID_CAM, 0, "modern_camera", 0},
    {ID_FPS, 0, "fps_counter", 0},      {ID_HDTEX, 0, "hd_textures", 1},
    {ID_HUDEDGES, 0, "hud_edges", 1},   {ID_SKIPINTRO, 0, "skip_intro", 1},
    {ID_FASTBOOT, 0, "fast_boot", 1},   {ID_DEBUGMENU, 0, "debug_menu", 0},
    {ID_MONEY, 0, "money_cheat", 0},    {ID_UPDATES, 0, "check_updates", 1},
};
#define NSETTINGS ((int)(sizeof k_settings / sizeof k_settings[0]))

static void set_control(int i, int v)
{
    if (k_settings[i].combo)
        SendDlgItemMessageA(s_wnd, k_settings[i].id, CB_SETCURSEL, (WPARAM)v, 0);
    else
        CheckDlgButton(s_wnd, k_settings[i].id, v ? BST_CHECKED : BST_UNCHECKED);
}

static int get_control(int i)
{
    if (k_settings[i].combo)
        return (int)SendDlgItemMessageA(s_wnd, k_settings[i].id, CB_GETCURSEL, 0, 0);
    return IsDlgButtonChecked(s_wnd, k_settings[i].id) == BST_CHECKED;
}

static void load_settings(void)
{
    char dir[MAX_PATH] = "";
    int i;
    GetPrivateProfileStringA("launcher", "game_dir", "", dir, sizeof dir, s_ini);
    if (!*dir) {
        /* First run: a game\ folder next to the launcher, if there is one. */
        char cand[MAX_PATH];
        snprintf(cand, sizeof cand, "%s\\game\\default.xbe", s_dir);
        if (file_exists(cand))
            snprintf(dir, sizeof dir, "%s\\game", s_dir);
    }
    SetDlgItemTextA(s_wnd, ID_PATH, dir);
    for (i = 0; i < NSETTINGS; i++)
        set_control(i, (int)GetPrivateProfileIntA("launcher", k_settings[i].key, k_settings[i].def, s_ini));
    update_status();
}

static void save_settings(void)
{
    char dir[MAX_PATH], b[16];
    int i;
    GetDlgItemTextA(s_wnd, ID_PATH, dir, sizeof dir);
    WritePrivateProfileStringA("launcher", "game_dir", dir, s_ini);
    for (i = 0; i < NSETTINGS; i++) {
        snprintf(b, sizeof b, "%d", get_control(i));
        WritePrivateProfileStringA("launcher", k_settings[i].key, b, s_ini);
    }
}

/* Every option back to its default; the game folder stays. */
static void reset_settings(void)
{
    int i;
    if (MessageBoxA(s_wnd, "Put every setting back to its default? The game folder is kept.", "SSX 3",
                    MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    for (i = 0; i < NSETTINGS; i++)
        set_control(i, k_settings[i].def);
    save_settings();
}

/* ---- update check -------------------------------------------------------- */

/* "v0.2.4", "v0.2.4-3-gabc1234-dirty" -> 0x000204; 0 when not a version */
static unsigned version_of(const char *s)
{
    unsigned a, b, c;
    if (!s || (*s != 'v' && *s != 'V') || sscanf(s + 1, "%u.%u.%u", &a, &b, &c) != 3)
        return 0;
    return a << 16 | b << 8 | c;
}

/* The latest release's tag from GitHub's API (one small request, on its
 * own thread so the window never waits; any failure says nothing). */
static DWORD WINAPI update_thread(LPVOID unused)
{
    HINTERNET ses = NULL, con = NULL, req = NULL;
    char *body = NULL, *tag = NULL;
    DWORD got = 0, n;
    (void)unused;
    ses = WinHttpOpen(L"SSX3-recomp-launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses)
        goto out;
    WinHttpSetTimeouts(ses, 5000, 5000, 5000, 5000);
    con = WinHttpConnect(ses, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!con)
        goto out;
    req = WinHttpOpenRequest(con, L"GET", L"/repos/koenhendriks/SSX3-recomp/releases/latest", NULL,
                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req || !WinHttpSendRequest(req, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L,
                                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !WinHttpReceiveResponse(req, NULL))
        goto out;
    if (!(body = (char *)malloc(65536)))
        goto out;
    while (got < 65535 && WinHttpReadData(req, body + got, 65535 - got, &n) && n)
        got += n;
    body[got] = 0;
    {
        const char *t = strstr(body, "\"tag_name\""), *q, *e;
        if (t && (q = strchr(t + 10, '"')) != NULL && (e = strchr(q + 1, '"')) != NULL && e - q < 32) {
            if ((tag = (char *)malloc((size_t)(e - q))) != NULL) {
                memcpy(tag, q + 1, (size_t)(e - q - 1));
                tag[e - q - 1] = 0;
            }
        }
    }
out:
    free(body);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    if (!tag || !PostMessageA(s_wnd, WM_UPDATE, 0, (LPARAM)tag))
        free(tag);
    return 0;
}

static int s_update_newer;

static void update_answer(char *tag)
{
    char t[128];
    unsigned mine = version_of(SSX3_BUILD_COMMIT), latest = version_of(tag);
    s_update_newer = latest && latest > mine;
    if (s_update_newer && mine)
        snprintf(t, sizeof t, "%s is available: click to download", tag);
    else if (s_update_newer)
        snprintf(t, sizeof t, "Latest release: %s (click to open)", tag);
    else
        snprintf(t, sizeof t, "Up to date");
    SetDlgItemTextA(s_wnd, ID_UPDATE, t);
    InvalidateRect(GetDlgItem(s_wnd, ID_UPDATE), NULL, TRUE);
    free(tag);
}

/* ---- launch -------------------------------------------------------------- */

static void play(void)
{
    char exe[MAX_PATH], dir[MAX_PATH], cmd[MAX_PATH + 2];
    int res = (int)SendDlgItemMessageA(s_wnd, ID_RES, CB_GETCURSEL, 0, 0);
    int asp = (int)SendDlgItemMessageA(s_wnd, ID_ASPECT, CB_GETCURSEL, 0, 0);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    snprintf(exe, sizeof exe, "%s\\%s", s_dir, GAME_EXE);
    if (!file_exists(exe)) {
        MessageBoxA(s_wnd, GAME_EXE " was not found next to the launcher.\n"
                    "Keep launcher.exe in the same folder as " GAME_EXE ".",
                    "SSX 3", MB_ICONERROR);
        return;
    }
    save_settings();
    GetDlgItemTextA(s_wnd, ID_PATH, dir, sizeof dir);
    /* The game's settings, inherited through the environment. */
    SetEnvironmentVariableA("RECOMP_GAME_DIR", dir);
    SetEnvironmentVariableA("RECOMP_KEYBOARD", "1");
    SetEnvironmentVariableA("RECOMP_RES", res > 0 && res < (int)(sizeof k_res / sizeof k_res[0])
                                          ? k_res[res].value : NULL);
    SetEnvironmentVariableA("RECOMP_WIDESCREEN", asp >= 0 && asp < (int)(sizeof k_aspect / sizeof k_aspect[0])
                                                 ? k_aspect[asp].value : "auto");
    {
        int aa = (int)SendDlgItemMessageA(s_wnd, ID_MSAA, CB_GETCURSEL, 0, 0);
        SetEnvironmentVariableA("RECOMP_MSAA", aa >= 0 && aa < (int)(sizeof k_msaa / sizeof k_msaa[0])
                                               ? k_msaa[aa].value : NULL);
        aa = (int)SendDlgItemMessageA(s_wnd, ID_ANISO, CB_GETCURSEL, 0, 0);
        SetEnvironmentVariableA("RECOMP_ANISO", aa >= 0 && aa < (int)(sizeof k_aniso / sizeof k_aniso[0])
                                                ? k_aniso[aa].value : NULL);
        aa = (int)SendDlgItemMessageA(s_wnd, ID_DRAWDIST, CB_GETCURSEL, 0, 0);
        SetEnvironmentVariableA("RECOMP_DRAW_DIST", aa >= 0 && aa < (int)(sizeof k_drawdist / sizeof k_drawdist[0])
                                                    ? k_drawdist[aa].value : NULL);
        aa = (int)SendDlgItemMessageA(s_wnd, ID_SHADOWS, CB_GETCURSEL, 0, 0);
        SetEnvironmentVariableA("RECOMP_SHADOW_RES", aa >= 0 && aa < (int)(sizeof k_shadows / sizeof k_shadows[0])
                                                     ? k_shadows[aa].value : NULL);
        aa = (int)SendDlgItemMessageA(s_wnd, ID_AUDIO, CB_GETCURSEL, 0, 0);
        SetEnvironmentVariableA("RECOMP_AUDIO_CHANNELS", aa >= 0 && aa < (int)(sizeof k_audio / sizeof k_audio[0])
                                                         ? k_audio[aa].value : NULL);
    }
    SetEnvironmentVariableA("RECOMP_FULLSCREEN", IsDlgButtonChecked(s_wnd, ID_FULLSCREEN) ? "1" : NULL);
    SetEnvironmentVariableA("RECOMP_CRT", IsDlgButtonChecked(s_wnd, ID_CRT) ? "1" : NULL);
    SetEnvironmentVariableA("RECOMP_MBLUR", IsDlgButtonChecked(s_wnd, ID_MBLUR) ? "0.5" : NULL);
    SetEnvironmentVariableA("RECOMP_CAM", IsDlgButtonChecked(s_wnd, ID_CAM) ? "1" : NULL);
    SetEnvironmentVariableA("RECOMP_FPS_OVERLAY", IsDlgButtonChecked(s_wnd, ID_FPS) ? "1" : NULL);
    SetEnvironmentVariableA("RECOMP_HDTEX", IsDlgButtonChecked(s_wnd, ID_HDTEX) ? NULL : "0");
    SetEnvironmentVariableA("RECOMP_HUD_EDGES", IsDlgButtonChecked(s_wnd, ID_HUDEDGES) ? NULL : "0");
    SetEnvironmentVariableA("RECOMP_SKIP_INTRO", IsDlgButtonChecked(s_wnd, ID_SKIPINTRO) ? "1" : "0");
    SetEnvironmentVariableA("RECOMP_FAST_BOOT", IsDlgButtonChecked(s_wnd, ID_FASTBOOT) ? NULL : "0");
    SetEnvironmentVariableA("RECOMP_DEBUG_MENU", IsDlgButtonChecked(s_wnd, ID_DEBUGMENU) ? "1" : NULL);
    SetEnvironmentVariableA("RECOMP_MONEY_CHEAT", IsDlgButtonChecked(s_wnd, ID_MONEY) ? "1" : NULL);
    {
        int ll = (int)SendDlgItemMessageA(s_wnd, ID_LOGLEVEL, CB_GETCURSEL, 0, 0);
        SetEnvironmentVariableA("RECOMP_LOG_LEVEL", ll >= 0 && ll < (int)(sizeof k_loglevel / sizeof k_loglevel[0])
                                                    ? k_loglevel[ll].value : NULL);
    }

    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    snprintf(cmd, sizeof cmd, "\"%s\"", exe);
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, s_dir, &si, &pi)) {
        char m[256];
        snprintf(m, sizeof m, "Could not start " GAME_EXE " (error %lu).", GetLastError());
        MessageBoxA(s_wnd, m, "SSX 3", MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    DestroyWindow(s_wnd);
}

static void open_logs(void)
{
    /* The game writes its log to logs\ beside the game folder when it is
     * not started from a console (main.c). */
    char dir[MAX_PATH], logs[MAX_PATH], *slash;
    GetDlgItemTextA(s_wnd, ID_PATH, dir, sizeof dir);
    GetFullPathNameA(*dir ? dir : s_dir, sizeof logs, logs, NULL);
    if (*dir && (slash = strrchr(logs, '\\')) != NULL)
        *slash = 0;
    strncat(logs, "\\logs", sizeof logs - strlen(logs) - 1);
    if (!dir_exists(logs)) {
        MessageBoxA(s_wnd, "No logs yet: they appear after the game has run once.", "SSX 3",
                    MB_ICONINFORMATION);
        return;
    }
    {
        /* the folder with this session's file selected (logs\latest.txt) */
        char lp[MAX_PATH + 16], name[MAX_PATH], arg[2 * MAX_PATH];
        FILE *f;
        size_t n = 0;
        snprintf(lp, sizeof lp, "%s\\latest.txt", logs);
        if ((f = fopen(lp, "r")) != NULL) {
            if (fgets(name, sizeof name, f))
                n = strcspn(name, "\r\n");
            fclose(f);
        }
        if (n) {
            name[n] = 0;
            snprintf(arg, sizeof arg, "/select,\"%s\\%s\"", logs, name);
            if ((INT_PTR)ShellExecuteA(s_wnd, "open", "explorer.exe", arg, NULL, SW_SHOWNORMAL) > 32)
                return;
        }
    }
    ShellExecuteA(s_wnd, "open", logs, NULL, NULL, SW_SHOWNORMAL);
}

/* ---- window -------------------------------------------------------------- */

static HWND add(const char *cls, const char *text, DWORD style, int id, int x, int y, int w, int h, HFONT f)
{
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                             s_wnd, (HMENU)(INT_PTR)id, s_inst, NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)f, TRUE);
    return c;
}

static void make_fonts(void)
{
    NONCLIENTMETRICSA m;
    LOGFONTA lf;
    m.cbSize = sizeof m;
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof m, &m, 0);
    lf = m.lfMessageFont;
    lf.lfHeight = -MulDiv(9, s_dpi, 72);
    s_font = CreateFontIndirectA(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    s_bold = CreateFontIndirectA(&lf);
    lf.lfHeight = -MulDiv(16, s_dpi, 72);
    lf.lfWeight = FW_BOLD;
    s_title = CreateFontIndirectA(&lf);
}

/* A label and a dropdown filled from a table, one row. */
#define LCOMBO(text, id, table, x, y) do { \
        HWND cb_; int i_; \
        add("STATIC", text, 0, -1, (x), (y) + 4, 115, 20, s_font); \
        cb_ = add("COMBOBOX", "", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, id, (x) + 120, (y), 270, 200, s_font); \
        for (i_ = 0; i_ < (int)(sizeof table / sizeof table[0]); i_++) \
            SendMessageA(cb_, CB_ADDSTRING, 0, (LPARAM)table[i_].label); \
    } while (0)
#define LCHECK(text, id, x, y) add("BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, id, (x), (y), 390, 22, s_font)

static void build(void)
{
    int y = 14, ya, yb, i, xa = 20, xb = 450;
    HWND cb;
    char auto_res[96], sub[128];
    add("STATIC", "SSX 3", 0, -1, 20, y, 300, 30, s_title);
    snprintf(sub, sizeof sub, "static recompilation for Windows   %s", SSX3_BUILD_COMMIT);
    add("STATIC", sub, 0, -1, 104, y + 11, 330, 18, s_font);
    add("STATIC", "", SS_NOTIFY | SS_RIGHT, ID_UPDATE, 440, y + 11, 400, 18, s_font);
    y += 44;

    add("STATIC", "Game files", 0, -1, 20, y, 400, 18, s_bold);
    y += 22;
    add("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, ID_PATH, 20, y, 722, 24, s_font);
    add("BUTTON", "Browse...", BS_PUSHBUTTON | WS_TABSTOP, ID_BROWSE, 750, y - 1, 90, 26, s_font);
    y += 30;
    add("STATIC", "", 0, ID_STATUS, 20, y, 820, 20, s_font);
    y += 30;

    /* left column: how it looks */
    ya = y;
    add("STATIC", "Display", 0, -1, xa, ya, 400, 18, s_bold);
    ya += 24;
    LCOMBO("Resolution", ID_RES, k_res, xa, ya);
    cb = GetDlgItem(s_wnd, ID_RES);
    snprintf(auto_res, sizeof auto_res, "Auto (your monitor: %dp)", GetSystemMetrics(SM_CYSCREEN));
    SendMessageA(cb, CB_DELETESTRING, 0, 0);
    SendMessageA(cb, CB_INSERTSTRING, 0, (LPARAM)auto_res);
    ya += 30;
    LCOMBO("Aspect ratio", ID_ASPECT, k_aspect, xa, ya);
    ya += 32;
    LCHECK("Fullscreen (F11 toggles in game)", ID_FULLSCREEN, xa, ya);
    ya += 24;
    LCHECK("HUD at the screen edges (widescreen)", ID_HUDEDGES, xa, ya);
    ya += 24;
    LCHECK("FPS counter (F3 toggles in game)", ID_FPS, xa, ya);
    ya += 24;
    LCHECK("CRT TV shader (needs a resolution above 480p)", ID_CRT, xa, ya);
    ya += 34;

    add("STATIC", "Graphics", 0, -1, xa, ya, 400, 18, s_bold);
    ya += 24;
    LCOMBO("Anti-aliasing", ID_MSAA, k_msaa, xa, ya);
    ya += 30;
    LCOMBO("Texture filtering", ID_ANISO, k_aniso, xa, ya);
    ya += 30;
    LCOMBO("Draw distance", ID_DRAWDIST, k_drawdist, xa, ya);
    ya += 30;
    LCOMBO("Shadow quality", ID_SHADOWS, k_shadows, xa, ya);
    ya += 32;
    LCHECK("HD textures (Bl4ckH4nd's HD texture pack)", ID_HDTEX, xa, ya);
    ya += 24;
    LCHECK("Motion blur (riders stay sharp)", ID_MBLUR, xa, ya);
    ya += 24;
    LCHECK("Modern camera (smoother follow, speed FOV, shake)", ID_CAM, xa, ya);
    ya += 24;

    /* right column: sound, game, logs, controllers */
    yb = y;
    add("STATIC", "Audio", 0, -1, xb, yb, 380, 18, s_bold);
    yb += 24;
    LCOMBO("Audio output", ID_AUDIO, k_audio, xb, yb);
    yb += 40;

    add("STATIC", "Game", 0, -1, xb, yb, 380, 18, s_bold);
    yb += 24;
    LCHECK("Skip the intro movies", ID_SKIPINTRO, xb, yb);
    yb += 24;
    LCHECK("Fast boot (shorter title and hard-disk waits)", ID_FASTBOOT, xb, yb);
    yb += 24;
    LCHECK("Debug mode: EA's quick-start menu", ID_DEBUGMENU, xb, yb);
    yb += 24;
    LCHECK("Testing: Home key gives 200,000 money", ID_MONEY, xb, yb);
    yb += 24;
    LCHECK("Check for updates at start (asks GitHub)", ID_UPDATES, xb, yb);
    yb += 34;

    add("STATIC", "Logs", 0, -1, xb, yb, 380, 18, s_bold);
    yb += 24;
    LCOMBO("Log detail", ID_LOGLEVEL, k_loglevel, xb, yb);
    yb += 32;
    add("BUTTON", "Open logs", BS_PUSHBUTTON | WS_TABSTOP, ID_LOGS, xb, yb, 100, 28, s_font);
    add("STATIC", "Saw a problem? Press F12 in game to mark it, then send the newest log.", 0, -1,
        xb + 112, yb - 2, 278, 36, s_font);
    yb += 44;

    add("STATIC", "Controllers", 0, -1, xb, yb, 380, 18, s_bold);
    yb += 22;
    for (i = 0; i < 4; i++, yb += 21)
        add("STATIC", "", 0, ID_PAD0 + i, xb, yb, 390, 20, s_font);
    add("STATIC", "Plug in controllers before pressing Play; two or more enable Multi Play. "
        "The keyboard always works as player 1 (keys.ini).", 0, ID_KEYS, xb, yb + 2, 390, 34, s_font);
    yb += 40;

    y = (ya > yb ? ya : yb) + 8;
    add("STATIC", "In game:  F3 FPS counter   F4 HD textures   F5 draw distance   F6 shadows   "
        "F11 fullscreen   F12 mark a problem (or click both sticks)", 0, -1, 20, y, 820, 20, s_font);
    y += 30;
    add("BUTTON", "Reset to defaults", BS_PUSHBUTTON | WS_TABSTOP, ID_RESET, 20, y, 130, 30, s_font);
    add("BUTTON", "Play", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_PLAY, 720, y, 120, 30, s_bold);
    y += 46;
    {
        RECT r = {0, 0, S(860), S(y)};
        AdjustWindowRectEx(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
        SetWindowPos(s_wnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_BROWSE: browse(); return 0;
        case ID_PATH:
            if (HIWORD(w) == EN_CHANGE)
                update_status();
            return 0;
        case ID_PLAY: if (s_game_ok) play(); return 0;
        case ID_LOGS: open_logs(); return 0;
        case ID_RESET: reset_settings(); return 0;
        case ID_UPDATE:
            if (s_update_newer && HIWORD(w) == STN_CLICKED)
                ShellExecuteA(h, "open", RELEASES, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        break;
    case WM_UPDATE:
        update_answer((char *)l);
        return 0;
    case WM_SETCURSOR:
        if ((HWND)w == GetDlgItem(h, ID_UPDATE) && s_update_newer) {
            SetCursor(LoadCursor(NULL, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_TIMER:
        update_pads();
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        SetBkMode(dc, TRANSPARENT);
        if ((HWND)l == GetDlgItem(h, ID_STATUS))
            SetTextColor(dc, s_game_ok ? RGB(0, 120, 40) : RGB(190, 40, 30));
        else if ((HWND)l == GetDlgItem(h, ID_UPDATE))
            SetTextColor(dc, s_update_newer ? RGB(0, 90, 200) : RGB(110, 110, 110));
        return (LRESULT)s_bg;
    }
    case WM_CLOSE:
        save_settings();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASSA wc;
    MSG msg;
    char *slash;
    HDC dc;
    (void)prev; (void)cmdline;

    s_inst = inst;
    {
        /* Sharp text on high-DPI screens. */
        typedef BOOL (WINAPI *sdpi_t)(HANDLE);
        sdpi_t f = (sdpi_t)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDpiAwarenessContext");
        if (!f || !f((HANDLE)-4))       /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
            SetProcessDPIAware();
    }
    dc = GetDC(NULL);
    s_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    InitCommonControls();

    GetModuleFileNameA(NULL, s_dir, sizeof s_dir);
    if ((slash = strrchr(s_dir, '\\')) != NULL)
        *slash = 0;
    snprintf(s_ini, sizeof s_ini, "%s\\launcher.ini", s_dir);

    s_bg = GetSysColorBrush(COLOR_WINDOW);
    make_fonts();
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = s_bg;
    wc.lpszClassName = "SSX3Launcher";
    RegisterClassA(&wc);
    s_wnd = CreateWindowExA(0, wc.lpszClassName, "SSX 3 Launcher",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, S(860), S(600), NULL, NULL, inst, NULL);
    if (!s_wnd)
        return 1;
    build();
    load_settings();
    update_pads();
    SetTimer(s_wnd, ID_TIMER, 1000, NULL);
    if (IsDlgButtonChecked(s_wnd, ID_UPDATES) == BST_CHECKED)
        CloseHandle(CreateThread(NULL, 0, update_thread, NULL, 0, NULL));
    {
        /* centre on the screen */
        RECT r, wa;
        GetWindowRect(s_wnd, &r);
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
        SetWindowPos(s_wnd, NULL, wa.left + ((wa.right - wa.left) - (r.right - r.left)) / 2,
                     wa.top + ((wa.bottom - wa.top) - (r.bottom - r.top)) / 2, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER);
    }
    ShowWindow(s_wnd, show);
    SetFocus(GetDlgItem(s_wnd, s_game_ok ? ID_PLAY : ID_BROWSE));
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(s_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    CoUninitialize();
    return 0;
}

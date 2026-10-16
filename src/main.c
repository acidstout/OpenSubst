/*
 * OpenSubst - free and open-source virtual drive manager for Windows.
 * Copyright (c) 2026 Rekow IT. MIT License, see LICENSE.
 *
 * main.c - entry point, command line, process capabilities.
 *
 *   OpenSubst                      open the window
 *   OpenSubst X: path [label] ...  create a drive (like SUBST / NET USE)
 *   OpenSubst X: /D                remove a drive
 *   OpenSubst /LIST                list drives
 *   OpenSubst /STARTUP             autostart: restore stored drives
 *   OpenSubst /REGTASK|/UNREGTASK  (elevated) register/remove the UAC task
 *   OpenSubst /UACTASK <payload>   (elevated, from the task) run drive ops
 *   OpenSubst /ELEVATED <payload>  (elevated, from a UAC prompt) same
 */
#include "common.h"
#include "lang.h"
#include "settings.h"
#include "drives.h"
#include "engine.h"
#include "uac.h"
#include "ui.h"

HINSTANCE   g_hinst;
AppSettings g_set;
DriveConfig g_cfg[26];
wchar_t     g_exePath[MAX_PATH];
wchar_t     g_exeDir[MAX_PATH];
BOOL        g_elevated, g_splitToken, g_isAdminNow, g_quiet;

static BOOL s_console;

/* ============================================================
 *  Output
 * ============================================================ */

static void out_text(const wchar_t *s)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode, n;
    if (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        WriteConsoleW(h, s, (DWORD)lstrlenW(s), &n, NULL);
    } else if (h && h != INVALID_HANDLE_VALUE) {
        /* Redirected to a file or pipe: UTF-8, sized for the whole text. */
        int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
        char *buf = len > 1 ? (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)len) : NULL;
        if (buf) {
            if (WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, len, NULL, NULL) > 1)
                WriteFile(h, buf, (DWORD)(len - 1), &n, NULL);
            HeapFree(GetProcessHeap(), 0, buf);
        }
    } else {
        HANDLE con = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (con != INVALID_HANDLE_VALUE) {
            WriteConsoleW(con, s, (DWORD)lstrlenW(s), &n, NULL);
            CloseHandle(con);
        }
    }
}

void App_Message(HWND owner, const wchar_t *text, BOOL error)
{
    if (s_console) {
        out_text(text);
        out_text(L"\r\n");
        return;
    }
    if (g_quiet) return;
    MessageBoxW(owner, text, T(S_APP_TITLE), MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
}

static void attach_console(void)
{
    if (s_console) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        s_console = TRUE;
        out_text(L"\r\n");
    } else {
        /* Output redirected to a file or pipe also counts. */
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) s_console = TRUE;
    }
}

/* ============================================================
 *  Process capabilities
 * ============================================================ */

static void detect_token(void)
{
    HANDLE tok;
    TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
    TOKEN_ELEVATION elev;
    DWORD got;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return;
    GetTokenInformation(tok, TokenElevationType, &type, sizeof(type), &got);
    elev.TokenIsElevated = 0;
    GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &got);
    CloseHandle(tok);

    if (type == TokenElevationTypeFull) {
        g_elevated = TRUE;
        g_isAdminNow = TRUE;
    } else if (type == TokenElevationTypeLimited) {
        g_splitToken = TRUE;
    } else {
        /* No split token: UAC off, built-in Administrator or standard user. */
        SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
        PSID admins = NULL;
        BOOL member = FALSE;
        if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                     0, 0, 0, 0, 0, 0, &admins)) {
            if (!CheckTokenMembership(NULL, admins, &member)) member = FALSE;
            FreeSid(admins);
        }
        g_isAdminNow = member || elev.TokenIsElevated;
    }
}

static void harden_dll_search(void)
{
    typedef BOOL (WINAPI *fnSetDefaultDllDirectories)(DWORD);
    fnSetDefaultDllDirectories p = (fnSetDefaultDllDirectories)(void *)
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetDefaultDllDirectories");
    SetDllDirectoryW(L"");
    if (p) p(LOAD_LIBRARY_SEARCH_SYSTEM32);
}

/* ============================================================
 *  Command line
 * ============================================================ */

typedef struct Cli {
    wchar_t letter;
    const wchar_t *path;
    const wchar_t *label;
    const wchar_t *icon;
    const wchar_t *exportFile;
    const wchar_t *payload;
    BOOL remove, nosave, nocreate, uac, nouac, recycle, autorestore;
    BOOL startup, nowelcome, list, help, regtask, unregtask, uactask, elevatedRun, bad;
} Cli;

static BOOL is_switch(const wchar_t *a, const wchar_t *name)
{
    return (a[0] == L'/' || a[0] == L'-') && lstrcmpiW(a + 1, name) == 0;
}

static void parse_cli(int argc, wchar_t **argv, Cli *c)
{
    int i, positional = 0;
    ZeroMemory(c, sizeof(*c));
    for (i = 1; i < argc; i++) {
        const wchar_t *a = argv[i];
        if (a[0] == L'/' || (a[0] == L'-' && a[1] && a[1] != L'-' && !iswdigit(a[1]))) {
            if (is_switch(a, L"D") || is_switch(a, L"DELETE") || is_switch(a, L"REMOVE")) c->remove = TRUE;
            else if (is_switch(a, L"ICON") && i + 1 < argc) c->icon = argv[++i];
            else if (is_switch(a, L"UAC")) c->uac = TRUE;
            else if (is_switch(a, L"NOUAC")) c->nouac = TRUE;
            else if (is_switch(a, L"RECYCLEBIN") || is_switch(a, L"RECYCLE")) c->recycle = TRUE;
            else if (is_switch(a, L"AUTORESTORE")) c->autorestore = TRUE;
            else if (is_switch(a, L"NOSAVE")) c->nosave = TRUE;
            else if (is_switch(a, L"NOCREATE")) c->nocreate = TRUE;
            else if (is_switch(a, L"STARTUP")) c->startup = TRUE;
            else if (is_switch(a, L"NOWELCOME")) c->nowelcome = TRUE;
            else if (is_switch(a, L"NOMSG")) g_quiet = TRUE;
            else if (is_switch(a, L"LIST")) c->list = TRUE;
            else if (is_switch(a, L"?") || is_switch(a, L"H") || is_switch(a, L"HELP")) c->help = TRUE;
            else if (is_switch(a, L"REGTASK")) c->regtask = TRUE;
            else if (is_switch(a, L"UNREGTASK")) c->unregtask = TRUE;
            else if (is_switch(a, L"UACTASK")) { c->uactask = TRUE; c->payload = i + 1 < argc ? argv[++i] : L""; }
            else if (is_switch(a, L"ELEVATED")) { c->elevatedRun = TRUE; c->payload = i + 1 < argc ? argv[++i] : L""; }
            else if (is_switch(a, L"EXPORTLANG") && i + 1 < argc) c->exportFile = argv[++i];
            else c->bad = TRUE;
            continue;
        }
        if (positional == 0 && IS_DRIVE_LETTER(ch_upper(a[0])) &&
            a[1] == L':' && a[2] == 0) {
            c->letter = ch_upper(a[0]);
            positional = 1;
        } else if (positional == 1) {
            c->path = a;
            positional = 2;
        } else if (positional == 2) {
            c->label = a;
            positional = 3;
        } else {
            c->bad = TRUE;
        }
    }
}

static void notify_running_instance(void)
{
    HWND w = FindWindowW(APP_WNDCLASS, NULL);
    if (w) PostMessageW(w, RegisterWindowMessageW(MSG_REFRESH), 0, 0);
}

static int cli_list(void)
{
    int i, n = 0;
    for (i = 0; i < 26; i++) {
        wchar_t letter = (wchar_t)(L'A' + i), line[PATHBUF + LABELBUF + 64];
        const DriveConfig *c = &g_cfg[i];
        DriveState st;
        Drive_Query(letter, &st);
        if (!c->used && st.kind != DK_SUBST && st.kind != DK_NETWORK && st.kind != DK_NETUNAVAIL)
            continue;
        if (c->used) {
            BOOL mounted = (c->flags & DF_NETWORK)
                ? (st.kind == DK_NETWORK && lstrcmpiW(st.target, c->path) == 0)
                : (st.kind == DK_SUBST && lstrcmpiW(st.target, c->path) == 0);
            StringCchPrintfW(line, ARRAYSIZE(line), L"%c:\\: => %s%s%s%s  [%s%s%s%s]\r\n",
                letter, c->path,
                c->label[0] ? L"  \"" : L"", c->label, c->label[0] ? L"\"" : L"",
                mounted ? L"mounted" : ((c->flags & DF_ENABLED) ? L"not mounted" : L"disabled"),
                (c->flags & DF_UAC) ? L", UAC" : L"",
                (c->flags & DF_RECYCLE) ? L", RecycleBin" : L"",
                (c->flags & DF_AUTORESTORE) ? L", AutoRestore" : L"");
        } else {
            StringCchPrintfW(line, ARRAYSIZE(line), L"%c:\\: => %s  [%s]\r\n", letter, st.target,
                             st.kind == DK_NETUNAVAIL ? L"disconnected, not managed" : L"not managed");
        }
        out_text(line);
        n++;
    }
    return 0;
}

static int cli_remove(const Cli *a)
{
    DriveConfig *c = &g_cfg[LETTER_INDEX(a->letter)];
    DWORD r;
    if (c->used) {
        if (a->nosave) {
            r = Engine_Unmount(a->letter, NULL, TRUE);
        } else {
            Engine_Remove(a->letter, NULL, TRUE);
            r = c->used ? ERROR_CANCELLED : 0;
            if (!c->used) Config_Delete(a->letter);
        }
    } else {
        r = Engine_UnmountForeign(a->letter, NULL, TRUE);
        if (r == ERROR_INVALID_DRIVE) {
            wchar_t msg[256];
            StringCchPrintfW(msg, ARRAYSIZE(msg), T(S_E_UNSUBST), a->letter);
            App_Message(NULL, msg, TRUE);
        }
    }
    Engine_Commit(NULL, TRUE);
    return r ? 1 : 0;
}

static int cli_add(const Cli *a)
{
    wchar_t path[PATHBUF];
    DriveConfig *c = &g_cfg[LETTER_INDEX(a->letter)];
    DriveConfig saved = *c;
    DWORD flags = DF_ENABLED, r = 0;
    BOOL net;

    if (!Path_Normalize(a->path, path, ARRAYSIZE(path))) {
        wchar_t msg[PATHBUF + 64];
        StringCchPrintfW(msg, ARRAYSIZE(msg), L"%s%s", T(S_E_INVALIDPATH), a->path);
        App_Message(NULL, msg, TRUE);
        return 1;
    }
    net = Path_IsUnc(path);
    if (!net && !Path_IsDirectory(path)) {
        wchar_t msg[PATHBUF + 64];
        StringCchPrintfW(msg, ARRAYSIZE(msg), L"%s%s", T(S_E_INVALIDPATH), path);
        App_Message(NULL, msg, TRUE);
        return 1;
    }
    if (net && !(g_set.flags & AF_MANAGENET)) {
        /* Asking for a network drive on the command line is consent enough. */
        g_set.flags |= AF_MANAGENET;
        Settings_Save();
    }

    if (net) flags |= DF_NETWORK;
    if (g_splitToken && (a->uac || (!a->nouac && (g_set.defaultDriveFlags & DF_UAC))) && Uac_TaskRegistered())
        flags |= DF_UAC;
    if (a->recycle && (g_splitToken || g_isAdminNow)) flags |= DF_RECYCLE;
    if (a->autorestore) flags |= DF_AUTORESTORE;

    /* Replacing a stored drive: take the old one down first. */
    if (c->used && !a->nocreate) {
        DriveState st;
        Drive_Query(a->letter, &st);
        if ((st.kind == DK_SUBST || st.kind == DK_NETWORK) && lstrcmpiW(st.target, path) != 0)
            Engine_Unmount(a->letter, NULL, TRUE);
    }

    c->used = TRUE;
    c->letter = a->letter;
    c->flags = flags;
    StringCchCopyW(c->path, ARRAYSIZE(c->path), path);
    StringCchCopyW(c->label, ARRAYSIZE(c->label), a->label ? a->label : (saved.used ? saved.label : L""));
    StringCchCopyW(c->icon, ARRAYSIZE(c->icon), a->icon ? a->icon : (saved.used ? saved.icon : L""));

    if (!a->nocreate) r = Engine_Mount(a->letter, NULL, TRUE);
    if (r == 0 && !a->nosave) Config_Save(a->letter);
    if (r != 0) *c = saved;
    Engine_Commit(NULL, TRUE);
    return r ? 1 : 0;
}

/* ============================================================
 *  Entry point
 * ============================================================ */

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmdLine, int nShow)
{
    int argc = 0, rc = 0;
    wchar_t **argv;
    Cli cli;
    HANDLE mutex;

    (void)hPrev; (void)cmdLine; (void)nShow;
    g_hinst = hInst;
    /* A corrupted heap ends the process instead of being exploitable. */
    HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, NULL, 0);
    harden_dll_search();
    GetModuleFileNameW(NULL, g_exePath, ARRAYSIZE(g_exePath));
    StringCchCopyW(g_exeDir, ARRAYSIZE(g_exeDir), g_exePath);
    PathRemoveFileSpecW(g_exeDir);
    detect_token();

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    parse_cli(argc, argv, &cli);

    Settings_Load();
    if (!Lang_Select(g_set.language)) Lang_Select(L"");

    /* ---- elevated helper modes: no UI ---- */
    if (cli.uactask || cli.elevatedRun) {
        rc = Uac_ElevatedMain(cli.payload, cli.uactask);
        LocalFree(argv);
        return rc;
    }
    if (cli.regtask || cli.unregtask) {
        HRESULT hr;
        if (!g_elevated) {
            DWORD code = 1;
            attach_console();
            if (!Uac_RunElevatedSelf(NULL, cli.regtask ? L"/REGTASK" : L"/UNREGTASK", &code)) code = 1;
            if (s_console)
                App_Message(NULL, code == 0 ? T(cli.regtask ? S_I_REGTASK : S_I_UNREGTASK)
                                            : T(cli.regtask ? S_E_REGTASK : S_E_UNREGTASK), code != 0);
            LocalFree(argv);
            return (int)code;
        }
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        hr = cli.regtask ? Uac_RegisterTask() : Uac_UnregisterTask();
        CoUninitialize();
        LocalFree(argv);
        return (int)hr;
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    Config_LoadAll();

    if (cli.exportFile) {
        attach_console();
        rc = Lang_ExportTemplate(cli.exportFile) ? 0 : 1;
        App_Message(NULL, rc ? T(S_E_LANGFILE) : T(S_I_EXPORTED), rc != 0);
        if (!rc) App_Message(NULL, cli.exportFile, FALSE);
        goto done;
    }
    if (cli.help || cli.bad || (cli.path && !cli.letter) || (cli.letter && !cli.path && !cli.remove)) {
        attach_console();
        if (s_console) {
            App_Message(NULL, T(S_CLI_USAGE), FALSE);
        } else if (!g_quiet) {
            MessageBoxW(NULL, T(S_CLI_USAGE), T(S_APP_TITLE), MB_OK | MB_ICONINFORMATION);
        }
        rc = (cli.help && !cli.bad) ? 0 : 1;
        goto done;
    }
    if (cli.list) {
        attach_console();
        rc = cli_list();
        goto done;
    }
    if (cli.letter) {
        attach_console();
        rc = cli.remove ? cli_remove(&cli) : cli_add(&cli);
        notify_running_instance();
        goto done;
    }

    /* ---- GUI ---- */
    mutex = CreateMutexW(NULL, TRUE, APP_MUTEX);
    /* ACCESS_DENIED: an elevated instance owns the mutex - also "running". */
    if ((mutex && GetLastError() == ERROR_ALREADY_EXISTS) ||
        (!mutex && GetLastError() == ERROR_ACCESS_DENIED)) {
        if (!cli.startup) {
            HWND w = FindWindowW(APP_WNDCLASS, NULL);
            if (w) {
                DWORD pid = 0;
                GetWindowThreadProcessId(w, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(w, RegisterWindowMessageW(MSG_SHOWME), 0, 0);
            }
        }
        if (mutex) CloseHandle(mutex);
        goto done;
    }
    {
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
        InitCommonControlsEx(&icc);
    }
    if (Ui_Create(cli.startup, cli.nowelcome)) rc = Ui_Run();
    else rc = 1;
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }

done:
    CoUninitialize();
    LocalFree(argv);
    return rc;
}

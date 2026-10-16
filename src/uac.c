/*
 * uac.c - UAC task (Task Scheduler 2.0 COM API in plain C) and the
 * elevated operation protocol.
 *
 * Copyright (c) 2026 Rekow IT. MIT License, see LICENSE.
 */
#include "uac.h"
#include "drives.h"
#include "recycle.h"
#include "lang.h"

#include <taskschd.h>
#include <sddl.h>
#include <aclapi.h>

static const CLSID my_CLSID_TaskScheduler =
    { 0x0f87369f, 0xa4e5, 0x4cfc, { 0xbd, 0x3e, 0x73, 0xe6, 0x15, 0x45, 0x72, 0xdd } };
static const IID my_IID_ITaskService =
    { 0x2faba4c7, 0x4da9, 0x4013, { 0x96, 0x97, 0x20, 0xcc, 0x3f, 0xd4, 0x0f, 0x85 } };

#define RESULT_MAGIC    0x4F534231u     /* "OSB1" */
#define MAX_OPS         96              /* 26 letters x (drive, network, recycle) */
#define MAX_CHUNK_HEX   16000           /* keeps every command line well below 32K */
#define ELEV_MUTEX      L"Local\\OpenSubst.ElevatedOps"

static BOOL s_busy;     /* Elev_Flush is running (it pumps messages while waiting) */

BOOL Elev_IsBusy(void) { return s_busy; }

/* ============================================================
 *  Waiting without freezing the window
 * ============================================================ */

/* Wait for 'h'. With an owner window the thread keeps pumping messages so the
   window repaints; the owner is disabled meanwhile, so the user cannot start
   anything else. Returns WAIT_OBJECT_0, WAIT_TIMEOUT or WAIT_FAILED. */
static DWORD wait_pumping(HANDLE h, DWORD ms, HWND owner)
{
    ULONGLONG end = GetTickCount64() + ms;
    BOOL disabled = FALSE, quit = FALSE;
    int quitCode = 0;
    DWORD res = WAIT_TIMEOUT;

    if (!owner || !IsWindow(owner)) return WaitForSingleObject(h, ms);
    if (IsWindowEnabled(owner)) {
        EnableWindow(owner, FALSE);
        disabled = TRUE;
    }
    for (;;) {
        ULONGLONG now = GetTickCount64();
        DWORD left = now >= end ? 0 : (DWORD)(end - now);
        DWORD r = MsgWaitForMultipleObjects(1, &h, FALSE, left, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) { res = WAIT_OBJECT_0; break; }
        if (r == WAIT_OBJECT_0 + 1) {
            MSG m;
            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                if (m.message == WM_QUIT) { quit = TRUE; quitCode = (int)m.wParam; continue; }
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            if (left == 0) break;
            continue;
        }
        res = (r == WAIT_TIMEOUT) ? WAIT_TIMEOUT : WAIT_FAILED;
        break;
    }
    if (disabled && IsWindow(owner)) EnableWindow(owner, TRUE);
    if (quit) PostQuitMessage(quitCode);    /* hand it back to the main loop */
    return res;
}

/* ============================================================
 *  Is this exe safe to run elevated without a prompt?
 * ============================================================ */

/* TRUE if 'tok' has any of 'want' on 'path' - or if that cannot be told
   (missing ACL, FAT drive, error): when in doubt, assume writable. */
static BOOL token_may(HANDLE tok, const wchar_t *path, DWORD want)
{
    static GENERIC_MAPPING gm = { FILE_GENERIC_READ, FILE_GENERIC_WRITE,
                                  FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS };
    PSECURITY_DESCRIPTOR sd = NULL;
    PRIVILEGE_SET ps;
    DWORD psl = sizeof(ps), granted = 0;
    BOOL status = FALSE;

    if (GetNamedSecurityInfoW(path, SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
            DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION,
            NULL, NULL, NULL, NULL, &sd) != ERROR_SUCCESS || !sd)
        return TRUE;
    if (!AccessCheck(sd, tok, MAXIMUM_ALLOWED, &gm, &ps, &psl, &granted, &status)) {
        status = TRUE;      /* unknown: fail safe */
        granted = want;
    }
    LocalFree(sd);
    return status && (granted & want) != 0;
}

/* The token a standard (non-elevated) process of this user has. */
static HANDLE limited_token(void)
{
    HANDLE proc = NULL, out = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &proc)) return NULL;
    if (g_elevated) {
        TOKEN_LINKED_TOKEN lt;
        DWORD got = 0;
        lt.LinkedToken = NULL;
        /* The linked token of an elevated process is the filtered one, and
           it comes back as an identification-level impersonation token. */
        if (GetTokenInformation(proc, TokenLinkedToken, &lt, sizeof(lt), &got)) out = lt.LinkedToken;
    } else {
        if (!DuplicateToken(proc, SecurityIdentification, &out)) out = NULL;
    }
    CloseHandle(proc);
    return out;
}

BOOL Uac_LocationSecure(void)
{
    wchar_t dir[MAX_PATH], parent[MAX_PATH];
    HANDLE tok = limited_token();
    BOOL bad;

    if (!tok) return FALSE;
    StringCchCopyW(dir, ARRAYSIZE(dir), g_exePath);
    PathRemoveFileSpecW(dir);
    StringCchCopyW(parent, ARRAYSIZE(parent), dir);
    PathRemoveFileSpecW(parent);

    /* Replacing the exe, planting a DLL next to it, or renaming its folder
       would each let a standard process run code through the task. */
    bad = token_may(tok, g_exePath, FILE_WRITE_DATA | FILE_APPEND_DATA | WRITE_DAC | WRITE_OWNER | DELETE) ||
          token_may(tok, dir, FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD |
                              WRITE_DAC | WRITE_OWNER | DELETE);
    if (!bad && lstrcmpiW(parent, dir) != 0 && parent[0])
        bad = token_may(tok, parent, FILE_DELETE_CHILD | WRITE_DAC | WRITE_OWNER);
    CloseHandle(tok);
    return !bad;
}

typedef struct ElevResult {
    DWORD magic;
    DWORD done;
    DWORD count;
    DWORD err[MAX_OPS];
} ElevResult;

/* ============================================================
 *  Queue
 * ============================================================ */

typedef struct ElevOp {
    wchar_t op;
    wchar_t letter;
    wchar_t arg[PATHBUF];
} ElevOp;

static ElevOp s_ops[MAX_OPS];
static int    s_nops;

void Elev_Add(wchar_t op, wchar_t letter, const wchar_t *arg)
{
    ElevOp *o;
    int i;
    if (!IS_DRIVE_LETTER(letter)) return;
    /* A later operation on the same letter and kind replaces an earlier one. */
    for (i = 0; i < s_nops; i++) {
        wchar_t a = s_ops[i].op, b = op;
        if (s_ops[i].letter == letter &&
            ch_upper(a) == ch_upper(b)) {
            MoveMemory(&s_ops[i], &s_ops[i + 1], (size_t)(s_nops - i - 1) * sizeof(ElevOp));
            s_nops--;
            break;
        }
    }
    if (s_nops >= MAX_OPS) return;
    o = &s_ops[s_nops++];
    o->op = op;
    o->letter = letter;
    StringCchCopyW(o->arg, ARRAYSIZE(o->arg), arg ? arg : L"");
}

BOOL Elev_Pending(void) { return s_nops > 0; }
void Elev_Discard(void) { s_nops = 0; }

static BOOL is_namespace_op(wchar_t op)
{
    return op == EOP_SUBST || op == EOP_UNSUBST || op == EOP_NET || op == EOP_UNNET;
}

/* ============================================================
 *  Elevated side: validate and execute one operation
 * ============================================================ */

static wchar_t system_letter(void)
{
    wchar_t dir[MAX_PATH];
    if (!GetSystemWindowsDirectoryW(dir, ARRAYSIZE(dir)) || dir[1] != L':') return L'C';
    return ch_upper(dir[0]);
}

static DWORD exec_op(wchar_t op, wchar_t letter, const wchar_t *arg)
{
    DriveState st;
    wchar_t path[PATHBUF];

    if (!IS_DRIVE_LETTER(letter) || letter == system_letter())
        return ERROR_INVALID_DRIVE;
    /* The elevated side runs with System32 as current directory: only
       accept paths that do not depend on it. */
    if ((op == EOP_SUBST || op == EOP_NET) && !Path_IsAbsoluteSpec(arg))
        return ERROR_BAD_PATHNAME;

    switch (op) {
    case EOP_SUBST:
        if (!Path_Normalize(arg, path, ARRAYSIZE(path)) || Path_IsUnc(path))
            return ERROR_BAD_PATHNAME;
        Drive_Query(letter, &st);
        if (st.kind == DK_SUBST) {
            if (lstrcmpiW(st.target, path) == 0) return 0;
            Drive_UnmountSubst(letter);
        } else if (st.kind != DK_NONE) {
            return ERROR_ALREADY_ASSIGNED;
        }
        return Drive_MountSubst(letter, path);

    case EOP_UNSUBST:
        Drive_Query(letter, &st);
        if (st.kind == DK_NONE) return 0;
        if (st.kind != DK_SUBST) return ERROR_INVALID_DRIVE;
        return Drive_UnmountSubst(letter);

    case EOP_NET:
        if (!Path_Normalize(arg, path, ARRAYSIZE(path)) || !Path_IsUnc(path))
            return ERROR_BAD_NETPATH;
        Drive_Query(letter, &st);
        if (st.kind == DK_NETWORK && lstrcmpiW(st.target, path) == 0) return 0;
        if (st.kind == DK_NETWORK || st.kind == DK_NETUNAVAIL)
            Drive_UnmountNet(letter, TRUE);
        else if (st.kind != DK_NONE)
            return ERROR_ALREADY_ASSIGNED;
        return Drive_MountNet(letter, path, NULL, FALSE, NULL, 0);

    case EOP_UNNET:
        Drive_Query(letter, &st);
        if (st.kind == DK_NONE) return 0;
        if (st.kind != DK_NETWORK && st.kind != DK_NETUNAVAIL) return ERROR_INVALID_DRIVE;
        return Drive_UnmountNet(letter, TRUE);

    case EOP_RECYCLE:
        return Recycle_RegisterMachine(letter, TRUE);

    case EOP_UNRECYCLE:
        return Recycle_RegisterMachine(letter, FALSE);
    }
    return ERROR_INVALID_FUNCTION;
}

/* ============================================================
 *  Payload encoding: UTF-8 text, hex encoded
 *     line 1: nonce (16 hex digits)
 *     then:   <op><letter><argument>   one per line
 * ============================================================ */

static wchar_t *encode_payload(const wchar_t *text)
{
    static const wchar_t hex[] = L"0123456789ABCDEF";
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    char *u8;
    wchar_t *out;
    int i;
    if (n <= 0) return NULL;
    u8 = (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)n);
    out = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, ((SIZE_T)n * 2 + 1) * sizeof(wchar_t));
    if (!u8 || !out) {
        if (u8) HeapFree(GetProcessHeap(), 0, u8);
        if (out) HeapFree(GetProcessHeap(), 0, out);
        return NULL;
    }
    WideCharToMultiByte(CP_UTF8, 0, text, -1, u8, n, NULL, NULL);
    n--;    /* without the terminator */
    for (i = 0; i < n; i++) {
        out[i * 2]     = hex[((unsigned char)u8[i]) >> 4];
        out[i * 2 + 1] = hex[((unsigned char)u8[i]) & 15];
    }
    out[n * 2] = 0;
    HeapFree(GetProcessHeap(), 0, u8);
    return out;
}

static int hexval(wchar_t c)
{
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    return -1;
}

static wchar_t *decode_payload(const wchar_t *hex)
{
    int len = lstrlenW(hex), i, n;
    char *u8;
    wchar_t *out;
    if (len == 0 || (len & 1) || len > 64000) return NULL;
    u8 = (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)len / 2 + 1);
    if (!u8) return NULL;
    for (i = 0; i < len / 2; i++) {
        int hi = hexval(hex[i * 2]), lo = hexval(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) { HeapFree(GetProcessHeap(), 0, u8); return NULL; }
        u8[i] = (char)((hi << 4) | lo);
    }
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8, len / 2, NULL, 0);
    out = n > 0 ? (wchar_t *)HeapAlloc(GetProcessHeap(), 0, ((SIZE_T)n + 1) * sizeof(wchar_t)) : NULL;
    if (out) {
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8, len / 2, out, n);
        out[n] = 0;
    }
    HeapFree(GetProcessHeap(), 0, u8);
    return out;
}

static void object_name(wchar_t *out, size_t cch, const wchar_t *kind, const wchar_t *nonce)
{
    StringCchPrintfW(out, cch, L"Local\\OpenSubst.%s.%s", kind, nonce);
}

int Uac_ElevatedMain(const wchar_t *payload, BOOL fromTask)
{
    wchar_t *text;
    wchar_t *p, *nl, nonce[20];
    wchar_t name[96];
    HANDLE map = NULL, ev = NULL, lock;
    ElevResult *res = NULL;
    DWORD count = 0, failed = 0, w;
    int i;

    /* Started by the task (no prompt): refuse to work from a location a
       standard process could tamper with. A UAC prompt (/ELEVATED) is the
       user's explicit consent and needs no such check. The operations are
       still answered (with an error), so the caller does not wait in vain. */
    BOOL refuse = fromTask && !Uac_LocationSecure();

    text = decode_payload(payload ? payload : L"");
    if (!text) return 2;

    /* Nonce: exactly 16 hex digits, used only to build object names. */
    nl = wcschr(text, L'\n');
    if (!nl || nl - text != 16) { HeapFree(GetProcessHeap(), 0, text); return 2; }
    for (i = 0; i < 16; i++) {
        if (hexval(text[i]) < 0) { HeapFree(GetProcessHeap(), 0, text); return 2; }
        nonce[i] = text[i];
    }
    nonce[16] = 0;

    object_name(name, ARRAYSIZE(name), L"Result", nonce);
    map = OpenFileMappingW(FILE_MAP_WRITE, FALSE, name);
    if (map) res = (ElevResult *)MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, sizeof(ElevResult));

    /* Several callers (window, command line) may start the task at once;
       run their operations one after the other. A stuck holder only delays. */
    lock = CreateMutexW(NULL, FALSE, ELEV_MUTEX);
    w = lock ? WaitForSingleObject(lock, 20000) : WAIT_FAILED;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    p = nl + 1;
    while (*p && count < MAX_OPS) {
        wchar_t *line = p, op, letter;
        DWORD r;
        nl = wcschr(p, L'\n');
        if (nl) { *nl = 0; p = nl + 1; } else { p += lstrlenW(p); }
        if (lstrlenW(line) < 2) continue;
        op = line[0];
        letter = ch_upper(line[1]);
        /* Reject control characters in arguments outright. */
        {
            const wchar_t *c;
            r = 0;
            for (c = line + 2; *c; c++) if (*c < 32) r = ERROR_INVALID_PARAMETER;
        }
        if (!r) r = refuse ? ERROR_ACCESS_DISABLED_BY_POLICY : exec_op(op, letter, line + 2);
        if (res) res->err[count] = r;
        if (r) failed++;
        count++;
    }
    CoUninitialize();
    if (lock) {
        if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) ReleaseMutex(lock);
        CloseHandle(lock);
    }

    if (res) {
        res->count = count;
        res->magic = RESULT_MAGIC;
        MemoryBarrier();
        res->done = 1;
        UnmapViewOfFile(res);
    }
    if (map) CloseHandle(map);
    object_name(name, ARRAYSIZE(name), L"Done", nonce);
    ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (ev) { SetEvent(ev); CloseHandle(ev); }
    HeapFree(GetProcessHeap(), 0, text);
    if (refuse) return 3;
    return failed ? 1 : 0;
}

/* ============================================================
 *  Task Scheduler
 * ============================================================ */

static HRESULT task_root(ITaskService **svc, ITaskFolder **root)
{
    VARIANT e;
    BSTR path;
    HRESULT hr;

    *svc = NULL;
    *root = NULL;
    hr = CoCreateInstance(&my_CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER,
                          &my_IID_ITaskService, (void **)svc);
    if (FAILED(hr)) return hr;
    VariantInit(&e);
    hr = ITaskService_Connect(*svc, e, e, e, e);
    if (SUCCEEDED(hr)) {
        path = SysAllocString(L"\\");
        hr = ITaskService_GetFolder(*svc, path, root);
        SysFreeString(path);
    }
    if (FAILED(hr)) {
        ITaskService_Release(*svc);
        *svc = NULL;
    }
    return hr;
}

static void xml_escape(const wchar_t *in, wchar_t *out, size_t cch)
{
    size_t w = 0;
    for (; *in && w + 7 < cch; in++) {
        const wchar_t *rep = NULL;
        switch (*in) {
        case L'&': rep = L"&amp;"; break;
        case L'<': rep = L"&lt;"; break;
        case L'>': rep = L"&gt;"; break;
        case L'"': rep = L"&quot;"; break;
        }
        if (rep) { while (*rep) out[w++] = *rep++; }
        else out[w++] = *in;
    }
    out[w] = 0;
}

static HRESULT get_task(ITaskService **svc, ITaskFolder **root, IRegisteredTask **task)
{
    HRESULT hr = task_root(svc, root);
    BSTR name;
    *task = NULL;
    if (FAILED(hr)) return hr;
    name = SysAllocString(APP_TASKNAME);
    hr = ITaskFolder_GetTask(*root, name, task);
    SysFreeString(name);
    return hr;
}

static void release_task(ITaskService *svc, ITaskFolder *root, IRegisteredTask *task)
{
    if (task) IRegisteredTask_Release(task);
    if (root) ITaskFolder_Release(root);
    if (svc) ITaskService_Release(svc);
}

BOOL Uac_TaskRegistered(void)
{
    /* A task for an exe in an unprotected place is never used. */
    return Uac_TaskPresent() && Uac_LocationSecure();
}

BOOL Uac_TaskPresent(void)
{
    ITaskService *svc;
    ITaskFolder *root;
    IRegisteredTask *task;
    BOOL ok = FALSE;

    if (SUCCEEDED(get_task(&svc, &root, &task)) && task) {
        BSTR xml = NULL;
        VARIANT_BOOL en = VARIANT_FALSE;
        IRegisteredTask_get_Enabled(task, &en);
        if (en && SUCCEEDED(IRegisteredTask_get_Xml(task, &xml)) && xml) {
            /* Only a task that starts exactly *this* exe counts. */
            wchar_t esc[MAX_PATH * 6], cmd[MAX_PATH * 6 + 32];
            xml_escape(g_exePath, esc, ARRAYSIZE(esc));
            StringCchPrintfW(cmd, ARRAYSIZE(cmd), L"<Command>%s</Command>", esc);
            ok = StrStrIW(xml, cmd) != NULL;
            SysFreeString(xml);
        }
    }
    release_task(svc, root, task);
    return ok;
}

static BOOL current_user_sid(wchar_t *out, size_t cch)
{
    HANDLE tok;
    BYTE buf[256];
    DWORD got = 0;
    wchar_t *s = NULL;
    BOOL ok = FALSE;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return FALSE;
    if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &got) &&
        ConvertSidToStringSidW(((TOKEN_USER *)buf)->User.Sid, &s)) {
        ok = SUCCEEDED(StringCchCopyW(out, cch, s));
        LocalFree(s);
    }
    CloseHandle(tok);
    return ok;
}

HRESULT Uac_RegisterTask(void)
{
    ITaskService *svc;
    ITaskFolder *root;
    IRegisteredTask *reg = NULL;
    wchar_t sid[192], exe[MAX_PATH * 6], desc[512];
    wchar_t *xml;
    BSTR bxml, bname;
    VARIANT e;
    HRESULT hr;
    const size_t cchXml = 8192;

    /* A task that runs a user-replaceable exe without a prompt would be a
       UAC bypass for every program of this user. */
    if (!Uac_LocationSecure()) return HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY);
    if (!current_user_sid(sid, ARRAYSIZE(sid))) return HRESULT_FROM_WIN32(GetLastError());
    xml_escape(g_exePath, exe, ARRAYSIZE(exe));
    xml_escape(T(S_TASK_DESC), desc, ARRAYSIZE(desc));

    xml = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, cchXml * sizeof(wchar_t));
    if (!xml) return E_OUTOFMEMORY;
    StringCchPrintfW(xml, cchXml,
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <RegistrationInfo>\r\n"
        L"    <Author>OpenSubst</Author>\r\n"
        L"    <Description>%s</Description>\r\n"
        L"  </RegistrationInfo>\r\n"
        L"  <Principals>\r\n"
        L"    <Principal id=\"Author\">\r\n"
        L"      <UserId>%s</UserId>\r\n"
        L"      <LogonType>InteractiveToken</LogonType>\r\n"
        L"      <RunLevel>HighestAvailable</RunLevel>\r\n"
        L"    </Principal>\r\n"
        L"  </Principals>\r\n"
        L"  <Settings>\r\n"
        L"    <MultipleInstancesPolicy>Parallel</MultipleInstancesPolicy>\r\n"
        L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
        L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
        L"    <AllowHardTerminate>true</AllowHardTerminate>\r\n"
        L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
        L"    <Enabled>true</Enabled>\r\n"
        L"    <ExecutionTimeLimit>PT5M</ExecutionTimeLimit>\r\n"
        L"    <Priority>5</Priority>\r\n"
        L"  </Settings>\r\n"
        L"  <Triggers />\r\n"
        L"  <Actions Context=\"Author\">\r\n"
        L"    <Exec>\r\n"
        L"      <Command>%s</Command>\r\n"
        L"      <Arguments>/UACTASK $(Arg0)</Arguments>\r\n"
        L"    </Exec>\r\n"
        L"  </Actions>\r\n"
        L"</Task>\r\n", desc, sid, exe);

    hr = task_root(&svc, &root);
    if (SUCCEEDED(hr)) {
        VariantInit(&e);
        bxml = SysAllocString(xml);
        bname = SysAllocString(APP_TASKNAME);
        hr = ITaskFolder_RegisterTask(root, bname, bxml, TASK_CREATE_OR_UPDATE, e, e,
                                      TASK_LOGON_INTERACTIVE_TOKEN, e, &reg);
        SysFreeString(bxml);
        SysFreeString(bname);
        if (reg) IRegisteredTask_Release(reg);
        ITaskFolder_Release(root);
        ITaskService_Release(svc);
    }
    HeapFree(GetProcessHeap(), 0, xml);
    return hr;
}

HRESULT Uac_UnregisterTask(void)
{
    ITaskService *svc;
    ITaskFolder *root;
    HRESULT hr = task_root(&svc, &root);
    if (SUCCEEDED(hr)) {
        BSTR name = SysAllocString(APP_TASKNAME);
        hr = ITaskFolder_DeleteTask(root, name, 0);
        SysFreeString(name);
        if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) hr = S_OK;
        ITaskFolder_Release(root);
        ITaskService_Release(svc);
    }
    return hr;
}

static HRESULT run_task(const wchar_t *param)
{
    ITaskService *svc;
    ITaskFolder *root;
    IRegisteredTask *task;
    IRunningTask *running = NULL;
    HRESULT hr = get_task(&svc, &root, &task);
    if (SUCCEEDED(hr) && task) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_BSTR;
        v.bstrVal = SysAllocString(param);
        hr = IRegisteredTask_Run(task, v, &running);
        VariantClear(&v);
        if (running) IRunningTask_Release(running);
    }
    release_task(svc, root, task);
    return hr;
}

BOOL Uac_RunElevatedSelf(HWND owner, const wchar_t *args, DWORD *exitCode)
{
    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = g_exePath;
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;
    if (exitCode) *exitCode = (DWORD)-1;
    if (!ShellExecuteExW(&sei)) return FALSE;
    if (sei.hProcess) {
        if (wait_pumping(sei.hProcess, 120000, owner) == WAIT_OBJECT_0) {
            if (exitCode && !GetExitCodeProcess(sei.hProcess, exitCode)) *exitCode = (DWORD)-1;
        } else if (exitCode) {
            *exitCode = WAIT_TIMEOUT;
        }
        CloseHandle(sei.hProcess);
    }
    return TRUE;
}

/* ============================================================
 *  Caller side
 * ============================================================ */

static void make_nonce(wchar_t out[17])
{
    static LONG counter;
    ULONGLONG v = GetTickCount64() ^ ((ULONGLONG)GetCurrentProcessId() << 32) ^
                  ((ULONGLONG)InterlockedIncrement(&counter) << 20);
    LARGE_INTEGER pc;
    QueryPerformanceCounter(&pc);
    v ^= (ULONGLONG)pc.QuadPart * 0x9E3779B97F4A7C15ull;
    StringCchPrintfW(out, 17, L"%016llX", v);
}

/* Run ops[first..first+n) on the elevated side. Writes the per-op results. */
static DWORD run_batch(HWND owner, const ElevOp *ops, int n, BOOL viaTask, DWORD *errs)
{
    wchar_t nonce[17], name[96];
    wchar_t *text, *hex = NULL;
    size_t cchText = 32 + (size_t)n * (PATHBUF + 4);
    HANDLE map, ev;
    ElevResult *res;
    DWORD r = 0;
    int i;

    for (i = 0; i < n; i++) errs[i] = ERROR_NOT_READY;
    make_nonce(nonce);

    text = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, cchText * sizeof(wchar_t));
    if (!text) return ERROR_OUTOFMEMORY;
    StringCchPrintfW(text, cchText, L"%s\n", nonce);
    for (i = 0; i < n; i++) {
        wchar_t line[PATHBUF + 8];
        StringCchPrintfW(line, ARRAYSIZE(line), L"%c%c%s\n", ops[i].op, ops[i].letter, ops[i].arg);
        StringCchCatW(text, cchText, line);
    }
    hex = encode_payload(text);
    HeapFree(GetProcessHeap(), 0, text);
    if (!hex) return ERROR_OUTOFMEMORY;

    object_name(name, ARRAYSIZE(name), L"Result", nonce);
    map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                             sizeof(ElevResult), name);
    object_name(name, ARRAYSIZE(name), L"Done", nonce);
    ev = CreateEventW(NULL, TRUE, FALSE, name);
    res = map ? (ElevResult *)MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(ElevResult)) : NULL;

    if (!map || !ev || !res) {
        r = GetLastError();
        if (!r) r = ERROR_OUTOFMEMORY;
    } else if (viaTask) {
        HRESULT hr = run_task(hex);
        if (FAILED(hr)) r = (DWORD)hr;
        else if (wait_pumping(ev, 60000, owner) != WAIT_OBJECT_0) r = WAIT_TIMEOUT;
    } else {
        size_t cchArgs = (size_t)lstrlenW(hex) + 16;
        wchar_t *args = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, cchArgs * sizeof(wchar_t));
        DWORD code = 0;
        if (!args) r = ERROR_OUTOFMEMORY;
        else {
            StringCchPrintfW(args, cchArgs, L"/ELEVATED %s", hex);
            if (!Uac_RunElevatedSelf(owner, args, &code)) r = ERROR_CANCELLED;
            HeapFree(GetProcessHeap(), 0, args);
        }
    }

    if (res) {
        if (!r && res->done && res->magic == RESULT_MAGIC) {
            for (i = 0; i < n && i < (int)res->count; i++) {
                errs[i] = res->err[i];
                if (errs[i] && !r) r = errs[i];
            }
        } else if (!r) {
            r = ERROR_NOT_READY;
        }
        UnmapViewOfFile(res);
    }
    if (map) CloseHandle(map);
    if (ev) CloseHandle(ev);
    HeapFree(GetProcessHeap(), 0, hex);
    return r;
}

static DWORD elev_flush(HWND owner, BOOL allowPrompt);

DWORD Elev_Flush(HWND owner, BOOL allowPrompt)
{
    DWORD r;
    /* Waiting pumps messages; a timer or a network completion must not start
       a second flush meanwhile. The queued operations then simply wait for
       the next flush. */
    if (s_busy) return ERROR_BUSY;
    s_busy = TRUE;
    r = elev_flush(owner, allowPrompt);
    s_busy = FALSE;
    return r;
}

static DWORD elev_flush(HWND owner, BOOL allowPrompt)
{
    ElevOp *ops;
    DWORD errs[MAX_OPS];
    DWORD result = 0;
    int n = s_nops, i, start;
    BOOL viaTask;

    if (n == 0) return 0;
    ops = (ElevOp *)HeapAlloc(GetProcessHeap(), 0, (size_t)n * sizeof(ElevOp));
    if (!ops) { s_nops = 0; return ERROR_OUTOFMEMORY; }
    CopyMemory(ops, s_ops, (size_t)n * sizeof(ElevOp));
    s_nops = 0;

    if (!g_splitToken) {
        /* Either elevated already or no UAC split: there is only one session
           that matters, and the normal operations already covered it. Only
           the machine-wide Recycle Bin part is left to do. */
        for (i = 0; i < n; i++) {
            DWORD r;
            if (is_namespace_op(ops[i].op)) continue;
            r = g_isAdminNow ? exec_op(ops[i].op, ops[i].letter, ops[i].arg) : ERROR_ACCESS_DENIED;
            if (r && !result) result = r;
        }
        HeapFree(GetProcessHeap(), 0, ops);
        return result;
    }

    viaTask = Uac_TaskRegistered();
    if (!viaTask) {
        /* Without the task only the one-off Recycle Bin registration is worth
           a prompt; drop the namespace operations. */
        int w = 0;
        for (i = 0; i < n; i++) if (!is_namespace_op(ops[i].op)) ops[w++] = ops[i];
        if (w < n) result = ERROR_NOT_READY;
        n = w;
        if (n == 0 || !allowPrompt) {
            HeapFree(GetProcessHeap(), 0, ops);
            return n ? ERROR_NOT_READY : result;
        }
    }

    /* Split into command lines of bounded length. */
    start = 0;
    while (start < n) {
        int cnt = 0;
        size_t hexLen = 40;
        DWORD r;
        while (start + cnt < n) {
            size_t add = ((size_t)lstrlenW(ops[start + cnt].arg) + 4) * 6;
            if (cnt && hexLen + add > MAX_CHUNK_HEX) break;
            hexLen += add;
            cnt++;
        }
        r = run_batch(owner, ops + start, cnt, viaTask, errs);
        if (r && !result) result = r;
        if (r == ERROR_CANCELLED) break;
        start += cnt;
    }
    HeapFree(GetProcessHeap(), 0, ops);
    return result;
}

/*
 * engine.c - drive management logic.
 *
 * Copyright (c) 2026 Rekow IT. MIT License, see LICENSE.
 */
#include "engine.h"
#include "drives.h"
#include "recycle.h"
#include "uac.h"
#include "lang.h"

#define NET_RETRY_MS    30000

typedef struct NetJob {
    wchar_t letter;
    BOOL    interactive;
    HWND    owner;
    HWND    notify;
    DWORD   err;
    wchar_t unc[PATHBUF];
    wchar_t text[512];
} NetJob;

static HWND      s_notify;
static BOOL      s_pending[26];
static ULONGLONG s_lastTry[26];
/* The job each pending letter waits for. WM_APP_NETDONE carries a pointer;
   only a pointer found here is ever dereferenced or freed, so a message
   posted by another process cannot make us touch arbitrary memory. */
static NetJob   *s_jobs[26];

void Engine_SetNotifyWindow(HWND hwnd) { s_notify = hwnd; }

BOOL Engine_IsPending(wchar_t letter)
{
    return IS_DRIVE_LETTER(letter) && s_pending[LETTER_INDEX(letter)];
}

int Engine_PendingCount(void)
{
    int i, n = 0;
    for (i = 0; i < 26; i++) if (s_pending[i]) n++;
    return n;
}

/* ============================================================
 *  Messages
 * ============================================================ */

static void report(HWND owner, StrId fmt, wchar_t letter, const wchar_t *path,
                   DWORD err, const wchar_t *extra)
{
    wchar_t head[PATHBUF + 256], msg[PATHBUF + 1024], etext[512], line[640];
    StringCchPrintfW(head, ARRAYSIZE(head), T(fmt), letter, path ? path : L"");
    if (extra && extra[0]) {
        StringCchCopyW(etext, ARRAYSIZE(etext), extra);
    } else {
        Error_Text(err, etext, ARRAYSIZE(etext));
    }
    StringCchPrintfW(line, ARRAYSIZE(line), T(S_E_ERRCODE), err, etext);
    StringCchPrintfW(msg, ARRAYSIZE(msg), L"%s\r\n\r\n%s", head, line);
    App_Message(owner, msg, TRUE);
}

static void report_text(HWND owner, StrId fmt, wchar_t letter, const wchar_t *path,
                        StrId detail)
{
    wchar_t head[PATHBUF + 256], msg[PATHBUF + 1024];
    StringCchPrintfW(head, ARRAYSIZE(head), T(fmt), letter, path ? path : L"");
    StringCchPrintfW(msg, ARRAYSIZE(msg), L"%s\r\n\r\n%s", head, T(detail));
    App_Message(owner, msg, TRUE);
}

static void report_in_use(HWND owner, wchar_t letter)
{
    wchar_t msg[256];
    StringCchPrintfW(msg, ARRAYSIZE(msg), T(S_E_LETTERINUSE), letter);
    App_Message(owner, msg, TRUE);
}

/* ============================================================
 *  After a successful mount
 * ============================================================ */

static void recycle_on(wchar_t letter)
{
    Recycle_RegisterUser(letter, TRUE);
    if (!Recycle_IsMachineRegistered(letter)) Elev_Add(EOP_RECYCLE, letter, NULL);
}

static void recycle_off(wchar_t letter)
{
    Recycle_RegisterUser(letter, FALSE);
    if (Recycle_IsMachineRegistered(letter)) Elev_Add(EOP_UNRECYCLE, letter, NULL);
}

void Engine_SetRecycle(wchar_t letter, BOOL on)
{
    if (on) recycle_on(letter); else recycle_off(letter);
    {
        wchar_t root[4] = { letter, L':', L'\\', 0 };
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, root, NULL);
    }
}

static void post_mount(const DriveConfig *c)
{
    BOOL net = (c->flags & DF_NETWORK) != 0;
    Drive_SetShellInfo(c->letter, c->label, c->icon, net ? c->path : NULL);
    Drive_NotifyShell(c->letter, TRUE);
    if (c->label[0] || c->icon[0]) Drive_NotifyShellInfo(c->letter, c->icon[0] != 0);
    if (c->flags & DF_UAC) Elev_Add(net ? EOP_NET : EOP_SUBST, c->letter, c->path);
    if (c->flags & DF_RECYCLE) recycle_on(c->letter);
}

void Engine_RefreshShellInfo(wchar_t letter, const wchar_t *oldLabel, const wchar_t *oldIcon)
{
    const DriveConfig *c = &g_cfg[LETTER_INDEX(letter)];
    BOOL net = (c->flags & DF_NETWORK) != 0;
    if (oldLabel && net && oldLabel[0] && lstrcmpW(oldLabel, c->label) != 0)
        Drive_ClearShellInfo(letter, c->path, oldLabel);
    Drive_SetShellInfo(letter, c->label, c->icon, net ? c->path : NULL);
    Drive_NotifyShellInfo(letter, oldIcon && lstrcmpiW(oldIcon, c->icon) != 0);
}

/* ============================================================
 *  Network connections in the background
 * ============================================================ */

static DWORD WINAPI net_thread(LPVOID param)
{
    NetJob *job = (NetJob *)param;
    job->err = Drive_MountNet(job->letter, job->unc, job->interactive ? job->owner : NULL,
                              job->interactive, job->text, ARRAYSIZE(job->text));
    /* The window thread owns the job from here (see s_jobs). If the post
       fails (queue full), retry; if the window is gone the process is
       ending and the job is simply left to the process teardown - freeing
       it here could race with the window thread. */
    {
        int tries;
        for (tries = 0; tries < 100; tries++) {
            if (PostMessageW(job->notify, WM_APP_NETDONE, 0, (LPARAM)job)) break;
            if (!IsWindow(job->notify)) break;
            Sleep(100);
        }
    }
    return 0;
}

static DWORD start_net(const DriveConfig *c, HWND owner, BOOL interactive)
{
    int i = LETTER_INDEX(c->letter);
    NetJob *job;
    HANDLE th;

    if (s_pending[i]) return ERROR_IO_PENDING;
    s_lastTry[i] = GetTickCount64();

    if (!s_notify) {
        /* No window to report back to: connect synchronously. */
        wchar_t text[512];
        DWORD r = Drive_MountNet(c->letter, c->path, owner, interactive, text, ARRAYSIZE(text));
        if (r == 0) post_mount(c);
        else if (interactive) report(owner, S_E_NET, c->letter, c->path, r, text);
        return r;
    }

    job = (NetJob *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(NetJob));
    if (!job) return ERROR_OUTOFMEMORY;
    job->letter = c->letter;
    job->interactive = interactive;
    job->owner = owner;
    job->notify = s_notify;
    StringCchCopyW(job->unc, ARRAYSIZE(job->unc), c->path);
    /* Registered before the thread starts: it may finish immediately. */
    s_jobs[i] = job;
    s_pending[i] = TRUE;
    th = CreateThread(NULL, 64 * 1024, net_thread, job, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!th) {
        DWORD e = GetLastError();
        s_jobs[i] = NULL;
        s_pending[i] = FALSE;
        HeapFree(GetProcessHeap(), 0, job);
        return e ? e : ERROR_NOT_ENOUGH_MEMORY;
    }
    CloseHandle(th);
    return ERROR_IO_PENDING;
}

BOOL Engine_IsJob(LPARAM lp)
{
    int i;
    if (!lp) return FALSE;
    for (i = 0; i < 26; i++) if ((LPARAM)s_jobs[i] == lp) return TRUE;
    return FALSE;
}

wchar_t Engine_OnNetDone(LPARAM lp, HWND owner)
{
    NetJob *job = NULL;
    wchar_t letter;
    const DriveConfig *c;
    int i;

    for (i = 0; i < 26; i++) {
        if (s_jobs[i] && (LPARAM)s_jobs[i] == lp) { job = s_jobs[i]; break; }
    }
    if (!job) return 0;
    s_jobs[i] = NULL;
    letter = job->letter;
    s_pending[i] = FALSE;
    c = &g_cfg[i];

    if (job->err == 0) {
        if (c->used && (c->flags & DF_ENABLED) && (c->flags & DF_NETWORK) &&
            lstrcmpiW(c->path, job->unc) == 0) {
            post_mount(c);
            Engine_Commit(owner, job->interactive);
        } else {
            /* Removed, disabled or changed while connecting: do not leave an
               orphaned connection behind. */
            DriveState st;
            Drive_Query(letter, &st);
            if (st.kind == DK_NETWORK && lstrcmpiW(st.target, job->unc) == 0)
                Drive_UnmountNet(letter, FALSE);
        }
    } else if (job->interactive && job->err != ERROR_CANCELLED) {
        report(owner, S_E_NET, letter, job->unc, job->err, job->text);
    }
    HeapFree(GetProcessHeap(), 0, job);
    return letter;
}

/* ============================================================
 *  Mount / unmount
 * ============================================================ */

DWORD Engine_Mount(wchar_t letter, HWND owner, BOOL interactive)
{
    DriveConfig *c;
    DriveState st;
    DWORD r;

    if (!IS_DRIVE_LETTER(letter)) return ERROR_INVALID_DRIVE;
    c = &g_cfg[LETTER_INDEX(letter)];
    if (!c->used) return ERROR_INVALID_DRIVE;
    Drive_Query(letter, &st);

    if (c->flags & DF_NETWORK) {
        if (!(g_set.flags & AF_MANAGENET)) {
            if (interactive) report_text(owner, S_E_NET, letter, c->path, S_E_NETDISABLED);
            return ERROR_NOT_SUPPORTED;
        }
        if (st.kind == DK_NETWORK && lstrcmpiW(st.target, c->path) == 0) {
            post_mount(c);
            return 0;
        }
        /* A live connection to another share is somebody else's drive. A
           disconnected remembered one is replaced by Drive_MountNet. */
        if (st.kind != DK_NONE && st.kind != DK_NETUNAVAIL) {
            if (interactive) report_in_use(owner, letter);
            return ERROR_ALREADY_ASSIGNED;
        }
        return start_net(c, owner, interactive);
    }

    if (st.kind == DK_SUBST && lstrcmpiW(st.target, c->path) == 0) {
        post_mount(c);
        return 0;
    }
    if (st.kind != DK_NONE) {
        /* Includes a virtual drive to another folder: never replace a drive
           OpenSubst did not create for this entry. */
        if (interactive) report_in_use(owner, letter);
        return ERROR_ALREADY_ASSIGNED;
    }

    r = Drive_MountSubst(letter, c->path);
    if (r) {
        if (interactive) report(owner, S_E_SUBST, letter, c->path, r, NULL);
        return r;
    }
    post_mount(c);
    return 0;
}

static DWORD unmount_net(wchar_t letter, HWND owner, BOOL interactive)
{
    DWORD r = Drive_UnmountNet(letter, FALSE);
    if ((r == ERROR_OPEN_FILES || r == ERROR_DEVICE_IN_USE) && interactive && !g_quiet) {
        wchar_t q[256];
        StringCchPrintfW(q, ARRAYSIZE(q), T(S_Q_FORCENET), letter);
        if (MessageBoxW(owner, q, T(S_APP_TITLE), MB_YESNO | MB_ICONWARNING) == IDYES)
            r = Drive_UnmountNet(letter, TRUE);
        else
            return ERROR_CANCELLED;
    }
    if (r == ERROR_NOT_CONNECTED) r = 0;
    if (r && interactive) report(owner, S_E_UNNET, letter, NULL, r, NULL);
    return r;
}

DWORD Engine_Unmount(wchar_t letter, HWND owner, BOOL interactive)
{
    DriveConfig *c;
    DriveState st;
    DWORD r = 0;

    if (!IS_DRIVE_LETTER(letter)) return ERROR_INVALID_DRIVE;
    c = &g_cfg[LETTER_INDEX(letter)];
    if (!c->used) return Engine_UnmountForeign(letter, owner, interactive);
    Drive_Query(letter, &st);

    if (c->flags & DF_NETWORK) {
        if ((st.kind == DK_NETWORK || st.kind == DK_NETUNAVAIL) &&
            lstrcmpiW(st.target, c->path) == 0) {
            r = unmount_net(letter, owner, interactive);
            if (r) return r;
            Drive_NotifyShell(letter, FALSE);
        }
        Drive_ClearShellInfo(letter, c->path, c->label);
        if (c->flags & DF_UAC) Elev_Add(EOP_UNNET, letter, NULL);
        return 0;
    }

    if (st.kind == DK_SUBST && lstrcmpiW(st.target, c->path) == 0) {
        r = Drive_UnmountSubst(letter);
        if (r) {
            if (interactive) report(owner, S_E_UNSUBST, letter, NULL, r, NULL);
            return r;
        }
        Drive_NotifyShell(letter, FALSE);
    }
    /* Only clear the Explorer override if no other drive took the letter. */
    if (st.kind == DK_NONE || (st.kind == DK_SUBST && lstrcmpiW(st.target, c->path) == 0))
        Drive_ClearShellInfo(letter, NULL, NULL);
    if (c->flags & DF_UAC) Elev_Add(EOP_UNSUBST, letter, NULL);
    return 0;
}

DWORD Engine_UnmountForeign(wchar_t letter, HWND owner, BOOL interactive)
{
    DriveState st;
    DWORD r = 0;
    Drive_Query(letter, &st);
    if (st.kind == DK_SUBST) {
        r = Drive_UnmountSubst(letter);
        if (r && interactive) report(owner, S_E_UNSUBST, letter, NULL, r, NULL);
    } else if (st.kind == DK_NETWORK || st.kind == DK_NETUNAVAIL) {
        r = unmount_net(letter, owner, interactive);
    } else {
        return ERROR_INVALID_DRIVE;
    }
    if (!r) Drive_NotifyShell(letter, FALSE);
    return r;
}

void Engine_Remove(wchar_t letter, HWND owner, BOOL interactive)
{
    DriveConfig *c;
    if (!IS_DRIVE_LETTER(letter)) return;
    c = &g_cfg[LETTER_INDEX(letter)];
    if (!c->used) {
        Engine_UnmountForeign(letter, owner, interactive);
        return;
    }
    /* If the drive could not be taken down, keep the entry: forgetting it
       would leave a drive behind that OpenSubst no longer knows about. */
    if (Engine_Unmount(letter, owner, interactive) != 0) return;
    if (c->flags & DF_RECYCLE) recycle_off(letter);
    c->used = FALSE;
    c->flags = 0;
    c->path[0] = c->label[0] = c->icon[0] = 0;
}

void Engine_Commit(HWND owner, BOOL interactive)
{
    DWORD r;
    if (!Elev_Pending()) return;
    r = Elev_Flush(owner, interactive);
    if (r && interactive && r != ERROR_NOT_READY && r != ERROR_CANCELLED && r != ERROR_BUSY) {
        wchar_t msg[1024], etext[512];
        Error_Text(r, etext, ARRAYSIZE(etext));
        StringCchPrintfW(msg, ARRAYSIZE(msg), L"%s\r\n\r\n", T(S_E_ELEVATED));
        {
            wchar_t line[640];
            StringCchPrintfW(line, ARRAYSIZE(line), T(S_E_ERRCODE), r, etext);
            StringCchCatW(msg, ARRAYSIZE(msg), line);
        }
        App_Message(owner, msg, TRUE);
    }
}

/* ============================================================
 *  Restore
 * ============================================================ */

void Engine_RestoreAll(HWND owner, BOOL interactive)
{
    int i;
    for (i = 0; i < 26; i++) {
        DriveConfig *c = &g_cfg[i];
        if (c->used && (c->flags & DF_ENABLED)) Engine_Mount(c->letter, owner, interactive);
    }
    Engine_Commit(owner, interactive);
}

BOOL Engine_AutoRestoreTick(HWND owner)
{
    ULONGLONG now = GetTickCount64();
    BOOL started = FALSE;
    int i;

    if (Elev_IsBusy()) return FALSE;    /* called from a message pumped during a wait */
    for (i = 0; i < 26; i++) {
        DriveConfig *c = &g_cfg[i];
        DriveState st;
        if (!c->used || (c->flags & (DF_ENABLED | DF_AUTORESTORE)) != (DF_ENABLED | DF_AUTORESTORE))
            continue;
        if (s_pending[i]) continue;
        Drive_Query(c->letter, &st);

        if (c->flags & DF_NETWORK) {
            if (!(g_set.flags & AF_MANAGENET)) continue;
            if (st.kind != DK_NONE && st.kind != DK_NETUNAVAIL) continue;
            if (s_lastTry[i] && now - s_lastTry[i] < NET_RETRY_MS) continue;
            start_net(c, owner, FALSE);
            started = TRUE;
        } else {
            if (st.kind != DK_NONE) continue;
            if (!Path_IsDirectoryTimeout(c->path, DIR_TIMEOUT_UI)) continue;
            if (Drive_MountSubst(c->letter, c->path) == 0) {
                post_mount(c);
                started = TRUE;
            }
        }
    }
    if (Elev_Pending()) Engine_Commit(owner, FALSE);
    return started;
}

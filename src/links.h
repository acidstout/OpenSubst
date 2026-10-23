/*
 * links.h - folder links: a short path (e.g. C:\p) that leads to a target
 * folder. Unlike a drive letter, a link lives in the file system, so every
 * user, elevated program and service sees it at once; each keeps their own
 * access rights to the target.
 *
 *   local target   -> junction (needs no administrator rights)
 *   network target -> directory symbolic link (needs Developer Mode or the
 *                     "Create symbolic links" right)
 *
 * Links are created with the rights of the running user only - never through
 * the UAC task or any other elevation.
 *
 * Copyright (c) 2026 Rekow IT. MIT License, see LICENSE.
 */
#ifndef OPENSUBST_LINKS_H
#define OPENSUBST_LINKS_H

#include "common.h"

#define MAX_LINKS 32

typedef struct LinkConfig {
    BOOL    used;
    DWORD   flags;                  /* DF_ENABLED */
    int     kind;                   /* LK_JUNCTION or LK_SYMLINK          */
    wchar_t link[PATHBUF];          /* where the link lives, e.g. C:\p   */
    wchar_t target[PATHBUF];        /* resolved folder or UNC path       */
} LinkConfig;

extern LinkConfig g_links[MAX_LINKS];

enum {
    LK_NONE = 0,        /* nothing at the link path          */
    LK_JUNCTION,        /* a junction (target in 'target')   */
    LK_SYMLINK,         /* a symbolic link                   */
    LK_PLAIN,           /* a real folder or file - never ours */
    LK_UNKNOWN          /* cannot be read (access denied, ...) */
};

typedef struct LinkState {
    int     kind;
    wchar_t target[PATHBUF];
} LinkState;

void  Link_Query(const wchar_t *link, LinkState *st);

/* Canonical target: resolves virtual drive letters to their folder and
   network drive letters to UNC, so the link works for every user (drive
   letters are per session). Returns FALSE for unusable input. */
BOOL  Link_ResolveTarget(const wchar_t *in, wchar_t *out, size_t cch);
/* Canonical link location: must end up on a local volume, not a root. */
BOOL  Link_ResolveLocation(const wchar_t *in, wchar_t *out, size_t cch);

/* FALSE if the volume holding the existing folder 'path' cannot store
   junctions or symbolic links (FAT32, exFAT). 'desc' gets e.g. "F:\ (exFAT)".
   Unknown counts as supported: creating the link then decides. */
BOOL  Link_VolumeSupported(const wchar_t *path, wchar_t *desc, size_t cch);

/* TRUE if a and b are equal or one lies inside the other. */
BOOL  Link_PathsOverlap(const wchar_t *a, const wchar_t *b);

/* Create a link at a free path. Win32 error code (0 = success). */
/* kind: LK_JUNCTION (local targets only) or LK_SYMLINK. */
DWORD Link_Create(const wchar_t *link, const wchar_t *target, int kind);
/* Remove the link object itself (never the target's contents). Refuses
   anything that is not a junction or symbolic link. */
DWORD Link_Remove(const wchar_t *link);

/* Stored links: HKCU\Software\OpenSubst\Links\<n>. */
void  Links_LoadAll(void);
void  Links_Save(int i);
void  Links_Delete(int i);
int   Links_Find(const wchar_t *link);      /* index or -1 */
int   Links_FreeSlot(void);                 /* index or -1 */

/* Higher level, used by window and command line. 'interactive' reports
   errors with App_Message. */
DWORD Links_Apply(int i, HWND owner, BOOL interactive);     /* create if missing */
DWORD Links_Unapply(int i, HWND owner, BOOL interactive);   /* remove if ours    */
void  Links_RestoreAll(void);

/* Store an existing junction or symbolic link at 'path' as it is (nothing is
   created or changed). Returns its slot, or -1 (not a link, relative target,
   already stored, no free slot). */
int   Links_Adopt(const wchar_t *path);
/* Adopt every link directly inside 'folder'. Returns the number added. */
int   Links_ScanFolder(const wchar_t *folder);

/* Check typed input for slot 'self' (-1 = a new link). Fills 'out' with the
   canonical paths and reports the first problem with App_Message. Returns 0,
   or the field at fault: 1 = link, 2 = target. 'kind' is LK_JUNCTION,
   LK_SYMLINK or 0 (automatic: junction for local, symlink for network). */
int   Links_CheckInput(int self, const wchar_t *link, const wchar_t *target, int kind,
                       LinkConfig *out, HWND owner);
/* Put the checked link 'nc' (enabled) into slot i - a free slot or the one
   being edited - and, with 'create', create it (an edited link that moved is
   removed first). On failure slot i is left as it was. Does not save. */
DWORD Links_Set(int i, const LinkConfig *nc, HWND owner, BOOL interactive, BOOL create);

#endif

/*
 * recycle.h - Recycle Bin for virtual and network drives.
 *
 * Explorer only gives a drive its own Recycle Bin if the drive root is a
 * known folder listed under BitBucket\KnownFolder. OpenSubst registers one
 * known folder per drive letter:
 *
 *   machine part (administrator, both 64- and 32-bit registry views, so the
 *   bin also works in 32-bit applications' file dialogs):
 *     HKLM\...\Explorer\FolderDescriptions\{guid}   Category=4, Name, RelativePath
 *   user part (no administrator rights needed):
 *     HKCU\...\Explorer\User Shell Folders          {guid} = X:\
 *     HKCU\...\Explorer\BitBucket\KnownFolder\{guid} MaxCapacity, NukeOnDelete
 *
 * Copyright (c) 2026 Rekow IT. MIT License, see LICENSE.
 */
#ifndef OPENSUBST_RECYCLE_H
#define OPENSUBST_RECYCLE_H

#include "common.h"

void  Recycle_Guid(wchar_t letter, wchar_t *out, size_t cch);   /* "{...}" */
BOOL  Recycle_IsMachineRegistered(wchar_t letter);
DWORD Recycle_RegisterMachine(wchar_t letter, BOOL on);          /* needs admin */
DWORD Recycle_RegisterUser(wchar_t letter, BOOL on);

#endif

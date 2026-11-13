#pragma once
#include <windows.h>

HANDLE CqOpenInterface();
BOOL CqInstallDevice(const char* path);
BOOL CqChangeDevice(bool remove);
BOOL CqAcquireSession();
void CqReleaseSession();

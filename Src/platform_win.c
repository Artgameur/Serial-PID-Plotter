// ------------------------------------------------------------------------------------------------
/*!@file
   @brief platform.h for Windows. UTF-8 paths are converted to UTF-16 for the system (any folder
          or file name, accents included).

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------
#include "platform.h"

#include <io.h>
#include <process.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <shlobj.h>

// ================================================================================================
// ================================================================================================
//            PRIVATE DEFINE / STRUCTURE / VARIABLE DECLARATION
// ================================================================================================
// ================================================================================================

#define WIDE_PATH_LEN         (MAX_PATH * 2)
#define OPEN_PATHS_LEN        32768       // file names of a multiple selection (UTF-16)
#define FILTER_LEN            256

struct PlatformMutex
{
  CRITICAL_SECTION section;
};

typedef struct
{
  PlatformThreadFn_t fn;
  void *             arg;
} ThreadStart_t;

static HWND dialogOwner;

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTIONS
// ================================================================================================
// ================================================================================================

static unsigned __stdcall threadMain(void * arg)
{
  ThreadStart_t start = *(ThreadStart_t *) arg;

  free(arg);
  start.fn(start.arg);
  return 0;
}

static bool wide(const char * text, wchar_t * out, size_t size)
{
  return MultiByteToWideChar(CP_UTF8, 0, text, -1, out, (int) size) != 0;
}

static bool narrow(const wchar_t * text, char * out, size_t size)
{
  return WideCharToMultiByte(CP_UTF8, 0, text, -1, out, (int) size, NULL, NULL) != 0;
}

// An environment variable in UTF-8 (getenv() gives the ANSI code page: accents in a user name
// would be lost); false: not set
static bool environment(const wchar_t * name, char * value, size_t size)
{
  const wchar_t * text = _wgetenv(name);

  return (text != NULL) && (text[0] != L'\0') && narrow(text, value, size);
}

// "CSV file (*.csv)\0*.csv\0All files (*.*)\0*.*\0"
static void filterBuild(const PlatformFilter_t * filter, wchar_t * out, size_t size)
{
  char    text[FILTER_LEN];
  int     used = snprintf(text, sizeof(text), "%s (*.%s)|*.%s|%s (*.*)|*.*||", filter->name, filter->extension,
                          filter->extension, filter->allName);

  wide(text, out, size);

  for (int i = 0; (i < used) && (out[i] != L'\0'); ++i)
  {
    out[i] = (out[i] == L'|') ? L'\0' : out[i];
  }
}

// The Explorer style dialog (COM). path: default name in, chosen path(s) out.
static bool fileDialog(bool save, bool multiple, const char * title, const PlatformFilter_t * filter, wchar_t * path, size_t size)
{
  HRESULT       com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  OPENFILENAMEW dialog = {0};
  wchar_t       wideTitle[64];
  wchar_t       wideFilter[FILTER_LEN];
  wchar_t       extension[16];

  wide(title, wideTitle, sizeof(wideTitle) / sizeof(wideTitle[0]));
  filterBuild(filter, wideFilter, sizeof(wideFilter) / sizeof(wideFilter[0]));
  wide(filter->extension, extension, sizeof(extension) / sizeof(extension[0]));

  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner   = dialogOwner;
  dialog.lpstrFilter = wideFilter;
  dialog.lpstrFile   = path;
  dialog.nMaxFile    = (DWORD) size;
  dialog.lpstrDefExt = save ? extension : NULL;
  dialog.lpstrTitle  = wideTitle;
  dialog.Flags       = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER |
                       (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST) | (multiple ? OFN_ALLOWMULTISELECT : 0);

  bool chosen = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);

  if (SUCCEEDED(com))
  {
    CoUninitialize();
  }

  return chosen;
}

// ================================================================================================
// ================================================================================================
//            PUBLIC FUNCTIONS
// ================================================================================================
// ================================================================================================

bool platformThreadStart(PlatformThreadFn_t fn, void * arg)
{
  ThreadStart_t * start = malloc(sizeof(ThreadStart_t));

  if (start == NULL)
  {
    return false;
  }

  *start = (ThreadStart_t) {fn, arg};

  uintptr_t thread = _beginthreadex(NULL, 0, threadMain, start, 0, NULL);

  if (thread == 0)
  {
    free(start);
    return false;
  }

  CloseHandle((HANDLE) thread);
  return true;
}

PlatformMutex_t * platformMutexCreate(void)
{
  PlatformMutex_t * mutex = malloc(sizeof(PlatformMutex_t));

  if (mutex != NULL)
  {
    InitializeCriticalSection(&mutex->section);
  }

  return mutex;
}

void platformMutexDestroy(PlatformMutex_t * mutex)
{
  if (mutex != NULL)
  {
    DeleteCriticalSection(&mutex->section);
    free(mutex);
  }
}

void platformMutexLock(PlatformMutex_t * mutex)
{
  EnterCriticalSection(&mutex->section);
}

void platformMutexUnlock(PlatformMutex_t * mutex)
{
  LeaveCriticalSection(&mutex->section);
}

// FILETIME: 100 ns units since 1601-01-01
int64_t platformUnixMs(void)
{
  FILETIME now;

  GetSystemTimeAsFileTime(&now);
  return (int64_t) (((((uint64_t) now.dwHighDateTime << 32) | now.dwLowDateTime) - 116444736000000000ULL) / 10000ULL);
}

void platformLocalTime(int64_t unixMs, PlatformTime_t * time)
{
  ULARGE_INTEGER value = {.QuadPart = ((uint64_t) unixMs * 10000ULL) + 116444736000000000ULL};
  FILETIME       utc   = {value.LowPart, value.HighPart};
  SYSTEMTIME     utcTime;
  SYSTEMTIME     local;

  FileTimeToSystemTime(&utc, &utcTime);

  if (!SystemTimeToTzSpecificLocalTime(NULL, &utcTime, &local))
  {
    local = utcTime;
  }

  *time = (PlatformTime_t) {local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond,
                            local.wMilliseconds};
}

// Sleep() wakes on the system timer step (~15.6 ms by default): a high resolution waitable timer
// wakes when asked (Windows 10 1803+; otherwise Sleep())
void platformSleepMs(uint32_t ms)
{
  if (ms == 0)
  {
    return;
  }

  HANDLE        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  LARGE_INTEGER due   = {.QuadPart = -(LONGLONG) ms * 10000};   // 100 ns units, relative

  if ((timer != NULL) && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
  {
    WaitForSingleObject(timer, INFINITE);
  }
  else
  {
    Sleep(ms);
  }

  if (timer != NULL)
  {
    CloseHandle(timer);
  }
}

FILE * platformFopen(const char * path, const char * mode)
{
  wchar_t widePath[WIDE_PATH_LEN];
  wchar_t wideMode[8];

  return (wide(path, widePath, WIDE_PATH_LEN) && wide(mode, wideMode, 8)) ? _wfopen(widePath, wideMode) : NULL;
}

bool platformFileExists(const char * path)
{
  wchar_t widePath[WIDE_PATH_LEN];

  return wide(path, widePath, WIDE_PATH_LEN) && (GetFileAttributesW(widePath) != INVALID_FILE_ATTRIBUTES);
}

bool platformRemove(const char * path)
{
  wchar_t widePath[WIDE_PATH_LEN];

  return wide(path, widePath, WIDE_PATH_LEN) && DeleteFileW(widePath);
}

bool platformMakeDirs(const char * path)
{
  wchar_t widePath[WIDE_PATH_LEN];

  if (!wide(path, widePath, WIDE_PATH_LEN))
  {
    return false;
  }

  int result = SHCreateDirectoryExW(NULL, widePath, NULL);

  return (result == ERROR_SUCCESS) || (result == ERROR_ALREADY_EXISTS) || (result == ERROR_FILE_EXISTS);
}

bool platformFileSync(FILE * file)
{
  return (fflush(file) == 0) && (_commit(_fileno(file)) == 0);
}

bool platformReplaceFile(const char * from, const char * to)
{
  wchar_t wideFrom[WIDE_PATH_LEN];
  wchar_t wideTo[WIDE_PATH_LEN];

  return wide(from, wideFrom, WIDE_PATH_LEN) && wide(to, wideTo, WIDE_PATH_LEN) &&
         MoveFileExW(wideFrom, wideTo, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void platformListFiles(const char * folder, const char * extension, PlatformPathCb_t cb, void * context)
{
  char             pattern[PLATFORM_PATH_LEN];
  wchar_t          widePattern[WIDE_PATH_LEN];
  WIN32_FIND_DATAW found;

  snprintf(pattern, sizeof(pattern), "%s\\*.%s", folder, extension);

  if (!wide(pattern, widePattern, WIDE_PATH_LEN))
  {
    return;
  }

  HANDLE find = FindFirstFileW(widePattern, &found);

  if (find == INVALID_HANDLE_VALUE)
  {
    return;
  }

  do
  {
    char   name[PLATFORM_PATH_LEN];
    size_t extensionLen = strlen(extension) + 1;

    if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && narrow(found.cFileName, name, sizeof(name)) &&
        (strlen(name) > extensionLen))
    {
      name[strlen(name) - extensionLen] = '\0';
      cb(name, context);
    }
  }
  while (FindNextFileW(find, &found));

  FindClose(find);
}

bool platformConfigDir(char * path, size_t size)
{
  return environment(L"APPDATA", path, size);
}

bool platformDocumentsDir(char * path, size_t size)
{
  char home[PLATFORM_PATH_LEN];

  snprintf(path, size, "%s\\Documents", environment(L"USERPROFILE", home, sizeof(home)) ? home : "C:");
  return true;
}

int platformStrcasecmp(const char * a, const char * b)
{
  return _stricmp(a, b);
}

int platformStrncasecmp(const char * a, const char * b, size_t length)
{
  return _strnicmp(a, b, length);
}

void platformDialogOwnerSet(void * window)
{
  dialogOwner = (HWND) window;
}

bool platformSaveDialog(const char * title, const PlatformFilter_t * filter, char * path, size_t size)
{
  wchar_t widePath[WIDE_PATH_LEN];

  wide(path, widePath, WIDE_PATH_LEN);
  return fileDialog(true, false, title, filter, widePath, WIDE_PATH_LEN) && narrow(widePath, path, size);
}

// Multiple selection: the folder, then the file names, each ending with a 0 (one file: its path)
bool platformOpenDialog(const char * title, const PlatformFilter_t * filter, bool multiple, PlatformPathCb_t cb, void * context)
{
  // Its own buffer: two dialogs may be open at once (Compare's and the settings')
  wchar_t * paths = calloc(OPEN_PATHS_LEN, sizeof(wchar_t));
  char      path[PLATFORM_PATH_LEN];

  if ((paths == NULL) || !fileDialog(false, multiple, title, filter, paths, OPEN_PATHS_LEN))
  {
    free(paths);
    return false;
  }

  const wchar_t * folder = paths;
  const wchar_t * name   = folder + wcslen(folder) + 1;

  if (*name == L'\0')
  {
    if (narrow(folder, path, sizeof(path)))
    {
      cb(path, context);
    }
  }

  while (*name != L'\0')
  {
    wchar_t full[WIDE_PATH_LEN];
    size_t  folderLen = wcslen(folder);
    size_t  nameLen   = wcslen(name);

    if (((folderLen + nameLen + 2) <= WIDE_PATH_LEN))
    {
      wmemcpy(full, folder, folderLen);
      full[folderLen] = L'\\';
      wmemcpy(&full[folderLen + 1], name, nameLen + 1);

      if (narrow(full, path, sizeof(path)))
      {
        cb(path, context);
      }
    }

    name += nameLen + 1;
  }

  free(paths);
  return true;
}

bool platformFolderDialog(const char * title, char * path, size_t size)
{
  HRESULT     com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  wchar_t     wideTitle[64];
  wchar_t     chosen[MAX_PATH];
  BROWSEINFOW info = {0};

  wide(title, wideTitle, sizeof(wideTitle) / sizeof(wideTitle[0]));
  info.hwndOwner = dialogOwner;
  info.lpszTitle = wideTitle;
  info.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

  LPITEMIDLIST list = SHBrowseForFolderW(&info);
  bool         ok   = (list != NULL) && SHGetPathFromIDListW(list, chosen) && narrow(chosen, path, size);

  CoTaskMemFree(list);

  if (SUCCEEDED(com))
  {
    CoUninitialize();
  }

  return ok;
}

bool platformFallbackFontPath(char * path, size_t size)
{
  char windows[PLATFORM_PATH_LEN];

  snprintf(path, size, "%s\\Fonts\\segoeui.ttf", environment(L"SystemRoot", windows, sizeof(windows)) ? windows : "C:\\Windows");
  return true;
}

// ------------------------------------------------------------------------------------------------
/*!@file
   @brief What differs between Windows, macOS and Linux, for the app's modules: threads, locks,
          time, files (UTF-8 paths), the app's folders, file / folder dialogs, a fallback font.
          platform_win.c (Windows) and platform_posix.c (macOS, Linux) implement it.

   Paths are UTF-8 everywhere; the Windows version converts them for the system.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// ================================================================================================
// ================================================================================================
//            DEFINE DECLARATION
// ================================================================================================
// ================================================================================================

#define PLATFORM_PATH_LEN     1024        // UTF-8 bytes of a path, with its end

#ifdef _WIN32
#define PLATFORM_PATH_SEP     '\\'
#define PLATFORM_PATH_SEP_STR "\\"
#else
#define PLATFORM_PATH_SEP     '/'
#define PLATFORM_PATH_SEP_STR "/"
#endif

// ================================================================================================
// ================================================================================================
//            STRUCTURE DECLARATION
// ================================================================================================
// ================================================================================================

typedef struct PlatformMutex PlatformMutex_t;

// Local date and time
typedef struct
{
  int year;
  int month;                              // 1..12
  int day;                                // 1..31
  int hour;
  int minute;
  int second;
  int ms;
} PlatformTime_t;

// Files of a dialog: "CSV file" (*.csv), and "All files"
typedef struct
{
  const char * name;                      // "CSV file" (translated)
  const char * extension;                 // "csv", without the dot
  const char * allName;                   // "All files" (translated)
} PlatformFilter_t;

typedef void (*PlatformThreadFn_t)(void * arg);
typedef void (*PlatformPathCb_t)(const char * path, void * context);

// ================================================================================================
// ================================================================================================
//            EXTERNAL FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

// Thread that runs fn(arg) and ends (nobody waits for it); false: could not be started
bool platformThreadStart(PlatformThreadFn_t fn, void * arg);

PlatformMutex_t * platformMutexCreate(void);
void              platformMutexDestroy(PlatformMutex_t * mutex);
void              platformMutexLock(PlatformMutex_t * mutex);
void              platformMutexUnlock(PlatformMutex_t * mutex);

// Wall clock: milliseconds since 1970-01-01 UTC; and that time in the local time zone
int64_t platformUnixMs(void);
void    platformLocalTime(int64_t unixMs, PlatformTime_t * time);

// Waits ms milliseconds, to within about a millisecond
void platformSleepMs(uint32_t ms);

// Files, UTF-8 paths
FILE * platformFopen(const char * path, const char * mode);
bool   platformFileExists(const char * path);
bool   platformRemove(const char * path);
bool   platformMakeDirs(const char * path);                     // with its parents
bool   platformFileSync(FILE * file);                           // written to the disk
bool   platformReplaceFile(const char * from, const char * to); // to replaced by from, whole or not at all

// Each file of folder with that extension ("ini", no dot), by its name without the extension
void platformListFiles(const char * folder, const char * extension, PlatformPathCb_t cb, void * context);

// Folder of the app's settings (Windows: %APPDATA%, macOS: ~/Library/Application Support,
// Linux: $XDG_CONFIG_HOME or ~/.config), and the user's documents; false: unknown
bool platformConfigDir(char * path, size_t size);
bool platformDocumentsDir(char * path, size_t size);

// Case insensitive comparison (ASCII letters)
int platformStrcasecmp(const char * a, const char * b);
int platformStrncasecmp(const char * a, const char * b, size_t length);

// Dialogs: they block until closed, call them from a thread of their own (not the LVGL thread).
// The native window they are modal to (Windows: HWND) is given once by the window module.
void platformDialogOwnerSet(void * window);
// Save: path holds the default file name, gets the chosen path. false: canceled.
bool platformSaveDialog(const char * title, const PlatformFilter_t * filter, char * path, size_t size);
// Open: cb is called for each file chosen (several when multiple). false: canceled.
bool platformOpenDialog(const char * title, const PlatformFilter_t * filter, bool multiple, PlatformPathCb_t cb, void * context);
// Folder: path holds the starting folder (may be empty), gets the chosen one. false: canceled.
bool platformFolderDialog(const char * title, char * path, size_t size);

// A font with all the Latin letters (accents), for the text Montserrat lacks; false: none found
bool platformFallbackFontPath(char * path, size_t size);

#ifndef _WIN32
// System notification, shown without waiting (macOS: osascript, Linux: notify-send). Windows: the
// window module shows its own (notification area icon, app_window.c).
void platformNotify(const char * title, const char * text);
#endif

#endif //PLATFORM_H

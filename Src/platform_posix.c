// ------------------------------------------------------------------------------------------------
/*!@file
   @brief platform.h for macOS and Linux (POSIX). The dialogs are the system's own tools, run as
          commands: osascript on macOS, zenity (or kdialog) on Linux. Without them, a dialog
          returns "canceled".

   NOT YET BUILT OR TESTED: written on Windows, to be built and checked on macOS / Linux.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------
#ifndef _WIN32

#define _GNU_SOURCE
#include "platform.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

// ================================================================================================
// ================================================================================================
//            PRIVATE DEFINE / STRUCTURE DECLARATION
// ================================================================================================
// ================================================================================================

#define COMMAND_LEN           (PLATFORM_PATH_LEN * 3)

struct PlatformMutex
{
  pthread_mutex_t mutex;
};

typedef struct
{
  PlatformThreadFn_t fn;
  void *             arg;
} ThreadStart_t;

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTIONS
// ================================================================================================
// ================================================================================================

static void * threadMain(void * arg)
{
  ThreadStart_t start = *(ThreadStart_t *) arg;

  free(arg);
  start.fn(start.arg);
  return NULL;
}

// text quoted for the shell: '...' with each ' as '\''
static void shellQuote(const char * text, char * out, size_t size)
{
  size_t used = 0;

  if (size < 3)
  {
    return;
  }

  out[used++] = '\'';

  for (const char * c = text; (*c != '\0') && (used + 5 < size); ++c)
  {
    if (*c == '\'')
    {
      memcpy(&out[used], "'\\''", 4);
      used += 4;
    }
    else
    {
      out[used++] = *c;
    }
  }

  out[used++] = '\'';
  out[used]   = '\0';
}

#ifdef __APPLE__
// text for an AppleScript string: "..." with \ and " escaped
static void appleQuote(const char * text, char * out, size_t size)
{
  size_t used = 0;

  out[used++] = '"';

  for (const char * c = text; (*c != '\0') && (used + 3 < size); ++c)
  {
    if ((*c == '"') || (*c == '\\'))
    {
      out[used++] = '\\';
    }

    out[used++] = *c;
  }

  out[used++] = '"';
  out[used]   = '\0';
}
#endif

#ifndef __APPLE__
// Linux: a dialog tool installed (zenity, kdialog)
static bool commandExists(const char * name)
{
  char command[128];

  snprintf(command, sizeof(command), "command -v %s >/dev/null 2>&1", name);
  return system(command) == 0;
}
#endif

// Runs command and gives each line it prints to cb; false: nothing printed (canceled / failed)
static bool commandLines(const char * command, PlatformPathCb_t cb, void * context)
{
  FILE * pipe = popen(command, "r");
  char   line[PLATFORM_PATH_LEN];
  bool   any = false;

  if (pipe == NULL)
  {
    return false;
  }

  while (fgets(line, sizeof(line), pipe) != NULL)
  {
    line[strcspn(line, "\r\n")] = '\0';

    if (line[0] != '\0')
    {
      cb(line, context);
      any = true;
    }
  }

  pclose(pipe);
  return any;
}

static void firstLine(const char * line, void * context)
{
  char ** out = (char **) context;

  if (out[0][0] == '\0')
  {
    snprintf(out[0], (size_t) (uintptr_t) out[1], "%s", line);
  }
}

// The single path a command prints into path
static bool commandPath(const char * command, char * path, size_t size)
{
  char   result[PLATFORM_PATH_LEN] = "";
  char * out[2] = {result, (char *) (uintptr_t) sizeof(result)};

  if (!commandLines(command, firstLine, out) || (result[0] == '\0'))
  {
    return false;
  }

  snprintf(path, size, "%s", result);
  return true;
}

// ================================================================================================
// ================================================================================================
//            PUBLIC FUNCTIONS
// ================================================================================================
// ================================================================================================

bool platformThreadStart(PlatformThreadFn_t fn, void * arg)
{
  ThreadStart_t * start = malloc(sizeof(ThreadStart_t));
  pthread_t       thread;

  if (start == NULL)
  {
    return false;
  }

  *start = (ThreadStart_t) {fn, arg};

  if (pthread_create(&thread, NULL, threadMain, start) != 0)
  {
    free(start);
    return false;
  }

  pthread_detach(thread);
  return true;
}

PlatformMutex_t * platformMutexCreate(void)
{
  PlatformMutex_t * mutex = malloc(sizeof(PlatformMutex_t));

  if ((mutex != NULL) && (pthread_mutex_init(&mutex->mutex, NULL) != 0))
  {
    free(mutex);
    mutex = NULL;
  }

  return mutex;
}

void platformMutexDestroy(PlatformMutex_t * mutex)
{
  if (mutex != NULL)
  {
    pthread_mutex_destroy(&mutex->mutex);
    free(mutex);
  }
}

void platformMutexLock(PlatformMutex_t * mutex)
{
  pthread_mutex_lock(&mutex->mutex);
}

void platformMutexUnlock(PlatformMutex_t * mutex)
{
  pthread_mutex_unlock(&mutex->mutex);
}

int64_t platformUnixMs(void)
{
  struct timespec now;

  clock_gettime(CLOCK_REALTIME, &now);
  return ((int64_t) now.tv_sec * 1000) + (now.tv_nsec / 1000000);
}

void platformLocalTime(int64_t unixMs, PlatformTime_t * time)
{
  time_t    seconds = (time_t) (unixMs / 1000);
  struct tm local;

  localtime_r(&seconds, &local);
  *time = (PlatformTime_t) {local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min,
                            local.tm_sec, (int) (unixMs % 1000)};
}

void platformSleepMs(uint32_t ms)
{
  struct timespec wait = {(time_t) (ms / 1000), (long) (ms % 1000) * 1000000L};

  while ((nanosleep(&wait, &wait) != 0) && (errno == EINTR))
  {
  }
}

FILE * platformFopen(const char * path, const char * mode)
{
  return fopen(path, mode);
}

bool platformFileExists(const char * path)
{
  struct stat info;

  return stat(path, &info) == 0;
}

bool platformRemove(const char * path)
{
  return unlink(path) == 0;
}

bool platformMakeDirs(const char * path)
{
  char copy[PLATFORM_PATH_LEN];

  snprintf(copy, sizeof(copy), "%s", path);

  for (char * c = copy + 1; *c != '\0'; ++c)
  {
    if (*c == '/')
    {
      *c = '\0';
      mkdir(copy, 0755);
      *c = '/';
    }
  }

  return (mkdir(copy, 0755) == 0) || (errno == EEXIST);
}

bool platformFileSync(FILE * file)
{
  return (fflush(file) == 0) && (fsync(fileno(file)) == 0);
}

// rename() replaces the file in one step; the folder is synced so the new name is on the disk
bool platformReplaceFile(const char * from, const char * to)
{
  if (rename(from, to) != 0)
  {
    return false;
  }

  char folder[PLATFORM_PATH_LEN];

  snprintf(folder, sizeof(folder), "%s", to);

  char * slash = strrchr(folder, '/');

  if (slash != NULL)
  {
    *slash = '\0';

    FILE * dir = fopen(folder, "r");

    if (dir != NULL)
    {
      fsync(fileno(dir));
      fclose(dir);
    }
  }

  return true;
}

void platformListFiles(const char * folder, const char * extension, PlatformPathCb_t cb, void * context)
{
  DIR *           dir = opendir(folder);
  struct dirent * entry;
  size_t          extensionLen = strlen(extension) + 1;

  if (dir == NULL)
  {
    return;
  }

  while ((entry = readdir(dir)) != NULL)
  {
    char   name[PLATFORM_PATH_LEN];
    size_t len = strlen(entry->d_name);

    if ((len <= extensionLen) || (entry->d_name[0] == '.') || (entry->d_name[len - extensionLen] != '.') ||
        (strcmp(&entry->d_name[len - extensionLen + 1], extension) != 0))
    {
      continue;
    }

    snprintf(name, sizeof(name), "%.*s", (int) (len - extensionLen), entry->d_name);
    cb(name, context);
  }

  closedir(dir);
}

bool platformConfigDir(char * path, size_t size)
{
  const char * home = getenv("HOME");

#ifdef __APPLE__
  if ((home == NULL) || (home[0] == '\0'))
  {
    return false;
  }

  snprintf(path, size, "%s/Library/Application Support", home);
#else
  const char * config = getenv("XDG_CONFIG_HOME");

  if ((config != NULL) && (config[0] != '\0'))
  {
    snprintf(path, size, "%s", config);
  }
  else if ((home != NULL) && (home[0] != '\0'))
  {
    snprintf(path, size, "%s/.config", home);
  }
  else
  {
    return false;
  }
#endif

  platformMakeDirs(path);
  return true;
}

bool platformDocumentsDir(char * path, size_t size)
{
  const char * home = getenv("HOME");

  snprintf(path, size, "%s/Documents", ((home != NULL) && (home[0] != '\0')) ? home : ".");
  return true;
}

int platformStrcasecmp(const char * a, const char * b)
{
  return strcasecmp(a, b);
}

int platformStrncasecmp(const char * a, const char * b, size_t length)
{
  return strncasecmp(a, b, length);
}

void platformDialogOwnerSet(void * window)
{
  (void) window;   // the dialogs are separate programs
}

bool platformSaveDialog(const char * title, const PlatformFilter_t * filter, char * path, size_t size)
{
  char command[COMMAND_LEN];
  char name[PLATFORM_PATH_LEN];
  char text[PLATFORM_PATH_LEN];

#ifdef __APPLE__
  char script[COMMAND_LEN];

  appleQuote(title, text, sizeof(text));
  appleQuote(path, name, sizeof(name));
  snprintf(script, sizeof(script), "POSIX path of (choose file name with prompt %s default name %s)", text, name);
  shellQuote(script, text, sizeof(text));
  snprintf(command, sizeof(command), "osascript -e %s 2>/dev/null", text);
#else
  char filterRaw[200];
  char filterText[256];

  shellQuote(title, text, sizeof(text));
  shellQuote(path, name, sizeof(name));

  if (commandExists("zenity"))
  {
    snprintf(filterRaw, sizeof(filterRaw), "%s | *.%s", filter->name, filter->extension);
    shellQuote(filterRaw, filterText, sizeof(filterText));
    snprintf(command, sizeof(command), "zenity --file-selection --save --confirm-overwrite --title=%s --filename=%s --file-filter=%s 2>/dev/null",
             text, name, filterText);
  }
  else if (commandExists("kdialog"))
  {
    snprintf(filterRaw, sizeof(filterRaw), "*.%s", filter->extension);
    shellQuote(filterRaw, filterText, sizeof(filterText));
    snprintf(command, sizeof(command), "kdialog --title %s --getsavefilename %s %s 2>/dev/null", text, name, filterText);
  }
  else
  {
    return false;
  }
#endif

  if (!commandPath(command, path, size))
  {
    return false;
  }

  // The extension added to a name without one (as the Windows dialog does)
  const char * base = strrchr(path, '/');

  if ((strchr((base != NULL) ? base : path, '.') == NULL) && ((strlen(path) + strlen(filter->extension) + 2) < size))
  {
    strcat(path, ".");
    strcat(path, filter->extension);
  }

  return true;
}

bool platformOpenDialog(const char * title, const PlatformFilter_t * filter, bool multiple, PlatformPathCb_t cb, void * context)
{
  char command[COMMAND_LEN];
  char text[PLATFORM_PATH_LEN];

#ifdef __APPLE__
  char script[COMMAND_LEN];

  (void) filter;
  appleQuote(title, text, sizeof(text));

  // One POSIX path per line
  snprintf(script, sizeof(script),
           "set chosen to (choose file with prompt %s%s)\n"
           "set out to \"\"\n"
           "repeat with f in (chosen as list)\n"
           "set out to out & POSIX path of f & linefeed\n"
           "end repeat\n"
           "return out",
           text, multiple ? " with multiple selections allowed" : "");
  shellQuote(script, text, sizeof(text));
  snprintf(command, sizeof(command), "osascript -e %s 2>/dev/null", text);
#else
  char filterRaw[200];
  char filterText[256];

  shellQuote(title, text, sizeof(text));

  if (commandExists("zenity"))
  {
    snprintf(filterRaw, sizeof(filterRaw), "%s | *.%s", filter->name, filter->extension);
    shellQuote(filterRaw, filterText, sizeof(filterText));
    // One path per line
    snprintf(command, sizeof(command), "zenity --file-selection %s --separator='\n' --title=%s --file-filter=%s 2>/dev/null",
             multiple ? "--multiple" : "", text, filterText);
  }
  else if (commandExists("kdialog"))
  {
    snprintf(filterRaw, sizeof(filterRaw), "*.%s", filter->extension);
    shellQuote(filterRaw, filterText, sizeof(filterText));
    snprintf(command, sizeof(command), "kdialog --title %s --getopenfilename . %s %s --separate-output 2>/dev/null", text,
             filterText, multiple ? "--multiple" : "");
  }
  else
  {
    return false;
  }
#endif

  return commandLines(command, cb, context);
}

bool platformFolderDialog(const char * title, char * path, size_t size)
{
  char command[COMMAND_LEN];
  char text[PLATFORM_PATH_LEN];

#ifdef __APPLE__
  char script[COMMAND_LEN];

  appleQuote(title, text, sizeof(text));
  snprintf(script, sizeof(script), "POSIX path of (choose folder with prompt %s)", text);
  shellQuote(script, text, sizeof(text));
  snprintf(command, sizeof(command), "osascript -e %s 2>/dev/null", text);
#else
  char start[PLATFORM_PATH_LEN];

  shellQuote(title, text, sizeof(text));
  shellQuote((path[0] != '\0') ? path : ".", start, sizeof(start));

  if (commandExists("zenity"))
  {
    snprintf(command, sizeof(command), "zenity --file-selection --directory --title=%s --filename=%s/ 2>/dev/null", text, start);
  }
  else if (commandExists("kdialog"))
  {
    snprintf(command, sizeof(command), "kdialog --title %s --getexistingdirectory %s 2>/dev/null", text, start);
  }
  else
  {
    return false;
  }
#endif

  if (!commandPath(command, path, size))
  {
    return false;
  }

  // Without the last slash (osascript adds one)
  size_t len = strlen(path);

  if ((len > 1) && (path[len - 1] == '/'))
  {
    path[len - 1] = '\0';
  }

  return true;
}

void platformNotify(const char * title, const char * text)
{
  char command[COMMAND_LEN];
  char quotedTitle[PLATFORM_PATH_LEN];
  char quotedText[PLATFORM_PATH_LEN];

#ifdef __APPLE__
  char script[COMMAND_LEN];

  appleQuote(title, quotedTitle, sizeof(quotedTitle));
  appleQuote(text, quotedText, sizeof(quotedText));
  snprintf(script, sizeof(script), "display notification %s with title %s", quotedText, quotedTitle);
  shellQuote(script, quotedText, sizeof(quotedText));
  snprintf(command, sizeof(command), "osascript -e %s >/dev/null 2>&1 &", quotedText);
#else
  shellQuote(title, quotedTitle, sizeof(quotedTitle));
  shellQuote(text, quotedText, sizeof(quotedText));
  snprintf(command, sizeof(command), "notify-send %s %s >/dev/null 2>&1 &", quotedTitle, quotedText);
#endif

  // "&": the shell returns at once
  if (system(command) != 0)
  {
    // No notification tool: nothing shown
  }
}

// The first one found of the usual system fonts with accents
bool platformFallbackFontPath(char * path, size_t size)
{
  static const char * const fonts[] =
  {
#ifdef __APPLE__
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/Library/Fonts/Arial.ttf",
    "/System/Library/Fonts/Supplemental/Verdana.ttf",
#else
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
#endif
  };

  for (size_t i = 0; i < (sizeof(fonts) / sizeof(fonts[0])); ++i)
  {
    if (platformFileExists(fonts[i]))
    {
      snprintf(path, size, "%s", fonts[i]);
      return true;
    }
  }

  return false;
}

#endif

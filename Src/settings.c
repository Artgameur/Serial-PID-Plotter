// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Settings kept between sessions, in %APPDATA%\SerialDashboard\settings.ini
          ("key=value" lines). Changes are written about a second after the last one.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------
#include "settings.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <windows.h>

#include "profile.h"

// ================================================================================================
// ================================================================================================
//            PRIVATE DEFINE DECLARATION
// ================================================================================================
// ================================================================================================

#define SETTINGS_MAX          1024        // 32 variables x 7 settings, 8 plots x 10, commands, ... (~450 used)
#define SETTINGS_KEY_LEN      32
#define SETTINGS_VALUE_LEN    192         // room for a command with its values (cmd_panel.c)
#define SETTINGS_PATH_LEN     MAX_PATH
#define SETTINGS_DIR_NAME     "SerialDashboard"
#define SETTINGS_FILE_NAME    "settings.ini"
#define SETTINGS_PROFILE_DIR  "profiles"
#define SETTINGS_PROFILE_EXT  ".ini"
#define SETTINGS_SAVE_MS      1000

// ================================================================================================
// ================================================================================================
//            PRIVATE STRUCTURE DECLARATION
// ================================================================================================
// ================================================================================================

typedef struct
{
  char key[SETTINGS_KEY_LEN];
  char value[SETTINGS_VALUE_LEN];
} SettingsEntry_t;

// ================================================================================================
// ================================================================================================
//            PRIVATE VARIABLE DECLARATION
// ================================================================================================
// ================================================================================================

static SettingsEntry_t entries[SETTINGS_MAX];
static uint32_t        entryCount;
static char            filePath[SETTINGS_PATH_LEN];
static char            profileDir[SETTINGS_PATH_LEN];
static bool            dirty;

// First line of the exported files, by kind
static const char * const fileHeaders[] = {"# Serial Dashboard setup", "# Serial Dashboard plot layout"};

// Plot layout: these keys, "plot<n>.*" (each plot, its window) and these settings of the variables
static const char * const layoutKeys[]       = {"plotCount", "plotLength", "xAxis", "bars", "cmdPanel", "cmdMarks", "cmdWindow.geom"};
static const char * const layoutVarSuffixes[] = {".plots", ".color", ".width"};

// App settings, kept when a profile is loaded (the profiles hold the project settings only)
static const char * const appKeys[] = {"language", "uiScale", "maxFps", "theme", "accent", SETTINGS_PROFILE_KEY};

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

static void              pathInit(void);
static void              fileLoad(const char * path);
static bool              fileSave(const char * path, bool appSettings);
static bool              fileWrite(const char * path, bool appSettings);
static SettingsEntry_t * entryFind(const char * key);
static bool              keyIsApp(const char * key);
static bool              profilePath(const char * name, char * path, size_t size);
static bool              keyIsLayout(const char * key);

static void saveTimer(lv_timer_t * tim);

#ifdef SERIAL_DASHBOARD_CLI
#define SETTINGS_CLI_BINDINGS_MAX 256

static SettingsCliBinding_t cliBindings[SETTINGS_CLI_BINDINGS_MAX];
static uint32_t             cliBindingCount;

static void cliBindingAdd(lv_obj_t * obj, const char * key, SettingsCliKind_t kind)
{
  if (cliBindingCount < SETTINGS_CLI_BINDINGS_MAX)
  {
    cliBindings[cliBindingCount++] = (SettingsCliBinding_t) {key, kind, obj};
  }
}
#endif
static void dropdownEvent(lv_event_t * e);
static void textareaEvent(lv_event_t * e);
static void switchEvent(lv_event_t * e);

// ================================================================================================
// ================================================================================================
//            PUBLIC FUNCTION SECTION
// ================================================================================================
// ================================================================================================

void settingsInit(void)
{
  pathInit();
  fileLoad(filePath);
  dirty = false;
  lv_timer_create(saveTimer, SETTINGS_SAVE_MS, NULL);
  // Closing the window ends the process with exit() (LVGL's Windows driver): the changes of the
  // last second are written then
  atexit(settingsFlush);
}

// Changes written now (ex: before a restart), not at the next save timer
void settingsFlush(void)
{
  if (dirty)
  {
    dirty = !fileSave(filePath, true);
  }
}

bool settingsProfileNameValid(const char * name)
{
  size_t len = strlen(name);

  if ((len == 0) || (len >= SETTINGS_PROFILE_NAME_LEN) || (name[0] == ' ') || (name[len - 1] == ' ') ||
      (name[len - 1] == '.'))
  {
    return false;
  }

  // Used as a file name: letters, digits, space, '-', '_' and '.'
  for (size_t i = 0; i < len; ++i)
  {
    char c = name[i];

    if (!(((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')) ||
          (c == ' ') || (c == '-') || (c == '_') || (c == '.')))
    {
      return false;
    }
  }

  return true;
}

// Profile names in alphabetical order (file names in profiles\ without the extension)
uint32_t settingsProfileList(char names[][SETTINGS_PROFILE_NAME_LEN], uint32_t max)
{
  char             pattern[SETTINGS_PATH_LEN];
  WIN32_FIND_DATAA found;
  uint32_t         count = 0;

  snprintf(pattern, sizeof(pattern), "%s\\*%s", profileDir, SETTINGS_PROFILE_EXT);
  HANDLE find = FindFirstFileA(pattern, &found);

  if (find == INVALID_HANDLE_VALUE)
  {
    return 0;
  }

  do
  {
    size_t len = strlen(found.cFileName) - strlen(SETTINGS_PROFILE_EXT);

    if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (len == 0) || (len >= SETTINGS_PROFILE_NAME_LEN))
    {
      continue;
    }

    // Insertion sort, case insensitive
    char name[SETTINGS_PROFILE_NAME_LEN];
    snprintf(name, sizeof(name), "%.*s", (int) len, found.cFileName);

    uint32_t i = count;
    while ((i > 0) && (_stricmp(names[i - 1], name) > 0))
    {
      if (i < max)
      {
        memcpy(names[i], names[i - 1], SETTINGS_PROFILE_NAME_LEN);
      }
      --i;
    }

    if (i < max)
    {
      memcpy(names[i], name, SETTINGS_PROFILE_NAME_LEN);
      count = LV_MIN(count + 1, max);
    }
  }
  while (FindNextFileA(find, &found));

  FindClose(find);
  return count;
}

// The project settings (everything except the app settings) are written to profiles\<name>.ini
bool settingsProfileSave(const char * name)
{
  char path[SETTINGS_PATH_LEN];

  if (!profilePath(name, path, sizeof(path)) || !fileSave(path, false))
  {
    return false;
  }

  settingsSetString(SETTINGS_PROFILE_KEY, name);
  return true;
}

// The project settings are replaced by the profile's and settings.ini is written immediately; the
// widgets still show the previous settings (the screens read the settings once, when created)
bool settingsProfileLoad(const char * name)
{
  char     path[SETTINGS_PATH_LEN];
  uint32_t kept = 0;

  if (!profilePath(name, path, sizeof(path)) || (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES))
  {
    return false;
  }

  for (uint32_t i = 0; i < entryCount; ++i)
  {
    if (keyIsApp(entries[i].key))
    {
      entries[kept++] = entries[i];
    }
  }

  entryCount = kept;
  fileLoad(path);
  settingsSetString(SETTINGS_PROFILE_KEY, name);
  dirty = !fileSave(filePath, true);
  return !dirty;
}

bool settingsFileExport(const wchar_t * path, SettingsFileKind_t kind)
{
  FILE * file = _wfopen(path, L"w");

  if (file == NULL)
  {
    return false;
  }

  fprintf(file, "%s\n", fileHeaders[kind]);

  for (uint32_t i = 0; i < entryCount; ++i)
  {
    bool wanted = (kind == SETTINGS_FILE_LAYOUT) ? keyIsLayout(entries[i].key)
                                                 : (!keyIsApp(entries[i].key) && (strcmp(entries[i].key, SETTINGS_PROFILE_KEY) != 0));

    if (wanted)
    {
      fprintf(file, "%s=%s\n", entries[i].key, entries[i].value);
    }
  }

  return fclose(file) == 0;
}

bool settingsFileImport(const wchar_t * path, SettingsFileKind_t * kind)
{
  FILE *   file = _wfopen(path, L"r");
  char     text[SETTINGS_KEY_LEN + SETTINGS_VALUE_LEN + 4];
  uint32_t kept = 0;
  bool     found = false;

  if (file == NULL)
  {
    return false;
  }

  if (fgets(text, sizeof(text), file) != NULL)
  {
    text[strcspn(text, "\r\n")] = '\0';

    for (uint32_t k = 0; k < (sizeof(fileHeaders) / sizeof(fileHeaders[0])); ++k)
    {
      if (strcmp(text, fileHeaders[k]) == 0)
      {
        *kind = (SettingsFileKind_t) k;
        found = true;
      }
    }
  }

  if (!found)
  {
    fclose(file);
    return false;
  }

  // What the file replaces is removed first: a plot or variable it does not have goes back to
  // its default
  for (uint32_t i = 0; i < entryCount; ++i)
  {
    bool replaced = (*kind == SETTINGS_FILE_LAYOUT) ? keyIsLayout(entries[i].key) : !keyIsApp(entries[i].key);

    if (!replaced)
    {
      entries[kept++] = entries[i];
    }
  }

  entryCount = kept;

  while (fgets(text, sizeof(text), file) != NULL)
  {
    char * equal = strchr(text, '=');

    if (equal == NULL)
    {
      continue;
    }

    *equal = '\0';
    equal[1 + strcspn(&equal[1], "\r\n")] = '\0';

    // Only what that kind of file holds
    if ((*kind == SETTINGS_FILE_LAYOUT) ? keyIsLayout(text) : !keyIsApp(text))
    {
      settingsSetString(text, &equal[1]);
    }
  }

  fclose(file);

  // A setup is not the profile it may have come from
  if (*kind == SETTINGS_FILE_SETUP)
  {
    settingsSetString(SETTINGS_PROFILE_KEY, "");
  }

  dirty = !fileSave(filePath, true);
  return !dirty;
}

bool settingsProfileDelete(const char * name)
{
  char path[SETTINGS_PATH_LEN];

  if (!profilePath(name, path, sizeof(path)) || !DeleteFileA(path))
  {
    return false;
  }

  if (strcmp(settingsGetString(SETTINGS_PROFILE_KEY, ""), name) == 0)
  {
    settingsSetString(SETTINGS_PROFILE_KEY, "");
  }

  return true;
}

const char * settingsGetString(const char * key, const char * fallback)
{
  SettingsEntry_t * entry = entryFind(key);
  return (entry != NULL) ? entry->value : fallback;
}

int32_t settingsGetInt(const char * key, int32_t fallback)
{
  const char * text = settingsGetString(key, NULL);
  char *       end;

  if ((text == NULL) || (text[0] == '\0'))
  {
    return fallback;
  }

  long value = strtol(text, &end, 10);
  return (*end == '\0') ? (int32_t) value : fallback;
}

void settingsSetString(const char * key, const char * value)
{
  SettingsEntry_t * entry = entryFind(key);

  if (entry == NULL)
  {
    if (entryCount >= SETTINGS_MAX)
    {
      return;
    }

    entry = &entries[entryCount++];
    snprintf(entry->key, SETTINGS_KEY_LEN, "%s", key);
    entry->value[0] = '\0';
    dirty = true;
  }

  if (strncmp(entry->value, value, SETTINGS_VALUE_LEN - 1) != 0)
  {
    snprintf(entry->value, SETTINGS_VALUE_LEN, "%s", value);
    dirty = true;
  }
}

void settingsSetInt(const char * key, int32_t value)
{
  char text[16];
  snprintf(text, sizeof(text), "%ld", (long) value);
  settingsSetString(key, text);
}

void settingsBindDropdown(lv_obj_t * dropdown, const char * key)
{
  int32_t selected = settingsGetInt(key, -1);

  if ((selected >= 0) && ((uint32_t) selected < lv_dropdown_get_option_count(dropdown)))
  {
    lv_dropdown_set_selected(dropdown, (uint32_t) selected);
  }

  lv_obj_add_event_cb(dropdown, dropdownEvent, LV_EVENT_VALUE_CHANGED, (void *) key);
#ifdef SERIAL_DASHBOARD_CLI
  cliBindingAdd(dropdown, key, SETTINGS_CLI_DROPDOWN);
#endif
}

void settingsBindTextarea(lv_obj_t * textarea, const char * key)
{
  const char * text = settingsGetString(key, NULL);

  if (text != NULL)
  {
    lv_textarea_set_text(textarea, text);
  }

  lv_obj_add_event_cb(textarea, textareaEvent, LV_EVENT_VALUE_CHANGED, (void *) key);
#ifdef SERIAL_DASHBOARD_CLI
  cliBindingAdd(textarea, key, SETTINGS_CLI_TEXT);
#endif
}

void settingsBindSwitch(lv_obj_t * sw, const char * key)
{
  int32_t checked = settingsGetInt(key, -1);

  if (checked >= 0)
  {
    lv_obj_set_state(sw, LV_STATE_CHECKED, checked != 0);
  }

  lv_obj_add_event_cb(sw, switchEvent, LV_EVENT_VALUE_CHANGED, (void *) key);
#ifdef SERIAL_DASHBOARD_CLI
  cliBindingAdd(sw, key, SETTINGS_CLI_SWITCH);
#endif
}

#ifdef SERIAL_DASHBOARD_CLI
void settingsCliLoad(const char * path)
{
  fileLoad(path);
}

uint32_t settingsCliBindings(const SettingsCliBinding_t ** list)
{
  *list = cliBindings;
  return cliBindingCount;
}

bool settingsCliApply(const char * key, const char * value, bool * live, char * error, size_t size)
{
  *live = false;

  for (uint32_t b = 0; b < cliBindingCount; ++b)
  {
    const SettingsCliBinding_t * binding = &cliBindings[b];

    if (strcmp(binding->key, key) != 0)
    {
      continue;
    }

    if (binding->kind == SETTINGS_CLI_TEXT)
    {
      // Sends the change itself
      lv_textarea_set_text(binding->obj, value);
    }
    else if (binding->kind == SETTINGS_CLI_DROPDOWN)
    {
      char *  end;
      long    number = strtol(value, &end, 10);
      int32_t index  = ((end != value) && (*end == '\0')) ? (int32_t) number : lv_dropdown_get_option_index(binding->obj, value);

      if ((index < 0) || ((uint32_t) index >= lv_dropdown_get_option_count(binding->obj)))
      {
        snprintf(error, size, "%s: not one of its options (by number from 0, or by text): %s", key,
                 lv_dropdown_get_options(binding->obj));
        return false;
      }

      lv_dropdown_set_selected(binding->obj, (uint32_t) index);
      lv_obj_send_event(binding->obj, LV_EVENT_VALUE_CHANGED, NULL);
    }
    else
    {
      bool on  = (strcmp(value, "1") == 0) || (_stricmp(value, "on") == 0) || (_stricmp(value, "true") == 0);
      bool off = (strcmp(value, "0") == 0) || (_stricmp(value, "off") == 0) || (_stricmp(value, "false") == 0);

      if (!on && !off)
      {
        snprintf(error, size, "%s: on or off (1 / 0)", key);
        return false;
      }

      lv_obj_set_state(binding->obj, LV_STATE_CHECKED, on);
      lv_obj_send_event(binding->obj, LV_EVENT_VALUE_CHANGED, NULL);
    }

    *live = true;
  }

  if (!*live)
  {
    settingsSetString(key, value);
  }

  return true;
}
#endif

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION SECTION
// ================================================================================================
// ================================================================================================

// %APPDATA%\SerialDashboard\settings.ini, or next to the program when APPDATA is not set
static void pathInit(void)
{
  const char * appData = getenv("APPDATA");

  if ((appData != NULL) && (appData[0] != '\0'))
  {
    char dir[SETTINGS_PATH_LEN];
    snprintf(dir, sizeof(dir), "%s\\%s", appData, SETTINGS_DIR_NAME);
    CreateDirectoryA(dir, NULL);
    snprintf(filePath, sizeof(filePath), "%s\\%s", dir, SETTINGS_FILE_NAME);
    snprintf(profileDir, sizeof(profileDir), "%s\\%s", dir, SETTINGS_PROFILE_DIR);
  }
  else
  {
    snprintf(filePath, sizeof(filePath), "%s", SETTINGS_FILE_NAME);
    snprintf(profileDir, sizeof(profileDir), "%s", SETTINGS_PROFILE_DIR);
  }

  CreateDirectoryA(profileDir, NULL);
}

static void fileLoad(const char * path)
{
  FILE * file = fopen(path, "r");
  char   text[SETTINGS_KEY_LEN + SETTINGS_VALUE_LEN + 4];

  if (file == NULL)
  {
    return;
  }

  while (fgets(text, sizeof(text), file) != NULL)
  {
    char * equal = strchr(text, '=');

    if (equal == NULL)
    {
      continue;
    }

    *equal = '\0';
    equal[1 + strcspn(&equal[1], "\r\n")] = '\0';
    settingsSetString(text, &equal[1]);
  }

  fclose(file);
}

// appSettings = false: without the app settings (profile). Written to "<path>.tmp", which then
// replaces the file: a crash or power cut while writing leaves the previous file whole.
static bool fileSave(const char * path, bool appSettings)
{
  PROF_PUSH(PROF_SETTINGS_SAVE);
  bool ok = fileWrite(path, appSettings);
  PROF_POP();
  return ok;
}

static bool fileWrite(const char * path, bool appSettings)
{
  char   temp[SETTINGS_PATH_LEN + 8];
  FILE * file;

  if ((size_t) snprintf(temp, sizeof(temp), "%s.tmp", path) >= sizeof(temp))
  {
    return false;
  }

  file = fopen(temp, "w");

  if (file == NULL)
  {
    return false;
  }

  for (uint32_t i = 0; i < entryCount; ++i)
  {
    if (appSettings || !keyIsApp(entries[i].key))
    {
      fprintf(file, "%s=%s\n", entries[i].key, entries[i].value);
    }
  }

  // On the disk before it replaces the file
  bool ok = (fflush(file) == 0) && (_commit(_fileno(file)) == 0) && !ferror(file);

  ok = (fclose(file) == 0) && ok;
  ok = ok && MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);

  if (!ok)
  {
    DeleteFileA(temp);
  }

  return ok;
}

static SettingsEntry_t * entryFind(const char * key)
{
  for (uint32_t i = 0; i < entryCount; ++i)
  {
    if (strcmp(entries[i].key, key) == 0)
    {
      return &entries[i];
    }
  }

  return NULL;
}

static bool keyIsLayout(const char * key)
{
  const char * dot = strchr(key, '.');

  for (uint32_t i = 0; i < (sizeof(layoutKeys) / sizeof(layoutKeys[0])); ++i)
  {
    if (strcmp(layoutKeys[i], key) == 0)
    {
      return true;
    }
  }

  // "plot3.geom", "plot1.yMin", ...
  if ((strncmp(key, "plot", 4) == 0) && (key[4] >= '1') && (key[4] <= '9') && (dot != NULL))
  {
    return true;
  }

  // "var12.color", ...: not the name, unit, gain or offset (what the data means)
  if ((strncmp(key, "var", 3) == 0) && (key[3] >= '1') && (key[3] <= '9') && (dot != NULL))
  {
    for (uint32_t i = 0; i < (sizeof(layoutVarSuffixes) / sizeof(layoutVarSuffixes[0])); ++i)
    {
      if (strcmp(dot, layoutVarSuffixes[i]) == 0)
      {
        return true;
      }
    }
  }

  return false;
}

static bool keyIsApp(const char * key)
{
  for (uint32_t i = 0; i < (sizeof(appKeys) / sizeof(appKeys[0])); ++i)
  {
    if (strcmp(appKeys[i], key) == 0)
    {
      return true;
    }
  }

  return false;
}

static bool profilePath(const char * name, char * path, size_t size)
{
  if (!settingsProfileNameValid(name))
  {
    return false;
  }

  snprintf(path, size, "%s\\%s%s", profileDir, name, SETTINGS_PROFILE_EXT);
  return true;
}

static void saveTimer(lv_timer_t * tim)
{
  LV_UNUSED(tim);

  // A failed write (ex: file locked by another program) is tried again at the next tick
  if (dirty)
  {
    dirty = !fileSave(filePath, true);
  }
}

static void dropdownEvent(lv_event_t * e)
{
  settingsSetInt((const char *) lv_event_get_user_data(e),
                 (int32_t) lv_dropdown_get_selected(lv_event_get_target_obj(e)));
}

static void textareaEvent(lv_event_t * e)
{
  settingsSetString((const char *) lv_event_get_user_data(e), lv_textarea_get_text(lv_event_get_target_obj(e)));
}

static void switchEvent(lv_event_t * e)
{
  settingsSetInt((const char *) lv_event_get_user_data(e),
                 lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED) ? 1 : 0);
}

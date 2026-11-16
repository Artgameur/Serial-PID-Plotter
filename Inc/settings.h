// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Settings kept between sessions, in %APPDATA%\SerialDashboard\settings.ini
          ("key=value" lines). Changes are written about a second after the last one.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------

#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>

#include <stdbool.h>
#include <wchar.h>

#include "lvgl.h"

// ================================================================================================
// ================================================================================================
//            DEFINE DECLARATION
// ================================================================================================
// ================================================================================================

#define SETTINGS_PROFILE_NAME_LEN 48
#define SETTINGS_PROFILE_KEY      "profile"   // name of the last profile saved / loaded

// ================================================================================================
// ================================================================================================
//            EXTERNAL FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

// Loads the file; call after lv_init() and before creating the screens
void settingsInit(void);

// Writes the pending changes now (they are otherwise written about a second later)
void settingsFlush(void);

const char * settingsGetString(const char * key, const char * fallback);
int32_t      settingsGetInt(const char * key, int32_t fallback);
void         settingsSetString(const char * key, const char * value);
void         settingsSetInt(const char * key, int32_t value);

// Applies the saved value to the widget, then saves each change. key must stay valid (literal).
void settingsBindDropdown(lv_obj_t * dropdown, const char * key);
void settingsBindTextarea(lv_obj_t * textarea, const char * key);
void settingsBindSwitch(lv_obj_t * sw, const char * key);

// Files to share or back up a setup, anywhere (UTF-16 paths). The first line tells which kind.
typedef enum
{
  SETTINGS_FILE_SETUP,                    // the project settings, as a profile
  SETTINGS_FILE_LAYOUT,                   // the plots' layout only (see settings.c)
} SettingsFileKind_t;

bool settingsFileExport(const char * path, SettingsFileKind_t kind);

// A setup replaces the project settings, a layout only the plots' layout. settings.ini is written
// now; the screens show it after a restart. false: not such a file (nothing changed).
bool settingsFileImport(const char * path, SettingsFileKind_t * kind);

// Profiles: copies of the project settings (port, format, variables, plots, commands, ...) in
// %APPDATA%\SerialDashboard\profiles\<name>.ini. The app settings (scale, frame rate, colors)
// are not part of them. After a load, the screens must be created again (restart) to show it.
bool     settingsProfileNameValid(const char * name);
uint32_t settingsProfileList(char names[][SETTINGS_PROFILE_NAME_LEN], uint32_t max);
bool     settingsProfileSave(const char * name);
bool     settingsProfileLoad(const char * name);
bool     settingsProfileDelete(const char * name);

#ifdef SERIAL_DASHBOARD_CLI               // only in the command line tool (agent/), not in the app
typedef enum
{
  SETTINGS_CLI_TEXT,
  SETTINGS_CLI_DROPDOWN,
  SETTINGS_CLI_SWITCH,
} SettingsCliKind_t;

typedef struct
{
  const char *      key;
  SettingsCliKind_t kind;
  lv_obj_t *        obj;
} SettingsCliBinding_t;

// Reads a settings file (added to the settings already read); nothing is ever written back
void settingsCliLoad(const char * path);

// The settings shown by widgets
uint32_t settingsCliBindings(const SettingsCliBinding_t ** list);

// Sets key as if typed / chosen in its widget (the app reacts to it). A key without widget is
// only stored: it counts where the app reads it (ex: a connection's baud rate, when connecting).
// false: value not valid for the widget (error says why)
bool settingsCliApply(const char * key, const char * value, bool * live, char * error, size_t size);
#endif

#endif //SETTINGS_H

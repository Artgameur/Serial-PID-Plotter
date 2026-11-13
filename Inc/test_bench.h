// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Test bench: saves each finished test (CSV) and pictures of the graphs (PNG) in a folder.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------

#ifndef TEST_BENCH_H
#define TEST_BENCH_H

#include <stdbool.h>

#include "lvgl.h"
#include "test_data.h"

// ================================================================================================
// ================================================================================================
//            STRUCTURE DECLARATION
// ================================================================================================
// ================================================================================================

// Size of the pictures for a report, from their format (16:8), width on the page and resolution:
// drawn again at that size, not stretched, so they stay sharp
typedef struct
{
  int32_t           width;
  int32_t           height;
  int32_t           scale;                // percent of the sizes on screen (text, axes, lines)
  const lv_font_t * font;
  int32_t           ppi;                  // resolution, also written in the file
} BenchPicture_t;

// ================================================================================================
// ================================================================================================
//            EXTERNAL FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

// Box of the bench's settings in parent; group: gets its text box, for the PC keyboard
void benchBoxCreate(lv_obj_t * parent, lv_group_t * group);

// The Auto save switch is on
bool benchAutoSave(void);

// Windows notification of text, when the Notify switch is on
void benchNotify(const char * text);

// The size chosen for the pictures; false: as on screen
bool benchPictureGet(BenchPicture_t * picture);

// Writes test as <folder>\<date>_<time>_<test name>.csv (in the background) and the picture of obj
// as the same name .png. Failures are shown as a message.
void benchTestSave(const TestData_t * test, lv_obj_t * obj);

// Writes the picture of obj (with all its children, ex: several graphs) as
// <folder>\<date>_<time>_<name>.png, and tells where. The UI waits for it (a few 100 ms).
void benchScreenshot(lv_obj_t * obj, const char * name);

#ifdef SERIAL_DASHBOARD_CLI               // only in the command line tool (agent/), not in the app
// Report pictures of that format ("16:8"), width on the page (inches) and resolution (ppi): the
// bench's fields. false when the format is not two numbers above 0.
bool benchCliPictureSet(const char * format, const char * inches, const char * ppi);

// Writes the picture of obj at exactly path
bool benchCliPictureWrite(lv_obj_t * obj, const char * path);
#endif

#endif //TEST_BENCH_H

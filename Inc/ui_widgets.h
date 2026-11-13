// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Small widget helpers and colors shared by the Serial Dashboard screens.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------

#ifndef UI_WIDGETS_H
#define UI_WIDGETS_H

#include "lvgl.h"

// ================================================================================================
// ================================================================================================
//            DEFINE DECLARATION
// ================================================================================================
// ================================================================================================

#define COLOR_BAR_BG          0x1F2937
#define COLOR_BAR_TEXT        0xFFFFFF
#define COLOR_CONNECTED       0x22C55E
#define COLOR_ERROR           0xDC2626
#define COLOR_INFO            0x6B7280
#define COLOR_GRID            0xE5E7EB
#define COLOR_GRID_ZERO       0xC4C8CE
#define COLOR_GRID_DARK       0x374151
#define COLOR_GRID_ZERO_DARK  0x6B7280
#define COLOR_SELECTION       0x2563EB

#define UI_LABEL_WIDTH        90          // name column of the settings rows (before uiPx)
#define UI_LEGEND_DOT_PAD      13         // room for the dot in front of a legend's text (before uiPx)
#define UI_LINE_ROUND_WIDTH   3           // chart lines this thick get round joins

// ================================================================================================
// ================================================================================================
//            EXTERNAL FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

// UI scale (100 = normal), set once before the UI is created: the UI is drawn natively at that
// size (larger font, theme DPI, uiPx() sizes), which keeps text and lines sharp
void              uiScaleSet(int32_t percent);
int32_t           uiScaleGet(void);
int32_t           uiPx(int32_t px);
const lv_font_t * uiFont(void);

// Font for pictures larger than the screen: the one closest to px up to 28, Segoe UI above
const lv_font_t * uiFontAt(int32_t px);

// Area of an X axis label centered on x, moved inward when it would be cut at the ends of the axis
lv_area_t uiAxisLabelArea(const char * text, const lv_font_t * font, int32_t x, const lv_area_t * axis);

// Top of a Y axis label centered on y, moved inward when it would be cut at the ends of the axis
int32_t uiAxisLabelTop(int32_t y, int32_t lineHeight, const lv_area_t * axis);

void         uiStylesInit(void);
lv_style_t * uiStyleCompact(void);

// Light / dark theme with an accent color, applied to the whole UI immediately
void uiThemeApply(bool dark, lv_color_t accent);

// Light theme for a moment (a picture for a report), whatever the chosen one; false: back to it
void uiThemeLightSet(bool light);
bool uiThemeIsDark(void);

lv_obj_t * uiRowCreate(lv_obj_t * parent);
lv_obj_t * uiSettingRowCreate(lv_obj_t * parent, const char * name);
lv_obj_t * uiSectionTitleCreate(lv_obj_t * parent, const char * text);

// Dot in front of the label's text, in the label's text color
void uiLegendDotAdd(lv_obj_t * label);

// For LV_EVENT_DRAW_TASK_ADDED of a line chart. With at least one point per pixel column, lv_chart
// draws a vertical bar per column: its line width only thickens it in X, so a flat curve stays
// 1 pixel high. The bars are made that much taller too. Other lines are real lines, anti-aliased.
void uiChartLineThicken(lv_draw_line_dsc_t * line);

// Bordered box: header (title, and what the box applies to, added by the caller in *header), then
// its rows (returned)
lv_obj_t * uiBoxCreate(lv_obj_t * parent, const char * title, lv_obj_t ** header);
lv_obj_t * uiButtonCreate(lv_obj_t * parent, const char * text, lv_event_cb_t cb);
lv_obj_t * uiDropdownCreate(lv_obj_t * parent, const char * options, uint32_t selected);
lv_obj_t * uiTextareaCreate(lv_obj_t * parent, const char * placeholder);

// Small "presets" button (only the arrow) at the right of a text box: its list opens to the left,
// so it stays in the window; picking an option calls cb (VALUE_CHANGED)
lv_obj_t * uiPresetDropdownCreate(lv_obj_t * parent, const char * options, lv_event_cb_t cb);

// Hidden panel covering its parent (outside its layout, above the other children) with a title
// and a close button (closeCb). Returns the panel; *content is its scrollable settings column.
lv_obj_t * uiOverlayCreate(lv_obj_t * parent, const char * title, lv_event_cb_t closeCb, lv_obj_t ** content);

// Visible scrollbar for a container whose styles were removed (lv_obj_remove_style_all)
void uiScrollbarShow(lv_obj_t * obj);

// The PC keyboard types in the text boxes of group (NULL: nowhere)
void uiKeypadGroupSet(lv_group_t * group);

// Tooltip: text shown next to the mouse after it rests about a second on obj (or on its children).
// text must stay valid (literal / tr()). uiTooltipsInit: once, after the screen is created.
void uiTooltipsInit(void);
void uiTooltipSet(lv_obj_t * obj, const char * text);

#endif //UI_WIDGETS_H

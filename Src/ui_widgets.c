// ------------------------------------------------------------------------------------------------
/*!@file
   @brief Small widget helpers and colors shared by the Serial Dashboard screens.

   @author artgameur
   @version 1.0
*/
// ------------------------------------------------------------------------------------------------
#include "ui_widgets.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "app_window.h"
#include "platform.h"

// ================================================================================================
// ================================================================================================
//            PRIVATE DEFINE DECLARATION
// ================================================================================================
// ================================================================================================


#define WHEEL_SCROLL_PX       48          // per wheel notch, at 100 % UI scale
#define WHEEL_NOTCH           120         // wheel delta of a notch

// Tooltips: shown after the pointer rests TOOLTIP_DELAY_MS on the same object (checked every
// TOOLTIP_POLL_MS), below right of the pointer
#define TOOLTIP_DELAY_MS      1000
#define TOOLTIP_POLL_MS       100
#define TOOLTIP_MAX_WIDTH     uiPx(260)
#define TOOLTIP_OFFSET_X      uiPx(12)
#define TOOLTIP_OFFSET_Y      uiPx(20)

// ================================================================================================
// ================================================================================================
//            PRIVATE VARIABLE DECLARATION
// ================================================================================================
// ================================================================================================

static lv_style_t styleCompact;
static bool       themeDark;
static lv_color_t themeAccent;
static int32_t    uiScale = 100;

#define UI_LARGE_FONTS 8

// Fonts enabled in lv_conf.h; uiFont() picks the one closest to 14 px x scale
static const lv_font_t * const fonts[] =
{
  &lv_font_montserrat_10, &lv_font_montserrat_12, &lv_font_montserrat_14, &lv_font_montserrat_16,
  &lv_font_montserrat_18, &lv_font_montserrat_20, &lv_font_montserrat_22, &lv_font_montserrat_24,
  &lv_font_montserrat_26, &lv_font_montserrat_28,
};
static const int32_t fontSizes[] = {10, 12, 14, 16, 18, 20, 22, 24, 26, 28};

// UI font: copy of the chosen Montserrat font whose fallback is Segoe UI at the same size, so
// accents ("é", "ç"...) are drawn (created once: the size is fixed at startup)
static lv_font_t   uiFontCopy;
static bool        uiFontReady;
static lv_font_t * fallbackFont;
static void *      fallbackData;          // Segoe UI file, kept for the fonts made from it
static size_t      fallbackSize;

// Tooltip: object under the pointer (compared only, it may be deleted), since when, label shown
static lv_obj_t *  tooltipTarget;
static uint32_t    tooltipSince;
static lv_obj_t *  tooltipLabel;

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION DECLARATION
// ================================================================================================
// ================================================================================================

static void         legendDotDrawEvent(lv_event_t * e);
static void         tooltipMarkerEvent(lv_event_t * e);
static const char * tooltipTextGet(lv_obj_t * obj);
static void         tooltipTimer(lv_timer_t * tim);

// ================================================================================================
// ================================================================================================
//            PUBLIC FUNCTION SECTION
// ================================================================================================
// ================================================================================================

// The display DPI scales the default theme (paddings, radius, borders) and LV_DPX() sizes
void uiScaleSet(int32_t percent)
{
  uiScale = percent;
  lv_display_set_dpi(lv_display_get_default(), (LV_DPI_DEF * percent) / 100);
}

int32_t uiScaleGet(void)
{
  return uiScale;
}

// Size in pixels at the UI scale (sizes in the code are given for 100 %)
int32_t uiPx(int32_t px)
{
  return ((px * uiScale) + 50) / 100;
}

const lv_font_t * uiFontAt(int32_t px)
{
  static lv_font_t   copy;
  static lv_font_t * large[UI_LARGE_FONTS];
  static int32_t     largeSizes[UI_LARGE_FONTS];
  uint32_t           best = 0;

  // Larger than the built-in fonts: made once per size, a session uses few of them
  for (uint32_t i = 0; (i < UI_LARGE_FONTS) && (px > fontSizes[(sizeof(fontSizes) / sizeof(fontSizes[0])) - 1]) && (fallbackData != NULL); ++i)
  {
    if (large[i] == NULL)
    {
      large[i]      = lv_tiny_ttf_create_data(fallbackData, fallbackSize, px);
      largeSizes[i] = px;
    }

    if ((large[i] != NULL) && (largeSizes[i] == px))
    {
      return large[i];
    }
  }

  for (uint32_t i = 1; i < (sizeof(fontSizes) / sizeof(fontSizes[0])); ++i)
  {
    if (LV_ABS(fontSizes[i] - px) < LV_ABS(fontSizes[best] - px))
    {
      best = i;
    }
  }

  // With the accents of the UI font
  copy = *fonts[best];
  copy.fallback = fallbackFont;
  return &copy;
}

lv_area_t uiAxisLabelArea(const char * text, const lv_font_t * font, int32_t x, const lv_area_t * axis)
{
  lv_point_t size;

  lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

  // 2 more: not narrower than the text once rounded, it would wrap
  int32_t width = size.x + 2;
  int32_t left  = LV_MAX(axis->x1, LV_MIN(x - (width / 2), axis->x2 - width + 1));

  return (lv_area_t) {left, axis->y1, left + width - 1, axis->y2};
}

int32_t uiAxisLabelTop(int32_t y, int32_t lineHeight, const lv_area_t * axis)
{
  return LV_MAX(axis->y1, LV_MIN(y - (lineHeight / 2), axis->y2 - lineHeight + 1));
}

const lv_font_t * uiFont(void)
{
  int32_t  target = uiPx(14);
  uint32_t best = 0;

  if (uiFontReady)
  {
    return &uiFontCopy;
  }

  for (uint32_t i = 1; i < (sizeof(fontSizes) / sizeof(fontSizes[0])); ++i)
  {
    if (LV_ABS(fontSizes[i] - target) < LV_ABS(fontSizes[best] - target))
    {
      best = i;
    }
  }

  // The TTF data must stay valid while the font is used: kept for the whole session
  char   path[PLATFORM_PATH_LEN];
  FILE * file = platformFallbackFontPath(path, sizeof(path)) ? platformFopen(path, "rb") : NULL;

  if (file != NULL)
  {
    long   size = (fseek(file, 0, SEEK_END) == 0) ? ftell(file) : -1;
    void * data = (size > 0) ? malloc((size_t) size) : NULL;

    if ((data != NULL) && (fseek(file, 0, SEEK_SET) == 0) && (fread(data, 1, (size_t) size, file) == (size_t) size))
    {
      fallbackFont = lv_tiny_ttf_create_data(data, (size_t) size, fontSizes[best]);
      fallbackData = data;
      fallbackSize = (size_t) size;
    }

    if (fallbackFont == NULL)
    {
      free(data);
      fallbackData = NULL;
    }

    fclose(file);
  }

  // Without the file: Montserrat alone (accents not shown)
  uiFontCopy = *fonts[best];
  uiFontCopy.fallback = fallbackFont;
  uiFontReady = true;
  return &uiFontCopy;
}

void uiStylesInit(void)
{
  lv_style_init(&styleCompact);
  lv_style_set_pad_ver(&styleCompact, uiPx(5));
  lv_style_set_pad_hor(&styleCompact, uiPx(8));
}

// Smaller paddings than the default theme, to fit a 320 x 240 window
lv_style_t * uiStyleCompact(void)
{
  return &styleCompact;
}

// Re-initializing the default theme updates its styles, and every widget is refreshed
void uiThemeApply(bool dark, lv_color_t accent)
{
  lv_display_t * display = lv_display_get_default();

  themeDark   = dark;
  themeAccent = accent;
  lv_display_set_theme(display, lv_theme_default_init(display, accent, lv_palette_main(LV_PALETTE_RED), dark,
                                                      uiFont()));
  lv_obj_report_style_change(NULL);
}

void uiThemeLightSet(bool light)
{
  static bool chosenDark;

  if (light && themeDark)
  {
    chosenDark = true;
    uiThemeApply(false, themeAccent);
  }
  else if (!light && chosenDark)
  {
    chosenDark = false;
    uiThemeApply(true, themeAccent);
  }
}

bool uiThemeIsDark(void)
{
  return themeDark;
}

// Transparent horizontal flex container, as high as its content
lv_obj_t * uiRowCreate(lv_obj_t * parent)
{
  lv_obj_t * row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, uiPx(6), 0);
  lv_obj_set_scrollable(row, false);
  return row;
}

// "Name    [widget added by the caller]" row
lv_obj_t * uiSettingRowCreate(lv_obj_t * parent, const char * name)
{
  lv_obj_t * row = uiRowCreate(parent);

  lv_obj_t * label = lv_label_create(row);
  lv_obj_set_width(label, uiPx(UI_LABEL_WIDTH));
  lv_obj_set_style_text_color(label, lv_color_hex(COLOR_INFO), 0);
  lv_label_set_text(label, name);

  return row;
}

lv_obj_t * uiSectionTitleCreate(lv_obj_t * parent, const char * text)
{
  lv_obj_t * title = lv_label_create(parent);
  lv_label_set_text(title, text);
  lv_obj_set_style_pad_top(title, uiPx(6), 0);
  return title;
}

lv_obj_t * uiBoxCreate(lv_obj_t * parent, const char * title, lv_obj_t ** header)
{
  lv_obj_t * box = lv_obj_create(parent);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(box, uiPx(6), 0);
  lv_obj_set_style_pad_row(box, uiPx(6), 0);
  lv_obj_set_style_radius(box, uiPx(6), 0);
  lv_obj_set_style_border_width(box, LV_MAX(1, uiPx(1)), 0);
  lv_obj_set_style_border_color(box, lv_color_hex(COLOR_INFO), 0);
  lv_obj_set_style_border_opa(box, LV_OPA_50, 0);
  lv_obj_set_scrollable(box, false);

  *header = uiRowCreate(box);
  lv_obj_set_style_pad_bottom(*header, uiPx(2), 0);

  lv_obj_t * label = lv_label_create(*header);
  lv_obj_set_width(label, uiPx(UI_LABEL_WIDTH));
  lv_label_set_text(label, title);

  return box;
}

int32_t uiChartPoint(float value, float yMin, float yMax)
{
  if (isnan(value))
  {
    return LV_CHART_POINT_NONE;
  }

  double scaled = (((double) value - yMin) * UI_CHART_RES) / ((double) yMax - yMin);
  return (int32_t) lround(LV_CLAMP(-(double) UI_CHART_CLAMP, scaled, (double) UI_CHART_CLAMP));
}

// Bars come as 3 points: top, bottom on the same X, and an end of line. A real line only gets
// round joins, where thick segments would show notches.
void uiChartLineThicken(lv_draw_line_dsc_t * line)
{
  int32_t above = (line->width - 1) / 2;
  int32_t below = (line->width - 1) - above;

  for (int32_t p = 0; (p + 2) < line->point_cnt; p += 3)
  {
    lv_point_precise_t * bar = &((lv_point_precise_t *) line->points)[p];

    if ((bar[0].x != bar[1].x) || (bar[2].y != LV_DRAW_LINE_POINT_NONE))
    {
      line->round_start = line->width >= UI_LINE_ROUND_WIDTH;
      line->round_end   = line->round_start;
      return;
    }

    bar[0].y -= above;
    bar[1].y += below;
  }
}

void uiLegendDotAdd(lv_obj_t * label)
{
  lv_obj_set_style_pad_left(label, uiPx(UI_LEGEND_DOT_PAD), 0);
  lv_obj_add_event_cb(label, legendDotDrawEvent, LV_EVENT_DRAW_MAIN, NULL);
}

lv_obj_t * uiButtonCreate(lv_obj_t * parent, const char * text, lv_event_cb_t cb)
{
  lv_obj_t * btn = lv_button_create(parent);
  lv_obj_add_style(btn, &styleCompact, 0);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t * label = lv_label_create(btn);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  return btn;
}

// Fills the remaining width of a row; options must stay valid (static string)
lv_obj_t * uiDropdownCreate(lv_obj_t * parent, const char * options, uint32_t selected)
{
  lv_obj_t * dropdown = lv_dropdown_create(parent);
  lv_obj_add_style(dropdown, &styleCompact, 0);
  lv_obj_set_flex_grow(dropdown, 1);
  lv_dropdown_set_options_static(dropdown, options);
  lv_dropdown_set_selected(dropdown, selected);
  return dropdown;
}

lv_obj_t * uiOverlayCreate(lv_obj_t * parent, const char * title, lv_event_cb_t closeCb, lv_obj_t ** content)
{
  lv_obj_t * panel = lv_obj_create(parent);
  lv_obj_set_floating(panel, true);
  lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
  lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_radius(panel, 0, 0);
  lv_obj_set_style_border_width(panel, 0, 0);
  lv_obj_set_style_pad_all(panel, 0, 0);
  lv_obj_set_style_pad_row(panel, 0, 0);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scrollable(panel, false);
  lv_obj_set_hidden(panel, true);

  // Title and close button
  lv_obj_t * header = uiRowCreate(panel);
  lv_obj_set_style_pad_all(header, uiPx(4), 0);
  lv_obj_set_style_pad_left(header, uiPx(8), 0);

  lv_obj_t * label = lv_label_create(header);
  lv_obj_set_flex_grow(label, 1);
  lv_label_set_text(label, title);

  uiButtonCreate(header, LV_SYMBOL_CLOSE, closeCb);

  *content = lv_obj_create(panel);
  lv_obj_remove_style_all(*content);
  lv_obj_set_width(*content, LV_PCT(100));
  lv_obj_set_flex_grow(*content, 1);
  lv_obj_set_flex_flow(*content, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(*content, uiPx(6), 0);
  lv_obj_set_style_pad_row(*content, uiPx(6), 0);
  lv_obj_set_scrollable(*content, true);
  lv_obj_set_scroll_dir(*content, LV_DIR_VER);
  uiScrollbarShow(*content);

  return panel;
}

void uiScrollbarShow(lv_obj_t * obj)
{
  lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_width(obj, uiPx(4), LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(obj, uiPx(2), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_color(obj, lv_color_hex(COLOR_INFO), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(obj, LV_OPA_60, LV_PART_SCROLLBAR);
  lv_obj_set_style_pad_right(obj, uiPx(2), LV_PART_SCROLLBAR);
}

void uiWheelScroll(lv_point_t point, int32_t delta)
{
  int32_t step = LV_MAX(1, (WHEEL_SCROLL_PX * (int32_t) lv_display_get_dpi(NULL)) / LV_DPI_DEF);
  int32_t dy   = (delta * step) / WHEEL_NOTCH;

  // Objects on the top layer (above the screen) first, then the screen (an open dropdown list is
  // the screen's last child, so it is found before what is behind it)
  lv_obj_t * hit = lv_indev_search_obj(lv_layer_top(), &point);

  if (hit == NULL)
  {
    hit = lv_indev_search_obj(lv_screen_active(), &point);
  }

  for (lv_obj_t * obj = hit; obj != NULL; obj = lv_obj_get_parent(obj))
  {
    bool canScroll = (dy > 0) ? (lv_obj_get_scroll_top(obj) > 0) : (lv_obj_get_scroll_bottom(obj) > 0);

    if (lv_obj_is_scrollable(obj) && canScroll)
    {
      lv_obj_scroll_by_bounded(obj, 0, dy, LV_ANIM_OFF);
      return;
    }
  }
}

void uiKeypadGroupSet(lv_group_t * group)
{
  for (lv_indev_t * indev = lv_indev_get_next(NULL); indev != NULL; indev = lv_indev_get_next(indev))
  {
    if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD)
    {
      lv_indev_set_group(indev, group);
    }
  }
}

lv_obj_t * uiPresetDropdownCreate(lv_obj_t * parent, const char * options, lv_event_cb_t cb)
{
  lv_obj_t * dropdown = lv_dropdown_create(parent);

  lv_obj_add_style(dropdown, &styleCompact, 0);
  lv_obj_set_width(dropdown, uiPx(40));
  lv_dropdown_set_options_static(dropdown, options);
  lv_dropdown_set_text(dropdown, "");

  // Opening downwards, the list starts at the button's left edge and its wider options went out
  // of the window on the right
  lv_dropdown_set_dir(dropdown, LV_DIR_LEFT);
  lv_dropdown_set_symbol(dropdown, LV_SYMBOL_DOWN);

  lv_obj_add_event_cb(dropdown, cb, LV_EVENT_VALUE_CHANGED, NULL);
  return dropdown;
}

// Label on the top layer (above everything), not clickable: the search under the pointer skips it
void uiTooltipsInit(void)
{
  tooltipLabel = lv_label_create(lv_layer_top());
  lv_obj_set_clickable(tooltipLabel, false);
  lv_label_set_long_mode(tooltipLabel, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_max_width(tooltipLabel, TOOLTIP_MAX_WIDTH, 0);
  lv_obj_set_style_bg_color(tooltipLabel, lv_color_hex(COLOR_BAR_BG), 0);
  lv_obj_set_style_bg_opa(tooltipLabel, LV_OPA_90, 0);
  lv_obj_set_style_text_color(tooltipLabel, lv_color_hex(COLOR_BAR_TEXT), 0);
  lv_obj_set_style_pad_hor(tooltipLabel, uiPx(8), 0);
  lv_obj_set_style_pad_ver(tooltipLabel, uiPx(4), 0);
  lv_obj_set_style_radius(tooltipLabel, uiPx(4), 0);
  lv_obj_set_hidden(tooltipLabel, true);

  lv_timer_create(tooltipTimer, TOOLTIP_POLL_MS, NULL);
}

// The text is kept as the user data of an event callback that does nothing (no table to clean
// up: it goes away with the object)
void uiTooltipSet(lv_obj_t * obj, const char * text)
{
  lv_obj_add_event_cb(obj, tooltipMarkerEvent, LV_EVENT_DELETE, (void *) text);
}

// One-line text box filling the remaining width of a row
lv_obj_t * uiTextareaCreate(lv_obj_t * parent, const char * placeholder)
{
  lv_obj_t * textarea = lv_textarea_create(parent);
  lv_obj_add_style(textarea, &styleCompact, 0);
  lv_obj_set_flex_grow(textarea, 1);
  lv_textarea_set_one_line(textarea, true);
  lv_textarea_set_placeholder_text(textarea, placeholder);
  return textarea;
}

// ================================================================================================
// ================================================================================================
//            PRIVATE FUNCTION SECTION
// ================================================================================================
// ================================================================================================

// Centered on the first line of the text, sized with it
static void legendDotDrawEvent(lv_event_t * e)
{
  lv_obj_t *         label = lv_event_get_target_obj(e);
  int32_t            line  = lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN));
  int32_t            size  = (line * 9) / 16;
  lv_area_t          coords;
  lv_draw_rect_dsc_t dotDsc;

  lv_obj_get_coords(label, &coords);

  lv_area_t dot = {coords.x1, coords.y1 + ((line - size) / 2), coords.x1 + size - 1, coords.y1 + ((line - size) / 2) + size - 1};

  lv_draw_rect_dsc_init(&dotDsc);
  dotDsc.bg_color = lv_obj_get_style_text_color(label, LV_PART_MAIN);
  dotDsc.radius   = LV_RADIUS_CIRCLE;
  lv_draw_rect(lv_event_get_layer(e), &dotDsc, &dot);
}

static void tooltipMarkerEvent(lv_event_t * e)
{
  LV_UNUSED(e);
}

// Tooltip of obj, or of its nearest parent that has one (ex: the label inside a button); NULL: none
static const char * tooltipTextGet(lv_obj_t * obj)
{
  for (; obj != NULL; obj = lv_obj_get_parent(obj))
  {
    for (uint32_t i = 0; i < lv_obj_get_event_count(obj); ++i)
    {
      lv_event_dsc_t * dsc = lv_obj_get_event_dsc(obj, i);

      if (lv_event_dsc_get_cb(dsc) == tooltipMarkerEvent)
      {
        return (const char *) lv_event_dsc_get_user_data(dsc);
      }
    }
  }

  return NULL;
}

// Shown once the pointer rests TOOLTIP_DELAY_MS on the same object; hidden when it moves to
// another one, a button is pressed, or the mouse leaves the window
static void tooltipTimer(lv_timer_t * tim)
{
  lv_indev_t * pointer = NULL;
  lv_point_t   point;
  lv_obj_t *   hit = NULL;

  LV_UNUSED(tim);

  for (lv_indev_t * indev = lv_indev_get_next(NULL); indev != NULL; indev = lv_indev_get_next(indev))
  {
    if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER)
    {
      pointer = indev;
      break;
    }
  }

  if ((pointer != NULL) && appWindowPointerInside() && (lv_indev_get_state(pointer) == LV_INDEV_STATE_RELEASED))
  {
    // Top layer first (above the screen), like the pointer's own search
    lv_indev_get_point(pointer, &point);
    hit = lv_indev_search_obj(lv_layer_top(), &point);
    if (hit == NULL)
    {
      hit = lv_indev_search_obj(lv_screen_active(), &point);
    }
  }

  if (hit != tooltipTarget)
  {
    tooltipTarget = hit;
    tooltipSince  = lv_tick_get();
    lv_obj_set_hidden(tooltipLabel, true);
    return;
  }

  const char * text = tooltipTextGet(hit);

  if ((text == NULL) || !lv_obj_is_hidden(tooltipLabel) || (lv_tick_elaps(tooltipSince) < TOOLTIP_DELAY_MS))
  {
    return;
  }

  // Below right of the pointer, moved back inside the window when it would go out
  lv_label_set_text_static(tooltipLabel, text);
  lv_obj_set_hidden(tooltipLabel, false);
  lv_obj_update_layout(tooltipLabel);

  int32_t width  = lv_obj_get_width(tooltipLabel);
  int32_t height = lv_obj_get_height(tooltipLabel);
  int32_t maxX   = lv_display_get_horizontal_resolution(NULL) - width;
  int32_t maxY   = lv_display_get_vertical_resolution(NULL) - height;
  int32_t x      = point.x + TOOLTIP_OFFSET_X;
  int32_t y      = point.y + TOOLTIP_OFFSET_Y;

  if (y > maxY)
  {
    // No room below: above the pointer
    y = point.y - height - uiPx(4);
  }

  lv_obj_set_pos(tooltipLabel, LV_CLAMP(0, x, LV_MAX(0, maxX)), LV_CLAMP(0, y, LV_MAX(0, maxY)));
  lv_obj_move_foreground(tooltipLabel);
}

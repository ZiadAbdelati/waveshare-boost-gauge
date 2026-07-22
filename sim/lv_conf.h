/**
 * LVGL 9 config for the desktop boost-gauge simulator.
 * Keep fonts/widgets aligned with firmware needs.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

#define LV_COLOR_DEPTH 32

#define LV_USE_STDLIB_MALLOC  LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING  LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_MEM_SIZE (512 * 1024U)

#define LV_USE_OS LV_OS_NONE

#define LV_DEF_REFR_PERIOD 16
#define LV_DPI_DEF 130

#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_DRAW_UNIT_CNT 1

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_FONT_COMPRESSED 0
#define LV_USE_FONT_PLACEHOLDER 1

#define LV_TXT_ENC LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS " ,.;:-_"

#define LV_USE_ARC 1
#define LV_USE_BAR 1
#define LV_USE_BUTTON 1
#define LV_USE_IMAGE 1
#define LV_USE_LABEL 1
#define LV_USE_LINE 1
#define LV_USE_SLIDER 1
#define LV_USE_SWITCH 1
#define LV_USE_TEXTAREA 1

#define LV_USE_FLEX 1
#define LV_USE_GRID 1

#define LV_USE_SNAPSHOT 1

/* Optional windowed mode when a display is available. */
/* Headless screenshots need no SDL. Set to 1 (and install SDL2 dev headers)
 * only if you want the interactive --window mode. */
#define LV_USE_SDL 0
#if LV_USE_SDL
    #define LV_SDL_INCLUDE_PATH     <SDL2/SDL.h>
    #define LV_SDL_RENDER_MODE      LV_DISPLAY_RENDER_MODE_DIRECT
    #define LV_SDL_BUF_COUNT        1
    #define LV_SDL_ACCELERATED      0
    #define LV_SDL_FULLSCREEN       0
    #define LV_SDL_DIRECT_EXIT      1
    #define LV_SDL_MOUSEWHEEL_MODE  LV_SDL_MOUSEWHEEL_MODE_ENCODER
#endif

#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

#endif /* LV_CONF_H */

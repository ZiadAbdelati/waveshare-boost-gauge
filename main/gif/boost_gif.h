/**
 * @file boost_gif.h
 *
 * PROJECT-OWNED wrapper around LVGL's public `lv_gif` widget header.
 *
 * The widget implementation is the project-owned copy at boost_gif.c, which
 * keeps including the upstream header for the public API. Any BOOST extension
 * to that API is declared here, because editing the upstream header under
 * managed_components/ is silently lost on the next dependency refresh (the
 * lv_gif_set_speed() declaration previously lived only there and broke the
 * build when a fresh checkout regenerated the component).
 */

#ifndef BOOST_GIF_H
#define BOOST_GIF_H

#include "libs/gif/lv_gif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Set the playback speed multiplier. 1.0 is the authored rate (default);
 * 0.5 holds each frame for half its authored delay (2x playback). The
 * multiplier only shortens the on-screen hold, never starts a frame before
 * the preceding one's scaled window has elapsed.
 * @param obj       pointer to a gif object
 * @param speed     speed multiplier (must be > 0)
 */
void lv_gif_set_speed(lv_obj_t * obj, float speed);

#ifdef __cplusplus
}
#endif

#endif /*BOOST_GIF_H*/
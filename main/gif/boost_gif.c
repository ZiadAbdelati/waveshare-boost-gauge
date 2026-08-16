/**
 * @file boost_gif.c
 *
 * PROJECT-OWNED COPY of LVGL's `lv_gif` widget.
 *
 * Upstream: managed_components/lvgl__lvgl/src/libs/gif/lv_gif.c
 *
 * `managed_components/` is git-ignored and is rewritten by any dependency
 * refresh, so edits made there are silently lost - which is exactly what
 * happened to the framebuffer zero-init this project used to carry (README
 * "Intentional local LVGL changes" described edits that were no longer in the
 * tree). The upstream sources under LVGL's src/libs/gif directory are excluded
 * from the build in the top-level CMakeLists.txt and this file defines the same
 * symbols, so
 * `main/boost_gauge.c` needs no change: it keeps including
 * "libs/gif/lv_gif.h" (upstream's *header*, i.e. the public API declarations)
 * and keeps calling lv_gif_create()/lv_gif_set_src()/...
 *
 * Divergences from upstream are marked "BOOST:".
 *   1. The framebuffer is zero-initialised (upstream leaves it uninitialised,
 *      so frame 1 renders over whatever was already in PSRAM).
 *   2. The dead 8-bit palette-index plane is gone - the allocation is now
 *      w*h*bpp instead of w*h*(bpp+1) and the cooked pixels start at offset 0.
 *      See boost_gif_dec.c's banner for why that plane is provably dead in
 *      GIF_DRAW_COOKED mode.
 *   3. Per-frame invalidation is bounded by the GIF frame's own rectangle
 *      instead of invalidating the whole widget.
 */

/*********************
 *      INCLUDES
 *********************/
#include "libs/gif/lv_gif.h"
#if LV_USE_GIF
#include "misc/lv_timer_private.h"
#include "misc/cache/lv_cache.h"
#include "core/lv_obj_class_private.h"
#include "core/lv_obj_private.h"
#include "widgets/image/lv_image_private.h"
#include "boost_gif_dec.h"
#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "boost_media_store.h"
#include "boost_display.h"
#endif

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "boost_media_store.h"
/* BOOST: safety interlock for ALLOWS_UNALIGNED.
 * GIFMakePels() copies 4 bytes at a time into GIFIMAGE::ucLineBuf and
 * deliberately overruns by up to 3 bytes (worst case last byte written is index
 * iWidth + 2). The decoder's own hard cap is MAX_WIDTH - GIFInit() rejects
 * wider canvases - and ucLineBuf carries GIF_LINEBUF_SLACK bytes past it, so
 * the overrun is contained for any canvas the decoder accepts. This assert
 * additionally ties the upload cap to that decoder cap, so raising
 * BOOST_MEDIA_STORE_MAX_DIMENSION past what the decoder can handle fails the
 * build instead of the panel. */
_Static_assert(BOOST_MEDIA_STORE_MAX_DIMENSION <= MAX_WIDTH,
               "BOOST_MEDIA_STORE_MAX_DIMENSION exceeds the GIF decoder MAX_WIDTH; "
               "raise MAX_WIDTH in main/gif/boost_gif_dec.h");
_Static_assert(GIF_LINEBUF_SLACK >= 4,
               "ALLOWS_UNALIGNED wide copies overrun ucLineBuf by up to 3 bytes");
#endif

/*********************
 *      DEFINES
 *********************/
#define MY_CLASS (&lv_gif_class)

/** BOOST: bound each frame's invalidation to the GIF frame rect. Set to 0 to
 * fall back to upstream's whole-widget invalidation. */
#ifndef BOOST_GIF_DIRTY_RECT
    #define BOOST_GIF_DIRTY_RECT 1
#endif

/** BOOST: rate-limited log of the mean frame-rect area, so the dirty-rect
 * fraction F can be measured on hardware. It cannot be derived from the source
 * - it is a property of whichever GIF the user uploaded. */
#ifndef BOOST_GIF_LOG_DIRTY_RECT
    #define BOOST_GIF_LOG_DIRTY_RECT 1
#endif
#ifndef BOOST_GIF_DIRTY_LOG_FRAMES
    #define BOOST_GIF_DIRTY_LOG_FRAMES 120
#endif

#if BOOST_GIF_LOG_DIRTY_RECT && defined(ESP_PLATFORM)
    #define BOOST_GIF_LOG(...) ESP_LOGI("boost_gif", __VA_ARGS__)
#else
    #define BOOST_GIF_LOG(...) do { } while(0)
#endif

/**********************
 *      TYPEDEFS
 **********************/

/* the type of the AnimatedGIF pallete type passed to `GIF_begin` */
typedef unsigned char animatedgif_color_format_t;

typedef struct {
    lv_image_t img;
    /* BOOST: heap-allocated, not embedded, so it can be placed in INTERNAL RAM.
     * LVGL allocates widgets from its own builtin pool (LV_USE_STDLIB_MALLOC
     * falls back to LV_STDLIB_BUILTIN here - CONFIG_LV_USE_CLIB_MALLOC is NOT
     * the symbol lv_conf_internal.h reads), so the object never goes through
     * malloc and heap_caps_malloc_extmem_enable() cannot reach it. The ~24 KB
     * of LZW tables in here are the decoder's hottest memory and the inner
     * loop is a dependent pointer chase, so PSRAM latency dominates. */
    GIFIMAGE * gif;
    const void * src;
    lv_color_format_t color_format;
    lv_timer_t * timer;
    lv_image_dsc_t imgdsc;
    int32_t loop_count;
    uint32_t is_open : 1;
    uint32_t force_full_invalidate : 1; /* BOOST: first frame after (re)open */
#if BOOST_GIF_LOG_DIRTY_RECT
    uint32_t stat_frames;
    uint32_t stat_rect_px;
    uint32_t stat_full;
#endif
#ifdef ESP_PLATFORM
    /* BOOST: dual-core pipelined decode + direct push state */
    void * framebuffers[2];
    uint8_t write_fb_idx;
    uint8_t read_fb_idx;
    TaskHandle_t decode_task;
    SemaphoreHandle_t sem_frame_ready;
    SemaphoreHandle_t sem_next_decode;
    volatile bool task_running;
    int ms_delay_next;
    int last_has_next;
    int32_t last_fx, last_fy, last_fw, last_fh;

    /* BOOST: direct-push + timing stats. decode_us/push_us accumulate totals
     * for the rate-limited summary; last_* keep the instantaneous values for
     * that same log line. push_failures counts fallbacks to the LVGL path. */
    uint32_t stat_decode_us;
    uint32_t stat_push_us;
    uint32_t stat_push_frames;
    uint32_t stat_push_failures;
    uint32_t last_decode_us;
    uint32_t last_push_us;
    int8_t push_active; /* -1 unknown, 0 LVGL path, 1 direct push */
#endif
} lv_gif_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void lv_gif_constructor(const lv_obj_class_t * class_p, lv_obj_t * obj);
static void lv_gif_destructor(const lv_obj_class_t * class_p, lv_obj_t * obj);
static void initialize(lv_gif_t * gifobj);
static void next_frame_task_cb(lv_timer_t * t);
static void invalidate_frame(lv_gif_t * gifobj);

/**********************
 *  STATIC VARIABLES
 **********************/

const lv_obj_class_t lv_gif_class = {
    .constructor_cb = lv_gif_constructor,
    .destructor_cb = lv_gif_destructor,
    .instance_size = sizeof(lv_gif_t),
    .base_class = &lv_image_class,
    .name = "lv_gif",
};

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * lv_gif_create(lv_obj_t * parent)
{

    LV_LOG_INFO("begin");
    lv_obj_t * obj = lv_obj_class_create_obj(MY_CLASS, parent);
    lv_obj_class_init_obj(obj);
    return obj;
}

void lv_gif_set_color_format(lv_obj_t * obj, lv_color_format_t color_format)
{
    lv_gif_t * gifobj = (lv_gif_t *) obj;

    if(gifobj->color_format == color_format) {
        return;
    }

    switch(color_format) {
        case LV_COLOR_FORMAT_RGB565:
        case LV_COLOR_FORMAT_RGB565_SWAPPED:
        case LV_COLOR_FORMAT_RGB888:
        case LV_COLOR_FORMAT_ARGB8888:
            break;
        default:
            LV_LOG_WARN("gif widget does not support this color format");
            return;
    }

    gifobj->color_format = color_format;

    if(gifobj->src != NULL) {
        initialize(gifobj);
    }
}

void lv_gif_set_src(lv_obj_t * obj, const void * src)
{
    lv_gif_t * gifobj = (lv_gif_t *) obj;

    gifobj->src = src;

    initialize(gifobj);
}

void lv_gif_restart(lv_obj_t * obj)
{
    lv_gif_t * gifobj = (lv_gif_t *) obj;

    if(!gifobj->is_open) {
        LV_LOG_WARN("Gif resource not loaded correctly");
        return;
    }

    GIF_reset(gifobj->gif);
    gifobj->loop_count = -1; /* match the behavior of the old library */
    gifobj->force_full_invalidate = 1; /* BOOST: the canvas is about to jump */
    lv_timer_resume(gifobj->timer);
    lv_timer_reset(gifobj->timer);
}

void lv_gif_pause(lv_obj_t * obj)
{
    lv_gif_t * gifobj = (lv_gif_t *) obj;
    lv_timer_pause(gifobj->timer);
}

void lv_gif_resume(lv_obj_t * obj)
{
    lv_gif_t * gifobj = (lv_gif_t *) obj;

    if(!gifobj->is_open) {
        LV_LOG_WARN("Gif resource not loaded correctly");
        return;
    }

    lv_timer_resume(gifobj->timer);
}

bool lv_gif_is_loaded(lv_obj_t * obj)
{
    lv_gif_t * gifobj = (lv_gif_t *) obj;

    return gifobj->is_open;
}

int32_t lv_gif_get_loop_count(lv_obj_t * obj)
{
    lv_gif_t * gifobj = (lv_gif_t *) obj;

    if(!gifobj->is_open) {
        return -1;
    }

    return gifobj->loop_count;
}

void lv_gif_set_loop_count(lv_obj_t * obj, int32_t count)
{
    lv_gif_t * gifobj = (lv_gif_t *) obj;

    if(!gifobj->is_open) {
        LV_LOG_WARN("Gif resource not loaded correctly");
        return;
    }

    gifobj->loop_count = count;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void lv_gif_constructor(const lv_obj_class_t * class_p, lv_obj_t * obj)
{
    LV_UNUSED(class_p);

    lv_gif_t * gifobj = (lv_gif_t *) obj;

    gifobj->color_format = LV_COLOR_FORMAT_ARGB8888;
    gifobj->is_open = 0;
    gifobj->force_full_invalidate = 1;
    /* NOT internal RAM. Placing this ~24.5 KB here starves the Wi-Fi driver:
     * the GIF loads at boot before SoftAP attach, leaving ~49 KB internal, and
     * ieee80211_hostap_attach() then faults on a failed allocation - a boot
     * loop, not a graceful degradation. The decoder's LZW tables would like to
     * be internal, but not at the cost of the radio. Revisit only with a hard
     * reserve measured against peak Wi-Fi usage, and only for the hottest few
     * KB rather than the whole struct. */
    gifobj->gif = lv_malloc(sizeof(GIFIMAGE));
    LV_ASSERT_MALLOC(gifobj->gif);
    if(gifobj->gif != NULL) lv_memset(gifobj->gif, 0, sizeof(GIFIMAGE));
    gifobj->timer = lv_timer_create(next_frame_task_cb, 10, obj);
    lv_timer_pause(gifobj->timer);
}

static void lv_gif_destructor(const lv_obj_class_t * class_p, lv_obj_t * obj)
{
    LV_UNUSED(class_p);
    lv_gif_t * gifobj = (lv_gif_t *) obj;

    lv_image_cache_drop(lv_image_get_src(obj));

#ifdef ESP_PLATFORM
    if(gifobj->decode_task != NULL) {
        gifobj->task_running = false;
        xSemaphoreGive(gifobj->sem_next_decode);
        vTaskDelay(pdMS_TO_TICKS(15));
        gifobj->decode_task = NULL;
    }
    if(gifobj->sem_frame_ready != NULL) {
        vSemaphoreDelete(gifobj->sem_frame_ready);
        gifobj->sem_frame_ready = NULL;
    }
    if(gifobj->sem_next_decode != NULL) {
        vSemaphoreDelete(gifobj->sem_next_decode);
        gifobj->sem_next_decode = NULL;
    }
    if(gifobj->framebuffers[0] != NULL) {
        lv_free(gifobj->framebuffers[0]);
        gifobj->framebuffers[0] = NULL;
    }
    if(gifobj->framebuffers[1] != NULL) {
        lv_free(gifobj->framebuffers[1]);
        gifobj->framebuffers[1] = NULL;
    }
#endif

    if(gifobj->is_open) {
        void * framebuffer = gifobj->gif->pFrameBuffer;
        if(gifobj->gif->pTurboBuffer != NULL) {
            lv_free(gifobj->gif->pTurboBuffer);
            gifobj->gif->pTurboBuffer = NULL;
        }
#ifdef ESP_PLATFORM
        if(gifobj->gif->pTurboSymbols != NULL) {
            free(gifobj->gif->pTurboSymbols);
            gifobj->gif->pTurboSymbols = NULL;
        }
        if(gifobj->gif->pTurboLengths != NULL) {
            free(gifobj->gif->pTurboLengths);
            gifobj->gif->pTurboLengths = NULL;
        }
#endif
        GIF_close(gifobj->gif);
        if(framebuffer != NULL && framebuffer != gifobj->framebuffers[0] && framebuffer != gifobj->framebuffers[1]) {
            lv_free(framebuffer);
        }
    }
    lv_free(gifobj->gif);
    gifobj->gif = NULL;
    lv_timer_delete(gifobj->timer);
}

#ifdef ESP_PLATFORM
static void gif_decode_task(void * arg)
{
    lv_gif_t * gifobj = (lv_gif_t *)arg;
    while(gifobj->task_running) {
        if(xSemaphoreTake(gifobj->sem_next_decode, portMAX_DELAY) != pdTRUE) {
            break;
        }
        if(!gifobj->task_running) {
            break;
        }

        uint8_t write_idx = gifobj->write_fb_idx;
        uint8_t prev_idx = 1 - write_idx;
        size_t fb_size = (size_t)gifobj->gif->iCanvasWidth * gifobj->gif->iCanvasHeight * sizeof(uint16_t);

        /* Only copy previous canvas if the frame has transparency or is a partial sub-rect.
         * Full-frame opaque frames will overwrite every pixel, so skipping 434 KB PSRAM memcpy saves ~1.3 ms. */
        const bool is_full_opaque = (gifobj->gif->iX == 0 && gifobj->gif->iY == 0 &&
                                     gifobj->gif->iWidth == gifobj->gif->iCanvasWidth &&
                                     gifobj->gif->iHeight == gifobj->gif->iCanvasHeight &&
                                     !(gifobj->gif->ucGIFBits & 1));
        if(!is_full_opaque && gifobj->framebuffers[prev_idx] != NULL && gifobj->framebuffers[write_idx] != NULL) {
            memcpy(gifobj->framebuffers[write_idx], gifobj->framebuffers[prev_idx], fb_size);
        }

        gifobj->gif->pFrameBuffer = gifobj->framebuffers[write_idx];

        int64_t t0 = esp_timer_get_time();
        int ms_delay = 0;
        gifobj->last_has_next = GIF_playFrame(gifobj->gif, &ms_delay, gifobj);
        gifobj->ms_delay_next = ms_delay;
        gifobj->last_decode_us = (uint32_t)(esp_timer_get_time() - t0);
        gifobj->stat_decode_us += gifobj->last_decode_us;

        xSemaphoreGive(gifobj->sem_frame_ready);
    }
    vTaskDelete(NULL);
}
#endif

static void initialize(lv_gif_t * gifobj)
{
    GIFIMAGE * gif = gifobj->gif;

    /*Close previous gif if any*/
    if(gifobj->is_open) {
        lv_image_cache_drop(lv_image_get_src((lv_obj_t *) gifobj));

#ifdef ESP_PLATFORM
        if(gifobj->decode_task != NULL) {
            gifobj->task_running = false;
            xSemaphoreGive(gifobj->sem_next_decode);
            vTaskDelay(pdMS_TO_TICKS(15));
            gifobj->decode_task = NULL;
        }
        if(gifobj->sem_frame_ready != NULL) {
            vSemaphoreDelete(gifobj->sem_frame_ready);
            gifobj->sem_frame_ready = NULL;
        }
        if(gifobj->sem_next_decode != NULL) {
            vSemaphoreDelete(gifobj->sem_next_decode);
            gifobj->sem_next_decode = NULL;
        }
        if(gifobj->framebuffers[0] != NULL) {
            lv_free(gifobj->framebuffers[0]);
            gifobj->framebuffers[0] = NULL;
        }
        if(gifobj->framebuffers[1] != NULL) {
            lv_free(gifobj->framebuffers[1]);
            gifobj->framebuffers[1] = NULL;
        }
#endif

        void * framebuffer = gif->pFrameBuffer;
        if(gif->pTurboBuffer != NULL) {
            lv_free(gif->pTurboBuffer);
            gif->pTurboBuffer = NULL;
        }
#ifdef ESP_PLATFORM
        if(gif->pTurboSymbols != NULL) {
            free(gif->pTurboSymbols);
            gif->pTurboSymbols = NULL;
        }
        if(gif->pTurboLengths != NULL) {
            free(gif->pTurboLengths);
            gif->pTurboLengths = NULL;
        }
#endif
        GIF_close(gif);
        if(framebuffer != NULL
#ifdef ESP_PLATFORM
           && framebuffer != gifobj->framebuffers[0] && framebuffer != gifobj->framebuffers[1]
#endif
          ) {
            lv_free(framebuffer);
        }
        gifobj->is_open = 0;
        gifobj->imgdsc.data = NULL;
    }

    animatedgif_color_format_t decoder_cf;
    uint32_t pixel_size_bytes;
    switch(gifobj->color_format) {
        case LV_COLOR_FORMAT_RGB565:
            decoder_cf = GIF_PALETTE_RGB565_LE;
            pixel_size_bytes = 2;
            break;
        case LV_COLOR_FORMAT_RGB565_SWAPPED:
            decoder_cf = GIF_PALETTE_RGB565_BE;
            pixel_size_bytes = 2;
            break;
        case LV_COLOR_FORMAT_RGB888:
            decoder_cf = GIF_PALETTE_RGB888;
            pixel_size_bytes = 3;
            break;
        case LV_COLOR_FORMAT_ARGB8888:
            decoder_cf = GIF_PALETTE_RGB8888;
            pixel_size_bytes = 4;
            break;
        default:
            return;
    }

    GIF_begin(gif, decoder_cf);

    if(lv_image_src_get_type(gifobj->src) == LV_IMAGE_SRC_VARIABLE) {
        const lv_image_dsc_t * img_dsc = gifobj->src;
        gifobj->is_open = GIF_openRAM(gif, (uint8_t *) img_dsc->data, img_dsc->data_size, NULL);
    }
    else if(lv_image_src_get_type(gifobj->src) == LV_IMAGE_SRC_FILE) {
        gifobj->is_open = GIF_openFile(gif, gifobj->src, NULL);
    }
    if(gifobj->is_open == 0) {
        LV_LOG_WARN("Couldn't load the source");
        return;
    }

    uint32_t width = GIF_getCanvasWidth(gif);
    uint32_t height = GIF_getCanvasHeight(gif);
    uint32_t framebuffer_size = width * height * pixel_size_bytes;

#ifdef ESP_PLATFORM
    /* BOOST: dual-core double-buffered pipeline. Allocate two PSRAM framebuffers
     * so Core 0 decodes frame N+1 in background while Core 1 pushes frame N. */
    gifobj->framebuffers[0] = lv_malloc(framebuffer_size);
    gifobj->framebuffers[1] = lv_malloc(framebuffer_size);
    LV_ASSERT_MALLOC(gifobj->framebuffers[0]);
    LV_ASSERT_MALLOC(gifobj->framebuffers[1]);
    if(gifobj->framebuffers[0] == NULL || gifobj->framebuffers[1] == NULL) {
        LV_LOG_WARN("Couldn't allocate double framebuffers for GIF");
        GIF_close(gif);
        gifobj->is_open = 0;
        return;
    }
    lv_memset(gifobj->framebuffers[0], 0, framebuffer_size);
    lv_memset(gifobj->framebuffers[1], 0, framebuffer_size);

    gif->pFrameBuffer = gifobj->framebuffers[0];
    gif->ucDrawType = GIF_DRAW_COOKED;
    gifobj->write_fb_idx = 0;
    gifobj->read_fb_idx = 0;
    gifobj->sem_frame_ready = xSemaphoreCreateBinary();
    gifobj->sem_next_decode = xSemaphoreCreateBinary();
    gifobj->task_running = true;
#else
    gif->pFrameBuffer = lv_malloc(framebuffer_size);
    gif->ucDrawType = GIF_DRAW_COOKED;
    LV_ASSERT_MALLOC(gif->pFrameBuffer);
    if(gif->pFrameBuffer == NULL) {
        LV_LOG_WARN("Couldn't allocate a buffer for a GIF");
        GIF_close(gif);
        gifobj->is_open = 0;
        return;
    }
    lv_memset(gif->pFrameBuffer, 0, framebuffer_size);
#endif

#if GIF_SUPPORT_TURBO
    /* BOOST: allocate turbo LZW buffer (~242 KB in PSRAM).
     * Decodes whole-frame byte streams without reverse-stack walks. */
    const uint32_t turbo_size = (width * height) + 256 + (4096 * sizeof(uint32_t)) + (4096 * sizeof(uint16_t));
    gif->pTurboBuffer = lv_malloc(turbo_size);
    if(gif->pTurboBuffer != NULL) {
        lv_memset(gif->pTurboBuffer, 0, turbo_size);
    }
#ifdef ESP_PLATFORM
    /* BOOST: allocate symbols (16 KB) and lengths (8 KB) from internal RAM for single-cycle latency */
    gif->pTurboSymbols = heap_caps_malloc(4096 * sizeof(uint32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    gif->pTurboLengths = heap_caps_malloc(4096 * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
#endif

    gifobj->imgdsc.data = gif->pFrameBuffer; /* BOOST: cooked plane is at offset 0 now */
    gifobj->imgdsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    gifobj->imgdsc.header.flags = LV_IMAGE_FLAGS_MODIFIABLE;
    gifobj->imgdsc.header.cf = gifobj->color_format;
    gifobj->imgdsc.header.h = height;
    gifobj->imgdsc.header.w = width;
    gifobj->imgdsc.header.stride = width * pixel_size_bytes;
    gifobj->imgdsc.data_size = framebuffer_size;

    lv_image_set_src((lv_obj_t *) gifobj, &gifobj->imgdsc);

    gifobj->loop_count = GIF_getLoopCount(gifobj->gif);
    gifobj->force_full_invalidate = 1;
#if BOOST_GIF_LOG_DIRTY_RECT
    gifobj->stat_frames = 0;
    gifobj->stat_rect_px = 0;
    gifobj->stat_full = 0;
#endif
#ifdef ESP_PLATFORM
    gifobj->stat_decode_us = 0;
    gifobj->stat_push_us = 0;
    gifobj->stat_push_frames = 0;
    gifobj->stat_push_failures = 0;
    gifobj->last_decode_us = 0;
    gifobj->last_push_us = 0;

    /* Decode frame 0 synchronously so widget has initial content */
    int ms_delay0 = 0;
    gifobj->last_has_next = GIF_playFrame(gif, &ms_delay0, gifobj);
    gifobj->ms_delay_next = ms_delay0;

    /* Spawn background decode task pinned to Core 0 with priority 5 */
    xTaskCreatePinnedToCore(gif_decode_task, "gif_dec", 4096, gifobj, 5, &gifobj->decode_task, 0);

    /* Prime decoder to start decoding frame 1 in background into buffer 1 */
    gifobj->write_fb_idx = 1;
    xSemaphoreGive(gifobj->sem_next_decode);
#endif

    lv_timer_resume(gifobj->timer);
    lv_timer_reset(gifobj->timer);

    next_frame_task_cb(gifobj->timer);

}

/**
 * BOOST: invalidate only the rectangle this frame actually touched.
 *
 * This is correct *for AnimatedGIF specifically*, because composition happens
 * entirely inside the current frame's rect. DrawCooked() is driven line by line
 * from GIFMakePels() over (iX, iY, iWidth, iHeight) only; a transparent pixel
 * either leaves the previous cooked pixel untouched (disposal 0/1) or is
 * overwritten with the background colour (disposal 2), and both cases stay
 * inside that rect. Nothing outside it is ever written, and GIFParseInfo()
 * rejects any frame whose rect is not fully inside the canvas.
 *
 * If a future decoder change ever composes *outside* the current frame rect -
 * e.g. a real "restore to previous"/"restore to background" disposal that
 * repaints the PREVIOUS frame's rect - this bound stops being valid and must
 * be widened to the union of the previous and current rects.
 *
 * Falls back to a whole-widget invalidation whenever the image placement is not
 * a plain 1:1 blit, and on the first frame after (re)opening a source.
 */
static void invalidate_frame(lv_gif_t * gifobj)
{
    lv_obj_t * obj = (lv_obj_t *) gifobj;
    GIFIMAGE * gif = gifobj->gif;
    bool bounded = false;
    LV_UNUSED(gif); /* only read by the bounded path and the stats below */
#if BOOST_GIF_LOG_DIRTY_RECT
    uint32_t dirty_px = (uint32_t)((int32_t) gif->iCanvasWidth * (int32_t) gif->iCanvasHeight);
#endif

#if BOOST_GIF_DIRTY_RECT
    lv_image_t * img = &gifobj->img;

    const int32_t fx = gif->iX;
    const int32_t fy = gif->iY;
    const int32_t fw = gif->iWidth;
    const int32_t fh = gif->iHeight;

    if(!gifobj->force_full_invalidate
       && img->align < _LV_IMAGE_ALIGN_AUTO_TRANSFORM
       && img->rotation == 0
       && img->scale_x == LV_SCALE_NONE
       && img->scale_y == LV_SCALE_NONE
       && fw > 0 && fh > 0
       && fx + fw <= (int32_t) gif->iCanvasWidth
       && fy + fh <= (int32_t) gif->iCanvasHeight
       && img->w == (int32_t) gif->iCanvasWidth
       && img->h == (int32_t) gif->iCanvasHeight) {

        /* Mirror lv_image_t's own placement maths (lv_image.c draw hook): the
         * image area is anchored at obj->coords, sized to the bitmap, then
         * aligned inside obj->coords with the inner-align mode and offset. */
        lv_area_t img_area;
        lv_area_set(&img_area, obj->coords.x1, obj->coords.y1,
                    obj->coords.x1 + img->w - 1, obj->coords.y1 + img->h - 1);
        lv_area_align(&obj->coords, &img_area, img->align, img->offset.x, img->offset.y);

        lv_area_t dirty;
        dirty.x1 = img_area.x1 + fx;
        dirty.y1 = img_area.y1 + fy;
        dirty.x2 = dirty.x1 + fw - 1;
        dirty.y2 = dirty.y1 + fh - 1;

        /* CO5300 AMOLED QSPI controller requires even x1/y1 and odd x2/y2
         * window edges (2-pixel/word aligned). Rounding matches rounder_event_cb
         * in boost_display.c so CASET/RASET hardware windows never suffer 1-pixel
         * phase shifts or horizontal streaking on partial delta frames. */
        dirty.x1 = (dirty.x1 >> 1) << 1;
        dirty.y1 = (dirty.y1 >> 1) << 1;
        dirty.x2 = ((dirty.x2 >> 1) << 1) + 1;
        dirty.y2 = ((dirty.y2 >> 1) << 1) + 1;

        /* Clamp to img_area bounds */
        if(dirty.x1 < img_area.x1) dirty.x1 = (img_area.x1 >> 1) << 1;
        if(dirty.y1 < img_area.y1) dirty.y1 = (img_area.y1 >> 1) << 1;
        if(dirty.x2 > img_area.x2) dirty.x2 = ((img_area.x2 >> 1) << 1) + 1;
        if(dirty.y2 > img_area.y2) dirty.y2 = ((img_area.y2 >> 1) << 1) + 1;

        const int32_t r_fx = dirty.x1 - img_area.x1;
        const int32_t r_fy = dirty.y1 - img_area.y1;
        const int32_t r_fw = dirty.x2 - dirty.x1 + 1;
        const int32_t r_fh = dirty.y2 - dirty.y1 + 1;

#ifdef ESP_PLATFORM
        /* BOOST direct push: the cooked framebuffer already holds panel-ready
         * RGB565 for exactly this rect, so instead of asking LVGL to re-blit
         * it through the render pipeline, push the strips ourselves. Valid
         * under the same placement conditions as the bounded invalidation
         * below (1:1 blit), plus panel rotation 0, which the push API checks.
         * img_area is in screen coords, which equal panel coords only
         * unrotated. On any push refusal we fall through to the ordinary
         * bounded LVGL invalidation. */
        const int64_t push_t0 = esp_timer_get_time();
        /* x1/y1 are end-EXCLUSIVE (esp_lcd_panel_draw_bitmap convention).
         * Since dirty.x1/y1 are even and dirty.x2/y2 are odd, dirty.x2+1 and
         * dirty.y2+1 are even, preserving CO5300 2-pixel alignment. */
        const uint16_t * read_fb = (const uint16_t *)(gifobj->framebuffers[gifobj->read_fb_idx] != NULL ?
                                                        gifobj->framebuffers[gifobj->read_fb_idx] : gif->pFrameBuffer);
        esp_err_t pushed = boost_display_push_bitmap(
            dirty.x1, dirty.y1,
            dirty.x2 + 1, dirty.y2 + 1,
            read_fb + (size_t)r_fy * gif->iCanvasWidth + r_fx,
            (int32_t)gif->iCanvasWidth);
        if(pushed == ESP_OK) {
            gifobj->last_push_us = (uint32_t)(esp_timer_get_time() - push_t0);
            gifobj->stat_push_us += gifobj->last_push_us;
            gifobj->stat_push_frames++;
            bounded = true; /* panel updated; skip the LVGL invalidation */
#if BOOST_GIF_LOG_DIRTY_RECT
            dirty_px = (uint32_t)(r_fw * r_fh);
#endif
        }
        else {
            gifobj->stat_push_failures++;
#endif /* ESP_PLATFORM */

            lv_obj_invalidate_area(obj, &dirty);
            bounded = true;
#if BOOST_GIF_LOG_DIRTY_RECT
            dirty_px = (uint32_t)(r_fw * r_fh);
#endif
#ifdef ESP_PLATFORM
        }
#endif /* ESP_PLATFORM */
    }
#endif /*BOOST_GIF_DIRTY_RECT*/

    if(!bounded) {
        lv_obj_invalidate(obj);
    }
    gifobj->force_full_invalidate = 0;

#if BOOST_GIF_LOG_DIRTY_RECT
    gifobj->stat_frames++;
    gifobj->stat_rect_px += dirty_px;
    if(!bounded) gifobj->stat_full++;

    if(gifobj->stat_frames >= BOOST_GIF_DIRTY_LOG_FRAMES) {
        uint32_t canvas_px = (uint32_t)((int32_t) gif->iCanvasWidth * (int32_t) gif->iCanvasHeight);
        uint32_t mean_px = gifobj->stat_rect_px / gifobj->stat_frames;
        uint32_t pct = canvas_px ? (uint32_t)(((uint64_t) gifobj->stat_rect_px * 100u) /
                                              ((uint64_t) canvas_px * gifobj->stat_frames)) : 0u;
        BOOST_GIF_LOG("dirty rect: %u frames, mean %u px = %u%% of %ux%u canvas, %u full invalidations",
                      (unsigned) gifobj->stat_frames, (unsigned) mean_px, (unsigned) pct,
                      (unsigned) gif->iCanvasWidth, (unsigned) gif->iCanvasHeight,
                      (unsigned) gifobj->stat_full);
#ifdef ESP_PLATFORM
        /* BOOST: decode/push split for the same window. decode covers every
         * decoded frame (stat_frames counts all of them, push or not); push
         * covers only direct-pushed frames. last_* give the instantaneous
         * values of the final frame in the window. */
        BOOST_GIF_LOG("perf: %u frames, decode mean %u us / last %u us, "
                      "push %u frames mean %u us / last %u us, delay %d ms, %u push fallbacks",
                      (unsigned) gifobj->stat_frames,
                      (unsigned) (gifobj->stat_decode_us / gifobj->stat_frames),
                      (unsigned) gifobj->last_decode_us,
                      (unsigned) gifobj->stat_push_frames,
                      (unsigned) (gifobj->stat_push_frames ? gifobj->stat_push_us / gifobj->stat_push_frames : 0),
                      (unsigned) gifobj->last_push_us,
                      gifobj->ms_delay_next,
                      (unsigned) gifobj->stat_push_failures);
        gifobj->stat_decode_us = 0;
        gifobj->stat_push_us = 0;
        gifobj->stat_push_frames = 0;
        gifobj->stat_push_failures = 0;
#endif
        gifobj->stat_frames = 0;
        gifobj->stat_rect_px = 0;
        gifobj->stat_full = 0;
    }
#endif
}

static void next_frame_task_cb(lv_timer_t * t)
{
    lv_obj_t * obj = (lv_obj_t *)lv_timer_get_user_data(t);
    lv_gif_t * gifobj = (lv_gif_t *) obj;

#ifdef ESP_PLATFORM
    if(gifobj->decode_task != NULL) {
        /* Wait for frame to finish decoding on Core 0 */
        if(xSemaphoreTake(gifobj->sem_frame_ready, pdMS_TO_TICKS(100)) == pdTRUE) {
            /* Ready frame is in write_fb_idx; switch active buffer to it */
            gifobj->read_fb_idx = gifobj->write_fb_idx;
            gifobj->gif->pFrameBuffer = gifobj->framebuffers[gifobj->read_fb_idx];
            gifobj->imgdsc.data = gifobj->framebuffers[gifobj->read_fb_idx];

            /* Kick Core 0 decoder to start decoding frame N+1 into alternate buffer */
            gifobj->write_fb_idx = 1 - gifobj->read_fb_idx;
            xSemaphoreGive(gifobj->sem_next_decode);

            /* Push frame N to panel via DMA on Core 1 while Core 0 decodes frame N+1! */
            lv_image_cache_drop(lv_image_get_src(obj));
            invalidate_frame(gifobj);

            if(gifobj->last_has_next <= 0) {
                gifobj->force_full_invalidate = 1;
                lv_result_t res = lv_obj_send_event(obj, LV_EVENT_READY, NULL);
                if(gifobj->loop_count > 0) {
                    if(gifobj->loop_count == 1) {
                        lv_timer_pause(t);
                    }
                    else {
                        gifobj->loop_count--;
                    }
                }
                if(res != LV_RESULT_OK) return;
            }
            else {
                /* Subtract push duration from frame delay so effective frame cadence
                 * matches the GIF's authored rate instead of serializing push + delay. */
                int remaining_ms = gifobj->ms_delay_next - (int)(gifobj->last_push_us / 1000);
                lv_timer_set_period(gifobj->timer, remaining_ms > 1 ? remaining_ms : 1);
            }
            return;
        }
    }
#endif
    int ms_delay_next;
    int has_next = GIF_playFrame(gifobj->gif, &ms_delay_next, gifobj);
    if(has_next <= 0) {
        /*It was the last repeat*/
        /* BOOST: nothing was necessarily decoded on this call (empty frame,
         * decode error, or end of file), so the frame rect may be stale -
         * repaint everything. At most once per animation loop. */
        gifobj->force_full_invalidate = 1;
        lv_result_t res = lv_obj_send_event(obj, LV_EVENT_READY, NULL);
        if(gifobj->loop_count > 0) {
            if(gifobj->loop_count == 1) {
                lv_timer_pause(t);
            }
            else {
                gifobj->loop_count--;
            }
        }
        if(res != LV_RESULT_OK) return;
    }
    else {
        lv_timer_set_period(gifobj->timer, ms_delay_next);
    }

    lv_image_cache_drop(lv_image_get_src(obj));
    invalidate_frame(gifobj);
}

#endif /*LV_USE_GIF*/

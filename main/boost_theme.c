#include "boost_theme.h"

#include <string.h>

/* Built for the desktop simulator too, which has no NVS. Settings there live
 * for the lifetime of the process; everything else in this file is identical,
 * so the sim exercises the same defaults and the same accessors. */
#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"
#endif

#define NVS_NS          "boost"
#define NVS_KEY_COLORS  "theme_colors"
#define NVS_KEY_BIGFLAT "bigdigit_flat"
#define NVS_KEY_BIGTEXT "bigdigit_text"
#define NVS_KEY_PXSHIFT "pixel_shift"
/* 11 chars; NVS keys cap at 15. */
#define NVS_KEY_PXSECS  "pxshift_sec"
#define NVS_KEY_BIGCOL  "bigdigit_col"
#define NVS_KEY_BIGTXC  "bigdigit_txc"
#define NVS_KEY_ARCGRAD "arc_gradient"
#define NVS_KEY_HUDGRAD "hud_gradient"
#define NVS_KEY_TESYNC  "te_sync"
#define NVS_KEY_ROT     "rotation"
#define NVS_KEY_VFACE   "vault_face"
#define NVS_KEY_VVIG    "vault_vig"
#define NVS_KEY_DEMO    "demo_mode"

/* Palettes/styles here MUST match tools/mock_server.py and the web renderers so
 * the physical panel and the dashboard mirror agree. */
static const boost_theme_t s_defaults[] = {
    {
        .id = "dyno-cell",
        .name = "Dyno Cell",
        .style = BOOST_STYLE_ARC,
        .face = 0x090A0D,
        .track = 0x20242C,
        .text = 0xF5F7FA,
        .muted = 0x8C95A3,
        .vacuum = 0x4DD2FF,
        .boost = 0xB8F35A,
        .overboost = 0xFF4F6D,
        .zero = 0xFFFFFF,
    },
    {
        .id = "vault-tec",
        .name = "Vault-Tec",
        .style = BOOST_STYLE_VAULT,
        .face = 0x05281A,
        .track = 0x0C3D24,
        .text = 0x38F08A,
        .muted = 0x1F7A4D,
        .vacuum = 0x38F08A,
        .boost = 0x38F08A,
        .overboost = 0xEAFC50,
        .zero = 0x38F08A,
    },
    {
        .id = "night-city",
        .name = "Night City",
        .style = BOOST_STYLE_HUD,
        .face = 0x080A08,
        .track = 0x1A1C0A,
        .text = 0xFCEE0A,
        .muted = 0x5A7A0A,
        .vacuum = 0x00E5FF,
        .boost = 0xFCEE0A,
        .overboost = 0xFF003C,
        .zero = 0x00E5FF,
    },
    {
        .id = "big-digit",
        .name = "Big Digit",
        .style = BOOST_STYLE_BIGDIGIT,
        .face = 0x0B0C0E,
        .track = 0x20242C,
        .text = 0xFFFFFF,
        .muted = 0x0B0C0E,
        .vacuum = 0x4DD2FF,
        .boost = 0xB8F35A,
        .overboost = 0xFF4F6D,
        .zero = 0xFFFFFF,
    },
};

#define THEME_COUNT (sizeof(s_defaults) / sizeof(s_defaults[0]))

/* Working copy: identical to s_defaults until overrides are loaded or set. */
static boost_theme_t s_themes[THEME_COUNT];
static bool s_loaded;
static bool s_bigdigit_static_bg;
static bool s_bigdigit_color_text;
/* 0x1000000 sentinel = "unset", so an absent NVS key keeps the black
 * default while a stored pure-black (0x000000) is still honoured. */
static uint32_t s_bigdigit_static_color = 0x000000u;
static uint32_t s_bigdigit_text_color = 0xFFFFFFu;
static bool s_arc_gradient;
static bool s_hud_gradient;
static bool s_te_sync;
/* Panel rotation in degrees. The LVGL adapter accepts only quarter turns and
 * takes the value at registration time, so this is applied at boot. */
static uint16_t s_rotation;
/* Vault dial glow. 0x1000000 sentinel = "unset" so an absent key keeps the
 * default while a stored value (including a dark one) is honoured. */
static uint32_t s_vault_face = 0x05281Au;
static uint8_t s_vault_vig_pct = 60u;
/* Burn-in protection is on unless it was explicitly switched off, so a panel
 * that never sees the settings page is still protected. */
static bool s_pixel_shift = true;
static uint16_t s_pixel_shift_sec = BOOST_PXSHIFT_SEC_DEFAULT;
/* Real sensors by default: an absent NVS key must not silently put a fresh
 * panel into the synthetic sweep. */
static bool s_demo_mode = false;

/* Persisted as one blob keyed by id rather than per-theme NVS keys: ids run to
 * 24 chars and NVS keys cap at 15, and a single blob keeps the whole set
 * consistent across a power loss. */
typedef struct {
    char id[BOOST_THEME_ID_MAX];
    uint32_t vacuum;
    uint32_t boost;
    uint32_t overboost;
} theme_override_t;

static void ensure_loaded(void)
{
    if (s_loaded) {
        return;
    }
    memcpy(s_themes, s_defaults, sizeof(s_themes));
    s_loaded = true;
}

static uint16_t clamp_pxshift_sec(uint16_t seconds)
{
    if (seconds < BOOST_PXSHIFT_SEC_MIN) {
        return (uint16_t)BOOST_PXSHIFT_SEC_MIN;
    }
    if (seconds > BOOST_PXSHIFT_SEC_MAX) {
        return (uint16_t)BOOST_PXSHIFT_SEC_MAX;
    }
    return seconds;
}

static void persist(void)
{
#ifdef ESP_PLATFORM
    theme_override_t saved[THEME_COUNT];
    for (size_t i = 0; i < THEME_COUNT; ++i) {
        memcpy(saved[i].id, s_themes[i].id, sizeof(saved[i].id));
        saved[i].vacuum = s_themes[i].vacuum;
        saved[i].boost = s_themes[i].boost;
        saved[i].overboost = s_themes[i].overboost;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, NVS_KEY_COLORS, saved, sizeof(saved));
    nvs_set_u8(h, NVS_KEY_BIGFLAT, s_bigdigit_static_bg ? 1 : 0);
    nvs_set_u8(h, NVS_KEY_BIGTEXT, s_bigdigit_color_text ? 1 : 0);
    nvs_set_u8(h, NVS_KEY_PXSHIFT, s_pixel_shift ? 1 : 0);
    nvs_set_u16(h, NVS_KEY_PXSECS, s_pixel_shift_sec);
    nvs_set_u32(h, NVS_KEY_BIGCOL, s_bigdigit_static_color);
    nvs_set_u32(h, NVS_KEY_BIGTXC, s_bigdigit_text_color);
    nvs_set_u8(h, NVS_KEY_ARCGRAD, s_arc_gradient ? 1 : 0);
    nvs_set_u8(h, NVS_KEY_HUDGRAD, s_hud_gradient ? 1 : 0);
    nvs_set_u8(h, NVS_KEY_TESYNC, s_te_sync ? 1 : 0);
    nvs_set_u16(h, NVS_KEY_ROT, s_rotation);
    nvs_set_u32(h, NVS_KEY_VFACE, s_vault_face);
    nvs_set_u8(h, NVS_KEY_VVIG, s_vault_vig_pct);
    nvs_set_u8(h, NVS_KEY_DEMO, s_demo_mode ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
#endif
}

void boost_theme_init(void)
{
    ensure_loaded();

#ifdef ESP_PLATFORM
    /* Mount NVS here rather than relying on being called after
     * boost_model_init(). Getting that order wrong is silent: nvs_open() just
     * fails, the read is skipped, and overrides appear to save correctly right
     * up until the next reboot drops them. nvs_flash_init() is idempotent. */
    esp_err_t nerr = nvs_flash_init();
    if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (nvs_flash_erase() == ESP_OK) {
            nerr = nvs_flash_init();
        }
    }
    if (nerr != ESP_OK) {
        return;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }

    uint8_t flat = 0;
    if (nvs_get_u8(h, NVS_KEY_BIGFLAT, &flat) == ESP_OK) {
        s_bigdigit_static_bg = (flat != 0);
    }
    uint8_t ctext = 0;
    if (nvs_get_u8(h, NVS_KEY_BIGTEXT, &ctext) == ESP_OK) {
        s_bigdigit_color_text = (ctext != 0);
    }

    /* Absent key keeps the default (on): an existing panel that predates this
     * setting gets burn-in protection on its next boot without being asked. */
    uint8_t px = 0;
    if (nvs_get_u8(h, NVS_KEY_PXSHIFT, &px) == ESP_OK) {
        s_pixel_shift = (px != 0);
    }

    /* Clamped on the way in as well as the way out: the range can narrow in a
     * later firmware, and a stored value from a wider one must not survive as
     * an out-of-range period that no code path would ever have accepted. */
    uint16_t pxsec = 0;
    if (nvs_get_u16(h, NVS_KEY_PXSECS, &pxsec) == ESP_OK && pxsec != 0) {
        s_pixel_shift_sec = clamp_pxshift_sec(pxsec);
    }

    uint32_t bigcol = 0;
    if (nvs_get_u32(h, NVS_KEY_BIGCOL, &bigcol) == ESP_OK) {
        s_bigdigit_static_color = bigcol & 0xFFFFFFu;
    }
    uint32_t bigtxc = 0;
    if (nvs_get_u32(h, NVS_KEY_BIGTXC, &bigtxc) == ESP_OK) {
        s_bigdigit_text_color = bigtxc & 0xFFFFFFu;
    }
    uint8_t ag = 0;
    if (nvs_get_u8(h, NVS_KEY_ARCGRAD, &ag) == ESP_OK) {
        s_arc_gradient = (ag != 0);
    }
    uint8_t hg = 0;
    if (nvs_get_u8(h, NVS_KEY_HUDGRAD, &hg) == ESP_OK) {
        s_hud_gradient = (hg != 0);
    }
    uint8_t te = 0;
    if (nvs_get_u8(h, NVS_KEY_TESYNC, &te) == ESP_OK) {
        s_te_sync = (te != 0);
    }

    uint16_t rot = 0;
    if (nvs_get_u16(h, NVS_KEY_ROT, &rot) == ESP_OK) {
        s_rotation = (rot == 90u || rot == 180u || rot == 270u) ? rot : 0u;
    }

    uint32_t vf = 0;
    if (nvs_get_u32(h, NVS_KEY_VFACE, &vf) == ESP_OK) {
        s_vault_face = vf & 0xFFFFFFu;
    }
    uint8_t vv = 0;
    if (nvs_get_u8(h, NVS_KEY_VVIG, &vv) == ESP_OK) {
        s_vault_vig_pct = vv > 90u ? 90u : vv;
    }

    /* Absent key keeps the default (off = real sensors). */
    uint8_t dm = 0;
    if (nvs_get_u8(h, NVS_KEY_DEMO, &dm) == ESP_OK) {
        s_demo_mode = (dm != 0);
    }

    size_t len = 0;
    if (nvs_get_blob(h, NVS_KEY_COLORS, NULL, &len) == ESP_OK && len > 0 &&
        (len % sizeof(theme_override_t)) == 0) {
        const size_t n = len / sizeof(theme_override_t);
        theme_override_t saved[THEME_COUNT];
        if (n <= THEME_COUNT && nvs_get_blob(h, NVS_KEY_COLORS, saved, &len) == ESP_OK) {
            /* Matched by id, not index: the theme table may gain or lose
             * entries between firmware versions and a positional restore
             * would silently paint one theme with another's colours. */
            for (size_t i = 0; i < n; ++i) {
                saved[i].id[BOOST_THEME_ID_MAX - 1] = '\0';
                for (size_t j = 0; j < THEME_COUNT; ++j) {
                    if (strcmp(s_themes[j].id, saved[i].id) == 0) {
                        s_themes[j].vacuum = saved[i].vacuum;
                        s_themes[j].boost = saved[i].boost;
                        s_themes[j].overboost = saved[i].overboost;
                        break;
                    }
                }
            }
        }
    }
    nvs_close(h);
#endif /* ESP_PLATFORM */
}

static boost_theme_t *find_mutable(const char *id)
{
    ensure_loaded();
    if (id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < THEME_COUNT; ++i) {
        if (strcmp(s_themes[i].id, id) == 0) {
            return &s_themes[i];
        }
    }
    return NULL;
}

bool boost_theme_set_colors(const char *id, const boost_theme_colors_t *colors)
{
    boost_theme_t *t = find_mutable(id);
    if (t == NULL || colors == NULL) {
        return false;
    }
    t->vacuum = colors->vacuum & 0xFFFFFFu;
    t->boost = colors->boost & 0xFFFFFFu;
    t->overboost = colors->overboost & 0xFFFFFFu;
    persist();
    return true;
}

bool boost_theme_reset_colors(const char *id)
{
    boost_theme_t *t = find_mutable(id);
    if (t == NULL) {
        return false;
    }
    for (size_t i = 0; i < THEME_COUNT; ++i) {
        if (strcmp(s_defaults[i].id, id) == 0) {
            t->vacuum = s_defaults[i].vacuum;
            t->boost = s_defaults[i].boost;
            t->overboost = s_defaults[i].overboost;
            persist();
            return true;
        }
    }
    return false;
}

bool boost_theme_is_customized(const char *id)
{
    const boost_theme_t *t = find_mutable(id);
    if (t == NULL) {
        return false;
    }
    for (size_t i = 0; i < THEME_COUNT; ++i) {
        if (strcmp(s_defaults[i].id, id) == 0) {
            return t->vacuum != s_defaults[i].vacuum ||
                   t->boost != s_defaults[i].boost ||
                   t->overboost != s_defaults[i].overboost;
        }
    }
    return false;
}

bool boost_theme_bigdigit_static_bg(void)
{
    return s_bigdigit_static_bg;
}

void boost_theme_set_bigdigit_static_bg(bool enabled)
{
    ensure_loaded();
    s_bigdigit_static_bg = enabled;
    persist();
}

bool boost_theme_bigdigit_color_text(void)
{
    return s_bigdigit_color_text;
}

void boost_theme_set_bigdigit_color_text(bool enabled)
{
    ensure_loaded();
    s_bigdigit_color_text = enabled;
    persist();
}

uint32_t boost_theme_bigdigit_static_color(void)
{
    return s_bigdigit_static_color;
}

void boost_theme_set_bigdigit_static_color(uint32_t rgb)
{
    ensure_loaded();
    s_bigdigit_static_color = rgb & 0xFFFFFFu;
    persist();
}

uint32_t boost_theme_bigdigit_text_color(void)
{
    return s_bigdigit_text_color;
}

void boost_theme_set_bigdigit_text_color(uint32_t rgb)
{
    ensure_loaded();
    s_bigdigit_text_color = rgb & 0xFFFFFFu;
    persist();
}

bool boost_theme_arc_gradient(void)
{
    return s_arc_gradient;
}

void boost_theme_set_arc_gradient(bool enabled)
{
    ensure_loaded();
    s_arc_gradient = enabled;
    persist();
}

bool boost_theme_hud_gradient(void)
{
    return s_hud_gradient;
}

void boost_theme_set_hud_gradient(bool enabled)
{
    ensure_loaded();
    s_hud_gradient = enabled;
    persist();
}

uint16_t boost_theme_rotation(void)
{
    return s_rotation;
}

void boost_theme_set_rotation(uint16_t degrees)
{
    ensure_loaded();
    /* Only quarter turns exist: the panel bridge maps rotation onto the CO5300
     * scan order, and anything else would need a full-frame affine transform
     * per render on a CPU-rasterised partial pipeline. Reject rather than
     * silently snap, so the API can report the bad value. */
    if (degrees != 0u && degrees != 90u && degrees != 180u && degrees != 270u) {
        return;
    }
    s_rotation = degrees;
    persist();
}

bool boost_theme_te_sync(void)
{
    return s_te_sync;
}

void boost_theme_set_te_sync(bool enabled)
{
    ensure_loaded();
    s_te_sync = enabled;
    persist();
}

bool boost_theme_demo_mode(void)
{
    return s_demo_mode;
}

void boost_theme_set_demo_mode(bool enabled)
{
    ensure_loaded();
    s_demo_mode = enabled;
    persist();
}

uint32_t boost_theme_vault_face(void)
{
    return s_vault_face;
}

void boost_theme_set_vault_face(uint32_t rgb)
{
    ensure_loaded();
    s_vault_face = rgb & 0xFFFFFFu;
    persist();
}

uint8_t boost_theme_vault_vignette_pct(void)
{
    return s_vault_vig_pct;
}

void boost_theme_set_vault_vignette_pct(uint8_t pct)
{
    ensure_loaded();
    s_vault_vig_pct = pct > 90u ? 90u : pct;
    persist();
}

bool boost_theme_pixel_shift(void)
{
    return s_pixel_shift;
}

void boost_theme_set_pixel_shift(bool enabled)
{
    ensure_loaded();
    s_pixel_shift = enabled;
    persist();
}

uint16_t boost_theme_pixel_shift_sec(void)
{
    return s_pixel_shift_sec;
}

void boost_theme_set_pixel_shift_sec(uint16_t seconds)
{
    ensure_loaded();
    /* Clamped, not rejected: this is a comfort setting with no wrong answer
     * inside a wide band, and the caller that asked for 5 s wants "as often as
     * you'll allow", not an error. The on/off decision stays with
     * boost_theme_set_pixel_shift() so there is exactly one way to disable
     * this and no way for the two controls to disagree. */
    s_pixel_shift_sec = clamp_pxshift_sec(seconds);
    persist();
}

const char *boost_style_name(boost_gauge_style_t style)
{
    switch (style) {
        case BOOST_STYLE_VAULT:
            return "vault";
        case BOOST_STYLE_HUD:
            return "hud";
        case BOOST_STYLE_BIGDIGIT:
            return "bigdigit";
        case BOOST_STYLE_ARC:
        default:
            return "arc";
    }
}

const boost_theme_t *boost_theme_default(void)
{
    ensure_loaded();
    return &s_themes[0];
}

const boost_theme_t *boost_theme_find(const char *id)
{
    if (id == NULL || id[0] == '\0') {
        return boost_theme_default();
    }
    for (size_t i = 0; i < boost_theme_count(); ++i) {
        if (strcmp(s_themes[i].id, id) == 0) {
            return &s_themes[i];
        }
    }
    return NULL;
}

const boost_theme_t *boost_theme_at(size_t index)
{
    ensure_loaded();
    if (index >= boost_theme_count()) {
        return NULL;
    }
    return &s_themes[index];
}

size_t boost_theme_count(void)
{
    return THEME_COUNT;
}

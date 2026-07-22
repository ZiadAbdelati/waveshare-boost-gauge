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
#define NVS_KEY_PXSHIFT "pixel_shift"

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
        .brightness_high = 92,
        .brightness_low = 18,
    },
    {
        .id = "vault-tec",
        .name = "Vault-Tec",
        .style = BOOST_STYLE_VAULT,
        .face = 0x02100A,
        .track = 0x0C3D24,
        .text = 0x38F08A,
        .muted = 0x1F7A4D,
        .vacuum = 0x38F08A,
        .boost = 0x38F08A,
        .overboost = 0xEAFC50,
        .zero = 0x38F08A,
        .brightness_high = 85,
        .brightness_low = 12,
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
        .brightness_high = 90,
        .brightness_low = 14,
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
        .brightness_high = 90,
        .brightness_low = 16,
    },
};

#define THEME_COUNT (sizeof(s_defaults) / sizeof(s_defaults[0]))

/* Working copy: identical to s_defaults until overrides are loaded or set. */
static boost_theme_t s_themes[THEME_COUNT];
static bool s_loaded;
static bool s_bigdigit_static_bg;
/* Burn-in protection is on unless it was explicitly switched off, so a panel
 * that never sees the settings page is still protected. */
static bool s_pixel_shift = true;

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
    nvs_set_u8(h, NVS_KEY_PXSHIFT, s_pixel_shift ? 1 : 0);
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

    /* Absent key keeps the default (on): an existing panel that predates this
     * setting gets burn-in protection on its next boot without being asked. */
    uint8_t px = 0;
    if (nvs_get_u8(h, NVS_KEY_PXSHIFT, &px) == ESP_OK) {
        s_pixel_shift = (px != 0);
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

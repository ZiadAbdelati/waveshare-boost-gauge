#include "boost_theme.h"

#include <string.h>

/* Palettes/styles here MUST match tools/mock_server.py and the web renderers so
 * the physical panel and the dashboard mirror agree. */
static const boost_theme_t s_themes[] = {
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
    if (index >= boost_theme_count()) {
        return NULL;
    }
    return &s_themes[index];
}

size_t boost_theme_count(void)
{
    return sizeof(s_themes) / sizeof(s_themes[0]);
}

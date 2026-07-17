#include "boost_theme.h"

#include <string.h>

static const boost_theme_t s_themes[] = {
    {
        .id = "pit-lane",
        .name = "Pit Lane",
        .face = 0x000000,
        .track = 0x3A3F4A,
        .text = 0xE8ECF2,
        .muted = 0x6B7280,
        .vacuum = 0x2EE6C5,
        .boost = 0xFFB020,
        .overboost = 0xFF3B30,
        .zero = 0xE8ECF2,
        .brightness_high = 100,
        .brightness_low = 12,
    },
    {
        .id = "dyno-cell",
        .name = "Dyno Cell",
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
        .id = "night-run",
        .name = "Night Run",
        .face = 0x050608,
        .track = 0x24201E,
        .text = 0xECE7DF,
        .muted = 0x8E8174,
        .vacuum = 0x31D7A9,
        .boost = 0xFFCA3A,
        .overboost = 0xFF5A36,
        .zero = 0xECE7DF,
        .brightness_high = 85,
        .brightness_low = 10,
    },
};

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

#ifndef BOOST_GENERATED_WEB_ASSETS_H
#define BOOST_GENERATED_WEB_ASSETS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *path;
    const char *content_type;
    const uint8_t *gzip_data;
    size_t gzip_size;
    const char *etag;
} boost_web_asset_t;

const boost_web_asset_t *boost_web_asset_find(const char *path);

#endif

#pragma once
/*
 * Heap capability shim. The driver's boot RAM guard only needs a large
 * "largest free block", so the fake answers with a healthy figure.
 */
#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_8BIT      (1u << 2)
#define MALLOC_CAP_DMA       (1u << 3)
#define MALLOC_CAP_IRAM_8BIT (1u << 4)
#define MALLOC_CAP_SPIRAM    (1u << 10)
#define MALLOC_CAP_INTERNAL  (1u << 11)
#define MALLOC_CAP_DEFAULT   (1u << 12)

size_t heap_caps_get_largest_free_block(uint32_t caps);
size_t heap_caps_get_free_size(uint32_t caps);

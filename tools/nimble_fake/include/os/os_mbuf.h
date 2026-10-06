#pragma once
/* Minimal os_mbuf shim: OS_MBUF_PKTLEN()/os_mbuf_copydata() over a flat buffer. */
#include <stdint.h>

struct os_mbuf {
    uint16_t omp_len;
    uint8_t  omp_data[512];
};

#define OS_MBUF_PKTLEN(__om) ((__om)->omp_len)

int os_mbuf_copydata(const struct os_mbuf *m, int off, int len, void *dst);

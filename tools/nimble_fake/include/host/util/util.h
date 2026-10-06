#pragma once
/* The driver includes host/util/util.h but calls nothing from it; the host
 * umbrella above already supplies every declaration it needs. */
#include "host/ble_hs.h"

int ble_hs_util_ensure_addr(int prefer_random);

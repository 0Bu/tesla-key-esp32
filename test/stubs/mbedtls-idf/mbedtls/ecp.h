/*
 * Host-only stand-in for ESP-IDF's public mbedtls/ecp.h wrapper
 * (components/mbedtls/port/include/mbedtls/ecp.h). Espressif's Mbed TLS fork includes it from
 * its own builtin sources; the ESP-only accelerator declarations it adds are not needed on a host.
 */
#pragma once
#include "mbedtls/private/ecp.h"

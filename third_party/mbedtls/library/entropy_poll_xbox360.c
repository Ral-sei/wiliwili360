/* Xbox 360 hardware entropy source. */

#if defined(MBEDTLS_CONFIG_FILE)
#include MBEDTLS_CONFIG_FILE
#else
#include "mbedtls/config.h"
#endif

#if defined(MBEDTLS_ENTROPY_HARDWARE_ALT)

#include <xtl.h>
#include <winsockx.h>
#include "mbedtls/entropy.h"

int mbedtls_hardware_poll(void *data, unsigned char *output,
                          size_t len, size_t *olen)
{
    (void)data;

    if (olen == NULL || (len != 0 && output == NULL))
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;

    *olen = 0;
    if (len > (size_t)UINT_MAX)
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;

    if (len != 0 && XNetRandom(output, (UINT)len) != 0)
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;

    *olen = len;
    return 0;
}

#endif /* MBEDTLS_ENTROPY_HARDWARE_ALT */

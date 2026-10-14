#pragma once
#include <stddef.h>
#include <stdint.h>

/* CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320, init/xorout 0xFFFFFFFF).
 * "123456789" -> 0xCBF43926. */
uint32_t esl_crc32(const void *data, size_t len);

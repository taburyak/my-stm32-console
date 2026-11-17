#ifndef FW_UPDATE_H
#define FW_UPDATE_H

#include <stdint.h>

#define FW_UPDATE_MAGIC      0x55465734UL /* "4WFU" little-endian */
#define FW_UPDATE_HDR_SIZE   32U
#define FW_UPDATE_APP_ADDR   0x08010000UL
#define FW_UPDATE_MAX_SIZE   (960UL * 1024UL)
#define FW_UPDATE_SD_PATH    "system/fw_update.bin"

typedef struct __attribute__((packed)) {
    uint32_t magic;       /* FW_UPDATE_MAGIC */
    uint32_t header_size; /* FW_UPDATE_HDR_SIZE (32) */
    uint32_t image_size;  /* Raw application .bin size in bytes */
    uint32_t crc32;       /* IEEE 802.3 CRC-32 of the raw application .bin */
    uint32_t target_addr; /* FW_UPDATE_APP_ADDR (0x08010000) */
    char     version[12]; /* Null-terminated version string */
} FwUpdate_Header_t;

_Static_assert(sizeof(FwUpdate_Header_t) == FW_UPDATE_HDR_SIZE, "FwUpdate_Header_t must be 32 bytes");

#endif /* FW_UPDATE_H */

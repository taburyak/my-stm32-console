#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

/* 1. MCU & OS Selection */
#define CFG_TUSB_MCU                 OPT_MCU_STM32F4
#define CFG_TUSB_OS                  OPT_OS_FREERTOS

/* 2. Enable USB Host Mode on Port 0 (USB_OTG_FS Full-Speed) */
#define CFG_TUH_ENABLED              1
#define CFG_TUH_MAX_SPEED            OPT_SPEED_FULL
#define BOARD_TUH_RHPORT             0
#define BOARD_TUH_MAX_SPEED          OPT_SPEED_FULL

/* 3. Memory alignment & section (Standard DMA-safe Internal SRAM) */
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN           __attribute__((aligned(4)))

/* 4. Host Configuration: Enumeration & Endpoints */
#define CFG_TUH_ENUMERATION_BUFSIZE  256
#define CFG_TUH_HUB                  1  /* Support USB Hubs! */
#define CFG_TUH_DEVICE_MAX           4  /* Up to 4 devices via hub or combo receiver */

/* 5. HID Host Class Configuration (Keyboard + Mouse + Gamepad) */
#define CFG_TUH_HID                  4  /* Up to 4 HID interfaces simultaneously! */
#define CFG_TUH_HID_EPIN_BUFSIZE     64
#define CFG_TUH_HID_EPOUT_BUFSIZE    64

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H_ */

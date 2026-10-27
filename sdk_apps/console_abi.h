#ifndef CONSOLE_ABI_H
#define CONSOLE_ABI_H

/*
 * Application Binary Interface shared by the OS kernel and SDK apps.
 * Data only (no code): syscall numbers, key masks, structures, limits.
 * Syscall numbers are never reused; structures only grow at the end.
 */

#include <stdint.h>

#define SDK_API_VERSION         3U

#define SDK_SCREEN_W            320U
#define SDK_SCREEN_H            240U

/* ---- App image: header at the image start, CRC-32 (IEEE) of all preceding bytes in the last word ---- */
#define SDK_APP_MAGIC           0x50504143UL /* "CAPP" */
#define SDK_APP_NAME_LEN        16U

typedef struct {
    uint32_t magic;          /* SDK_APP_MAGIC */
    uint16_t api_version;    /* SDK_API_VERSION the app was built against */
    uint16_t header_size;    /* sizeof(SDK_AppHeader_t) */
    uint32_t image_size;     /* file size in bytes, CRC word included */
    uint32_t bss_end;        /* first address after .bss */
    uint32_t entry;          /* Thumb entry address */
    char     name[SDK_APP_NAME_LEN];
} SDK_AppHeader_t;

/* ---- Syscall numbers (svc #N) ---- */
#define SDK_SVC_LED_TOGGLE      1U
#define SDK_SVC_BEEP            2U
#define SDK_SVC_GET_TICK        3U
#define SDK_SVC_GET_GAMEPAD     4U
#define SDK_SVC_LCD_FILL_RECT   5U
#define SDK_SVC_GET_KEYS        6U
/* 7 retired: never reuse */
#define SDK_SVC_EXIT            8U
#define SDK_SVC_DRAW_PIXEL      9U
#define SDK_SVC_DRAW_SPRITE     10U
#define SDK_SVC_DRAW_TEXT       11U
#define SDK_SVC_CLEAR_SCREEN    12U
#define SDK_SVC_GET_MOUSE       13U
#define SDK_SVC_PLAY_TONE       14U
#define SDK_SVC_SAVE_STATE      15U /* blocking (kernel gate) */
#define SDK_SVC_LOAD_STATE      16U /* blocking (kernel gate) */
#define SDK_SVC_GET_INFO        17U
#define SDK_SVC_SLEEP_MS        18U /* blocking (kernel gate) */
/* 19, 20 reserved */
#define SDK_SVC_RANDOM          21U
/* 22, 23 reserved */
#define SDK_SVC_INPUT_STATE     24U
#define SDK_SVC_LCD_BLIT        25U /* blocking (kernel gate) */
/* 26, 27 reserved */
#define SDK_SVC_AUDIO_PLAY      28U
#define SDK_SVC_AUDIO_STOP      29U
#define SDK_SVC_AUDIO_STATUS    30U
#define SDK_SVC_FILE_OPEN       31U /* blocking (kernel gate) */
#define SDK_SVC_FILE_READ       32U /* blocking (kernel gate) */
#define SDK_SVC_FILE_WRITE      33U /* blocking (kernel gate) */
#define SDK_SVC_FILE_SEEK       34U /* blocking (kernel gate) */
#define SDK_SVC_FILE_CLOSE      35U /* blocking (kernel gate) */
#define SDK_SVC_FILE_SIZE       36U /* blocking (kernel gate) */
#define SDK_SVC_DIR_READ        37U /* blocking (kernel gate) */
#define SDK_SVC_FILE_REMOVE     38U /* blocking (kernel gate) */
/* 39 reserved */
#define SDK_SVC_EEPROM_READ     40U /* blocking (kernel gate) */
#define SDK_SVC_EEPROM_WRITE    41U /* blocking (kernel gate) */
#define SDK_SVC_FLASH_READ      42U /* blocking (kernel gate) */
#define SDK_SVC_FLASH_ERASE     43U /* blocking (kernel gate) */
#define SDK_SVC_FLASH_WRITE     44U /* blocking (kernel gate) */
#define SDK_SVC_GPIO_GET_CAPS   45U
#define SDK_SVC_GPIO_CONFIG     46U
#define SDK_SVC_GPIO_WRITE      47U
#define SDK_SVC_GPIO_READ       48U

#define SDK_SLEEP_MAX_MS        1000U

/* ---- Result codes of int32_t syscalls (negative = error) ---- */
#define SDK_OK                  0
#define SDK_ERR_INVALID         (-1)
#define SDK_ERR_NOT_FOUND       (-2)
#define SDK_ERR_IO              (-3)
#define SDK_ERR_NO_HANDLES      (-4)
#define SDK_ERR_DENIED          (-5)
#define SDK_ERR_NO_SD           (-6)
#define SDK_ERR_EXISTS          (-7)
#define SDK_ERR_NOT_SUPPORTED   (-8)

/*
 * ---- Non-volatile storage shared by all apps (offsets are relative) ----
 * EEPROM: SDK_EEPROM_SIZE bytes; unchanged bytes are not rewritten.
 * Flash:  SDK_FLASH_SIZE bytes on the W25Q128; erase sets a SDK_FLASH_SECTOR-aligned
 *         sector to 0xFF, write can only clear bits (erase first).
 */
#define SDK_EEPROM_SIZE         128U
#define SDK_FLASH_SIZE          (1024UL * 1024UL)
#define SDK_FLASH_SECTOR        4096U

/*
 * ---- Files (SDK_SVC_FILE_*, SDK_SVC_DIR_READ) ----
 * Paths are relative, '/'-separated, components of [A-Za-z0-9_-.] and spaces, no "..".
 *   "name"      -> SD:/appdata/<app name>/name   (read/write, created on demand)
 *   "res:name"  -> SD:/games/<app name>/name     (read-only resources next to the .bin)
 * Handles are closed by the OS when the app exits.
 */
#define SDK_FILE_PATH_MAX       63U
#define SDK_FILE_MAX_OPEN       4U
#define SDK_RES_PREFIX          "res:"

#define SDK_FILE_READ           (1UL << 0)
#define SDK_FILE_WRITE          (1UL << 1)
#define SDK_FILE_CREATE         (1UL << 2) /* create if missing */
#define SDK_FILE_TRUNC          (1UL << 3) /* create or truncate to 0 */
#define SDK_FILE_APPEND         (1UL << 4) /* start at the end of the file */

#define SDK_DIR_NAME_MAX        31U

typedef struct {
    uint32_t size;
    uint8_t  is_dir;
    uint8_t  reserved[3];
    char     name[SDK_DIR_NAME_MAX + 1U];
} SDK_DirEntry_t;

/* ---- Unified key mask (SDK_SVC_GET_KEYS) ---- */
#define SDK_KEY_LEFT            (1UL << 0) /* Gamepad Left  / Keyboard Left Arrow or 'A' / Board KEY_0 */
#define SDK_KEY_RIGHT           (1UL << 1) /* Gamepad Right / Keyboard Right Arrow or 'D' / Board KEY_2 */
#define SDK_KEY_UP              (1UL << 2) /* Gamepad Up    / Keyboard Up Arrow or 'W' / Board KEY_1 */
#define SDK_KEY_DOWN            (1UL << 3) /* Gamepad Down  / Keyboard Down Arrow or 'S' */
#define SDK_KEY_A               (1UL << 4) /* Gamepad 'A'   / Keyboard Space or Enter */
#define SDK_KEY_B               (1UL << 5) /* Gamepad 'X'   / Keyboard 'X' or 'Z' */
#define SDK_KEY_X               (1UL << 6) /* Gamepad 'Y'   / Keyboard 'C' */
#define SDK_KEY_EXIT            (1UL << 7) /* Gamepad 'B' or Select / Keyboard ESC / Board KEY_3 */
#define SDK_KEY_START           (1UL << 8) /* Gamepad L2 or R2 / Keyboard 'P' (pause) */

/* ---- SDK_SVC_DRAW_TEXT parameter block ---- */
typedef struct {
    uint16_t    x;
    uint16_t    y;
    const char *str;
    uint16_t    fg_color;
    uint16_t    bg_color;
    uint8_t     scale;
} SDK_DrawTextParams_t;

/* ---- SDK_SVC_GET_GAMEPAD ---- */
#define SDK_PAD_UP              (1UL << 0)
#define SDK_PAD_DOWN            (1UL << 1)
#define SDK_PAD_LEFT            (1UL << 2)
#define SDK_PAD_RIGHT           (1UL << 3)
#define SDK_PAD_A               (1UL << 4)
#define SDK_PAD_B               (1UL << 5)
#define SDK_PAD_X               (1UL << 6)
#define SDK_PAD_Y               (1UL << 7)
#define SDK_PAD_L1              (1UL << 8)
#define SDK_PAD_R1              (1UL << 9)
#define SDK_PAD_L2              (1UL << 10)
#define SDK_PAD_R2              (1UL << 11)
#define SDK_PAD_START           (1UL << 12)
#define SDK_PAD_SELECT          (1UL << 13)

typedef struct {
    uint8_t  connected;
    int8_t   left_x;    /* -127..127 */
    int8_t   left_y;    /* -127..127 */
    uint8_t  hat;       /* 0=Up, 2=Right, 4=Down, 6=Left, 0x0F=Center */
    uint32_t buttons;   /* SDK_PAD_* */
} SDK_GamepadState_t;

/* ---- SDK_SVC_GET_MOUSE ---- */
#define SDK_MOUSE_SRC_NONE      0U
#define SDK_MOUSE_SRC_USB       1U
#define SDK_MOUSE_SRC_TOUCH     2U

#define SDK_MOUSE_BTN_LEFT      (1U << 0)
#define SDK_MOUSE_BTN_RIGHT     (1U << 1)
#define SDK_MOUSE_BTN_MIDDLE    (1U << 2)

typedef struct {
    int16_t  x;         /* 0..319 */
    int16_t  y;         /* 0..239 */
    int8_t   dx;
    int8_t   dy;
    uint8_t  buttons;   /* SDK_MOUSE_BTN_* */
    uint8_t  source;    /* SDK_MOUSE_SRC_* */
} SDK_MouseState_t;

/* ---- SDK_SVC_GET_INFO ---- */
#define SDK_DEV_KEYBOARD        (1UL << 0)
#define SDK_DEV_MOUSE           (1UL << 1)
#define SDK_DEV_GAMEPAD         (1UL << 2)
#define SDK_DEV_TOUCH           (1UL << 3)

#define SDK_INFO_SOUND_MUTED    (1UL << 0)
#define SDK_INFO_SD_MOUNTED     (1UL << 1)

typedef struct {
    uint16_t api_version;
    uint16_t screen_w;
    uint16_t screen_h;
    uint16_t reserved;
    uint32_t app_ram_size;   /* code + data + bss */
    uint32_t app_stack_size;
    uint32_t app_ext_size;
    uint32_t devices;        /* SDK_DEV_* currently connected */
    uint32_t flags;          /* SDK_INFO_* */
} SDK_SysInfo_t;

/* ---- SDK_SVC_INPUT_STATE: all input devices in one call ---- */
typedef struct {
    uint32_t           tick;     /* ms, same clock as SDK_SVC_GET_TICK */
    uint32_t           keys;     /* SDK_KEY_* */
    uint32_t           devices;  /* SDK_DEV_* */
    SDK_GamepadState_t pad;      /* pad.connected == 0 when absent */
    SDK_MouseState_t   mouse;    /* mouse.source == SDK_MOUSE_SRC_NONE when absent */
} SDK_InputState_t;

/* ---- SDK_SVC_LCD_BLIT: RGB565 rectangle from app memory, rows stride pixels apart ---- */
typedef struct {
    uint16_t        x;
    uint16_t        y;
    uint16_t        w;
    uint16_t        h;
    const uint16_t *pixels;
    uint16_t        stride;  /* pixels per source row, >= w */
    uint16_t        reserved;
} SDK_BlitParams_t;

/* ---- SDK_SVC_AUDIO_*: note sequence copied by the OS and played in the background ---- */
#define SDK_AUDIO_MAX_NOTES     64U
#define SDK_AUDIO_LOOP          (1UL << 0)

/* The buzzer has a fixed pitch: freq_hz > 0 sounds, 0 is a rest */
typedef struct {
    uint16_t freq_hz;
    uint16_t duration_ms;
} SDK_Note_t;

/* ---- SDK_SVC_GPIO_*: user pins (none exposed yet; calls return SDK_ERR_NOT_SUPPORTED) ---- */
#define SDK_GPIO_MODE_INPUT     0U
#define SDK_GPIO_MODE_OUTPUT    1U
#define SDK_GPIO_PULL_UP        (1UL << 4)
#define SDK_GPIO_PULL_DOWN      (1UL << 5)

typedef struct {
    uint32_t pin_count;
    uint32_t reserved[3];
} SDK_GpioCaps_t;

#endif /* CONSOLE_ABI_H */

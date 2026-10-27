#include "app_services.h"
#include "app_loader.h"
#include "beeper.h"
#include "usb_host_manager.h"
#include "ili9341_simple.h"
#include <string.h>

extern DMA_HandleTypeDef hdma_memtomem_dma2_stream0;

/* ---- Random: hardware RNG (48 MHz PLLQ clock, shared with USB), xorshift32 fallback ---- */

static bool     s_rng_ready = false;
static uint32_t s_xorshift  = 0U;

uint32_t AppSvc_Random(void)
{
    if (!s_rng_ready)
    {
        __HAL_RCC_RNG_CLK_ENABLE();
        RNG->CR |= RNG_CR_RNGEN;
        s_rng_ready = true;
        s_xorshift = HAL_GetTick() ^ SysTick->VAL ^ *(const uint32_t *)UID_BASE ^ 0x9E3779B9UL;
    }

    for (uint32_t tries = 0U; tries < 200U; tries++)
    {
        uint32_t sr = RNG->SR;
        if ((sr & (RNG_SR_SEIS | RNG_SR_CEIS)) != 0U)
        {
            RNG->SR = 0U;
            RNG->CR &= ~RNG_CR_RNGEN;
            RNG->CR |= RNG_CR_RNGEN;
            break;
        }
        if ((sr & RNG_SR_DRDY) != 0U)
        {
            return RNG->DR;
        }
    }

    s_xorshift ^= SysTick->VAL;
    s_xorshift ^= s_xorshift << 13;
    s_xorshift ^= s_xorshift >> 17;
    s_xorshift ^= s_xorshift << 5;
    return s_xorshift;
}

/* ---- Input snapshot ---- */

bool AppSvc_GetInputState(SDK_InputState_t *dst)
{
    if (!AppLoader_IsUserBufferValid(dst, sizeof(*dst)))
    {
        return false;
    }

    SDK_SysInfo_t info;
    AppLoader_GetSysInfo_Privileged(&info);

    dst->tick    = HAL_GetTick();
    dst->keys    = AppLoader_ReadInputMask_Privileged();
    dst->devices = info.devices;
    (void)AppLoader_GetGamepad_Privileged(&dst->pad);
    (void)AppLoader_GetMouse_Privileged(&dst->mouse);
    return true;
}

/* ---- LCD blit ---- */

int32_t AppSvc_LcdBlit(const SDK_BlitParams_t *user_params)
{
    if (!AppLoader_IsUserBufferValid(user_params, sizeof(*user_params)))
    {
        return SDK_ERR_INVALID;
    }
    const SDK_BlitParams_t p = *user_params;

    if ((p.w == 0U) || (p.h == 0U) || (p.stride < p.w) ||
        ((uint32_t)p.x + p.w > SDK_SCREEN_W) || ((uint32_t)p.y + p.h > SDK_SCREEN_H) ||
        (((uint32_t)p.pixels & 1U) != 0U))
    {
        return SDK_ERR_INVALID;
    }
    uint32_t span_px = (uint32_t)(p.h - 1U) * p.stride + p.w;
    if (!AppLoader_IsUserBufferValid(p.pixels, span_px * 2U))
    {
        return SDK_ERR_INVALID;
    }

    lcdSetWindow(p.x, p.y, p.x + p.w - 1U, p.y + p.h - 1U);
    if (p.stride == p.w)
    {
        lcdFlushFrameBufferDMA(&hdma_memtomem_dma2_stream0, (uint16_t *)p.pixels, (uint32_t)p.w * p.h);
    }
    else
    {
        const uint16_t *row = p.pixels;
        for (uint32_t r = 0U; r < p.h; r++)
        {
            for (uint32_t c = 0U; c < p.w; c++)
            {
                LCD_DataWrite(row[c]);
            }
            row += p.stride;
        }
    }
    __DSB();
    return SDK_OK;
}

/* ---- Audio sequences ---- */

_Static_assert(sizeof(SDK_Note_t) == sizeof(Beeper_Note_t), "SDK_Note_t must match Beeper_Note_t");
_Static_assert(SDK_AUDIO_MAX_NOTES == BEEPER_SEQ_MAX_NOTES, "audio sequence limits differ");

int32_t AppSvc_AudioPlay(const SDK_Note_t *notes, uint32_t count, uint32_t flags)
{
    if ((count == 0U) || (count > SDK_AUDIO_MAX_NOTES) ||
        !AppLoader_IsUserBufferValid(notes, count * sizeof(SDK_Note_t)))
    {
        return SDK_ERR_INVALID;
    }
    Beeper_PlaySequence((const Beeper_Note_t *)notes, count, (flags & SDK_AUDIO_LOOP) != 0U);
    return SDK_OK;
}

void AppSvc_AudioStop(void)
{
    Beeper_StopSequence();
}

/* ---- User GPIO: pins an app may drive. Add entries to expose them (check the schematic first) ---- */

typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
} UserGpio_t;

static const UserGpio_t s_user_gpio[] = {
    { NULL, 0U } /* placeholder: the array may not be empty */
};
#define USER_GPIO_COUNT 0U

_Static_assert(USER_GPIO_COUNT <= (sizeof(s_user_gpio) / sizeof(s_user_gpio[0])), "USER_GPIO_COUNT too large");

static int32_t AppSvc_GpioCheck(uint32_t pin)
{
    if (USER_GPIO_COUNT == 0U)
    {
        return SDK_ERR_NOT_SUPPORTED;
    }
    return (pin < USER_GPIO_COUNT) ? SDK_OK : SDK_ERR_INVALID;
}

int32_t AppSvc_GpioGetCaps(SDK_GpioCaps_t *dst)
{
    if (!AppLoader_IsUserBufferValid(dst, sizeof(*dst)))
    {
        return SDK_ERR_INVALID;
    }
    memset(dst, 0, sizeof(*dst));
    dst->pin_count = USER_GPIO_COUNT;
    return SDK_OK;
}

int32_t AppSvc_GpioConfig(uint32_t pin, uint32_t mode)
{
    int32_t r = AppSvc_GpioCheck(pin);
    if (r != SDK_OK)
    {
        return r;
    }

    GPIO_InitTypeDef init = {0};
    init.Pin   = s_user_gpio[pin].pin;
    init.Mode  = ((mode & 0x0FU) == SDK_GPIO_MODE_OUTPUT) ? GPIO_MODE_OUTPUT_PP : GPIO_MODE_INPUT;
    init.Pull  = ((mode & SDK_GPIO_PULL_UP) != 0U) ? GPIO_PULLUP :
                 (((mode & SDK_GPIO_PULL_DOWN) != 0U) ? GPIO_PULLDOWN : GPIO_NOPULL);
    init.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(s_user_gpio[pin].port, &init);
    return SDK_OK;
}

int32_t AppSvc_GpioWrite(uint32_t pin, uint32_t value)
{
    int32_t r = AppSvc_GpioCheck(pin);
    if (r != SDK_OK)
    {
        return r;
    }
    HAL_GPIO_WritePin(s_user_gpio[pin].port, s_user_gpio[pin].pin, (value != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return SDK_OK;
}

int32_t AppSvc_GpioRead(uint32_t pin)
{
    int32_t r = AppSvc_GpioCheck(pin);
    if (r != SDK_OK)
    {
        return r;
    }
    return (HAL_GPIO_ReadPin(s_user_gpio[pin].port, s_user_gpio[pin].pin) == GPIO_PIN_SET) ? 1 : 0;
}

void AppSvc_ResetHardware(void)
{
    Beeper_StopSequence();

    for (uint32_t i = 0U; i < USER_GPIO_COUNT; i++)
    {
        GPIO_InitTypeDef init = {0};
        init.Pin  = s_user_gpio[i].pin;
        init.Mode = GPIO_MODE_INPUT;
        init.Pull = GPIO_NOPULL;
        HAL_GPIO_Init(s_user_gpio[i].port, &init);
    }
}

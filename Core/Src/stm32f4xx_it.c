/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32f4xx_it.c
  * @brief   Interrupt Service Routines.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32f4xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "tusb.h"
#include "status_led.h"
#include "beeper.h"
#include "usb_host_manager.h"
#include "sram_ext.h"
#include "ili9341_simple.h"
#include "XPT2046_touch.h"
#include "app_loader.h"
#include "app_files.h"
#include "app_storage.h"
#include "app_services.h"
#include "cmsis_os2.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
/* Syscall gate state: blocking syscalls run in privileged thread mode on the sandbox task */
volatile uint32_t g_sandbox_in_gate = 0U;
volatile uint32_t g_gate_number     = 0U;
volatile uint32_t g_gate_app_lr     = 0U;
volatile uint32_t g_gate_ret_pc     = 0U;

/* Minimum free app stack for FatFs + SD driver running on the app stack */
#define SVC_GATE_MIN_STACK_BYTES  2048U
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */
extern void vPortSVCHandler(void); // Original FreeRTOS handler
void Custom_SVC_Handler_C(uint32_t *svc_args);
uint32_t Svc_GateDispatch(uint32_t a0, uint32_t a1, uint32_t a2);
void Svc_GateTrampoline(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern DMA_HandleTypeDef hdma_sdio_rx;
extern DMA_HandleTypeDef hdma_sdio_tx;
extern SD_HandleTypeDef hsd;
extern UART_HandleTypeDef huart3;
extern HCD_HandleTypeDef hhcd_USB_OTG_FS;
extern TIM_HandleTypeDef htim4;

/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex-M4 Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */
  if (AppLoader_HandleFault((uint32_t)__builtin_return_address(0)))
  {
    return;
  }
  volatile uint32_t cfsr  = SCB->CFSR;
  volatile uint32_t hfsr  = SCB->HFSR;
  volatile uint32_t mmfar = SCB->MMFAR;
  volatile uint32_t bfar  = SCB->BFAR;
  (void)cfsr; (void)hfsr; (void)mmfar; (void)bfar;
  /* USER CODE END HardFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_HardFault_IRQn 0 */
    HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
    for (volatile uint32_t i = 0; i < 1500000U; i++) { __NOP(); }
    /* USER CODE END W1_HardFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */
  if (AppLoader_HandleFault((uint32_t)__builtin_return_address(0)))
  {
    return;
  }
  volatile uint32_t cfsr  = SCB->CFSR;
  volatile uint32_t mmfar = SCB->MMFAR;
  (void)cfsr; (void)mmfar;
  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
    for (volatile uint32_t i = 0; i < 1500000U; i++) { __NOP(); }
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Pre-fetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */
  if (AppLoader_HandleFault((uint32_t)__builtin_return_address(0)))
  {
    return;
  }
  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
    for (volatile uint32_t i = 0; i < 1500000U; i++) { __NOP(); }
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */
  if (AppLoader_HandleFault((uint32_t)__builtin_return_address(0)))
  {
    return;
  }
  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
    for (volatile uint32_t i = 0; i < 1500000U; i++) { __NOP(); }
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/******************************************************************************/
/* STM32F4xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32f4xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles EXTI line[9:5] interrupts.
  */
void EXTI9_5_IRQHandler(void)
{
  /* USER CODE BEGIN EXTI9_5_IRQn 0 */
  // Clear interrupt flags for button lines KEY0..KEY3 and call HAL Callback
  HAL_GPIO_EXTI_IRQHandler(KEY_0_Pin);
  HAL_GPIO_EXTI_IRQHandler(KEY_1_Pin);
  HAL_GPIO_EXTI_IRQHandler(KEY_2_Pin);
  HAL_GPIO_EXTI_IRQHandler(KEY_3_Pin);
  /* USER CODE END EXTI9_5_IRQn 0 */
  /* USER CODE BEGIN EXTI9_5_IRQn 1 */

  /* USER CODE END EXTI9_5_IRQn 1 */
}

/**
  * @brief This function handles TIM4 global interrupt.
  */
void TIM4_IRQHandler(void)
{
  /* USER CODE BEGIN TIM4_IRQn 0 */

  /* USER CODE END TIM4_IRQn 0 */
  HAL_TIM_IRQHandler(&htim4);
  /* USER CODE BEGIN TIM4_IRQn 1 */

  /* USER CODE END TIM4_IRQn 1 */
}

/**
  * @brief This function handles USART3 global interrupt.
  */
void USART3_IRQHandler(void)
{
  /* USER CODE BEGIN USART3_IRQn 0 */

  /* USER CODE END USART3_IRQn 0 */
  HAL_UART_IRQHandler(&huart3);
  /* USER CODE BEGIN USART3_IRQn 1 */

  /* USER CODE END USART3_IRQn 1 */
}

/**
  * @brief This function handles EXTI line[15:10] interrupts.
  */
void EXTI15_10_IRQHandler(void)
{
  /* USER CODE BEGIN EXTI15_10_IRQn 0 */

  /* USER CODE END EXTI15_10_IRQn 0 */
  HAL_GPIO_EXTI_IRQHandler(T_PEN_Pin);
  /* USER CODE BEGIN EXTI15_10_IRQn 1 */

  /* USER CODE END EXTI15_10_IRQn 1 */
}

/**
  * @brief This function handles SDIO global interrupt.
  */
void SDIO_IRQHandler(void)
{
  /* USER CODE BEGIN SDIO_IRQn 0 */

  /* USER CODE END SDIO_IRQn 0 */
  HAL_SD_IRQHandler(&hsd);
  /* USER CODE BEGIN SDIO_IRQn 1 */

  /* USER CODE END SDIO_IRQn 1 */
}

/**
  * @brief This function handles DMA2 stream3 global interrupt.
  */
void DMA2_Stream3_IRQHandler(void)
{
  /* USER CODE BEGIN DMA2_Stream3_IRQn 0 */

  /* USER CODE END DMA2_Stream3_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_sdio_rx);
  /* USER CODE BEGIN DMA2_Stream3_IRQn 1 */

  /* USER CODE END DMA2_Stream3_IRQn 1 */
}

/**
  * @brief This function handles USB On The Go FS global interrupt.
  */
void OTG_FS_IRQHandler(void)
{
  /* USER CODE BEGIN OTG_FS_IRQn 0 */
  tuh_int_handler(0, true);
  return;
  /* USER CODE END OTG_FS_IRQn 0 */
  HAL_HCD_IRQHandler(&hhcd_USB_OTG_FS);
  /* USER CODE BEGIN OTG_FS_IRQn 1 */

  /* USER CODE END OTG_FS_IRQn 1 */
}

/**
  * @brief This function handles DMA2 stream6 global interrupt.
  */
void DMA2_Stream6_IRQHandler(void)
{
  /* USER CODE BEGIN DMA2_Stream6_IRQn 0 */

  /* USER CODE END DMA2_Stream6_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_sdio_tx);
  /* USER CODE BEGIN DMA2_Stream6_IRQn 1 */

  /* USER CODE END DMA2_Stream6_IRQn 1 */
}

/* USER CODE BEGIN 1 */
// SVC 0 from MSP starts FreeRTOS; every SVC from a task (PSP) goes to the custom dispatcher
__attribute__((naked)) void SVC_Handler(void)
{
    __asm volatile(
        "tst lr, #4                \n" /* Test bit 2 of EXC_RETURN to see if MSP or PSP was used */
        "ite eq                    \n"
        "mrseq r0, msp             \n" /* r0 = stack frame pointer */
        "mrsne r0, psp             \n"
        "bne 2f                    \n" /* Task (PSP) caller: never route to vPortSVCHandler */
        "ldr r1, [r0, #24]         \n" /* r1 = stacked PC */
        "ldrb r1, [r1, #-2]        \n" /* r1 = SVC number (byte from 'svc #N' instruction) */
        "cbz r1, 1f                \n" /* If SVC number == 0, jump directly to vPortSVCHandler! */
        "2:                        \n"
        "b Custom_SVC_Handler_C    \n"
        "1:                        \n"
        "b vPortSVCHandler         \n" /* Direct tail-jump with untouched MSP and LR! */
    );
}

/*
 * Blocking syscalls cannot wait in handler mode. The SVC handler redirects the exception
 * return to Svc_GateTrampoline, which runs privileged in thread mode on the app stack,
 * calls Svc_GateDispatch, drops privilege and resumes the app after its 'svc' instruction.
 * Clobbers r1-r3, r12 and flags; r0 = result; LR is restored.
 */
static void Svc_EnterGate(uint32_t *frame, uint8_t number)
{
  uint32_t free_stack = (uint32_t)frame - MM_APP_STACK_BASE;

  if ((g_sandbox_in_gate != 0U) ||
      ((uint32_t)frame < MM_APP_STACK_BASE) ||
      (free_stack < SVC_GATE_MIN_STACK_BYTES))
  {
    frame[0] = (number > SDK_SVC_SLEEP_MS) ? (uint32_t)SDK_ERR_IO : 0U;
    return;
  }

  g_gate_number     = number;
  g_gate_app_lr     = frame[5];
  g_gate_ret_pc     = frame[6] | 1U;
  g_sandbox_in_gate = 1U;

  __set_CONTROL(__get_CONTROL() & ~CONTROL_nPRIV_Msk);
  frame[6] = ((uint32_t)Svc_GateTrampoline) & ~1U;
}

__attribute__((naked)) void Svc_GateTrampoline(void)
{
    __asm volatile(
        "mov  r3, sp                          \n" /* Align SP to 8 bytes for AAPCS */
        "bic  r12, r3, #7                     \n"
        "mov  sp, r12                         \n"
        "push {r3, r4}                        \n"
        "bl   Svc_GateDispatch                \n" /* r0..r2 = app arguments, r0 = result */
        "pop  {r3, r4}                        \n"
        "mov  sp, r3                          \n"
        "movw r1, #:lower16:g_gate_app_lr     \n"
        "movt r1, #:upper16:g_gate_app_lr     \n"
        "ldr  lr, [r1]                        \n"
        "movw r1, #:lower16:g_gate_ret_pc     \n"
        "movt r1, #:upper16:g_gate_ret_pc     \n"
        "ldr  r12, [r1]                       \n"
        "movw r1, #:lower16:g_sandbox_in_gate \n" /* Clear the flag while still privileged */
        "movt r1, #:upper16:g_sandbox_in_gate \n"
        "movs r2, #0                          \n"
        "str  r2, [r1]                        \n"
        "mrs  r1, control                     \n"
        "orr  r1, r1, #1                      \n"
        "msr  control, r1                     \n"
        "isb                                  \n"
        "bx   r12                             \n"
    );
}

uint32_t Svc_GateDispatch(uint32_t a0, uint32_t a1, uint32_t a2)
{
  switch (g_gate_number)
  {
    case 15U: /* bool sys_save_state(const char *filename, const void *data, uint32_t len) */
      return AppLoader_SaveState_Privileged((const char *)a0, (const void *)a1, a2) ? 1U : 0U;

    case 16U: /* uint32_t sys_load_state(const char *filename, void *dst, uint32_t max_len) */
      return AppLoader_LoadState_Privileged((const char *)a0, (void *)a1, a2);

    case 18U: /* void sys_sleep_ms(uint32_t ms) */
      (void)osDelay((a0 > SDK_SLEEP_MAX_MS) ? SDK_SLEEP_MAX_MS : a0);
      return 0U;

    case SDK_SVC_LCD_BLIT:
      return (uint32_t)AppSvc_LcdBlit((const SDK_BlitParams_t *)a0);

    case SDK_SVC_FILE_OPEN:
      return (uint32_t)AppFiles_Open((const char *)a0, a1);

    case SDK_SVC_FILE_READ:
      return (uint32_t)AppFiles_Read(a0, (void *)a1, a2);

    case SDK_SVC_FILE_WRITE:
      return (uint32_t)AppFiles_Write(a0, (const void *)a1, a2);

    case SDK_SVC_FILE_SEEK:
      return (uint32_t)AppFiles_Seek(a0, a1);

    case SDK_SVC_FILE_CLOSE:
      return (uint32_t)AppFiles_Close(a0);

    case SDK_SVC_FILE_SIZE:
      return (uint32_t)AppFiles_Size(a0);

    case SDK_SVC_DIR_READ:
      return (uint32_t)AppFiles_DirRead((const char *)a0, a1, (SDK_DirEntry_t *)a2);

    case SDK_SVC_FILE_REMOVE:
      return (uint32_t)AppFiles_Remove((const char *)a0);

    case SDK_SVC_EEPROM_READ:
      return (uint32_t)AppStorage_EepromRead(a0, (void *)a1, a2);

    case SDK_SVC_EEPROM_WRITE:
      return (uint32_t)AppStorage_EepromWrite(a0, (const void *)a1, a2);

    case SDK_SVC_FLASH_READ:
      return (uint32_t)AppStorage_FlashRead(a0, (void *)a1, a2);

    case SDK_SVC_FLASH_ERASE:
      return (uint32_t)AppStorage_FlashErase(a0);

    case SDK_SVC_FLASH_WRITE:
      return (uint32_t)AppStorage_FlashWrite(a0, (const void *)a1, a2);

    default:
      return 0U;
  }
}

// C-handler for system calls (SVC 1..255) from Unprivileged Sandbox
void Custom_SVC_Handler_C(uint32_t *svc_args)
{
  /* Stack frame contains: r0, r1, r2, r3, r12, lr, pc (svc_args[6]), xPSR */
  uint8_t svc_number = ((uint8_t *)svc_args[6])[-2];

  /* After exit/kill/fault the app may still run until the GUI task deletes it: ignore its syscalls */
  if (!AppLoader_IsAppActive())
  {
    svc_args[0] = 0U;
    return;
  }

  switch (svc_number)
  {
    case 1U: /* SVC 1: sys_led_toggle(led_id) */
      StatusLED_Toggle((StatusLED_t)svc_args[0]);
      break;

    case 2U: /* SVC 2: sys_beep(duration_ms) */
      Beeper_Beep((uint16_t)svc_args[0]);
      break;

    case 3U: /* SVC 3: uint32_t sys_get_tick(void) -> returns in r0 */
      svc_args[0] = HAL_GetTick();
      break;

    case 4U: /* SVC 4: bool sys_get_gamepad(SDK_GamepadState_t *dst) -> returns in r0 */
      {
        SDK_GamepadState_t *dst = (SDK_GamepadState_t *)svc_args[0];
        svc_args[0] = (AppLoader_IsUserBufferValid(dst, sizeof(*dst)) &&
                       AppLoader_GetGamepad_Privileged(dst)) ? 1U : 0U;
      }
      break;

    case 5U: /* SVC 5: sys_lcd_fill_rect(packed_xy, packed_wh, color565) */
      {
        uint16_t x     = (uint16_t)(svc_args[0] >> 16);
        uint16_t y     = (uint16_t)(svc_args[0] & 0xFFFFU);
        uint16_t w     = (uint16_t)(svc_args[1] >> 16);
        uint16_t h     = (uint16_t)(svc_args[1] & 0xFFFFU);
        uint16_t color = (uint16_t)(svc_args[2] & 0xFFFFU);

        if (x < 320U && y < 240U && w > 0U && h > 0U)
        {
          if (x + w > 320U) w = 320U - x;
          if (y + h > 240U) h = 240U - y;

          lcdSetWindow(x, y, x + w - 1U, y + h - 1U);
          uint32_t total = (uint32_t)w * (uint32_t)h;
          while (total--)
          {
            LCD_DataWrite(color);
          }
          __DSB();
          __ISB();
        }
      }
      break;

    case 6U: /* SVC 6: uint32_t sys_get_keys(void) -> returns bitmask in r0 */
      svc_args[0] = AppLoader_ReadInputMask_Privileged();
      break;

    case 8U: /* SVC 8: void sys_exit(void) */
      AppLoader_RequestExit();
      break;

    case 9U: /* SVC 9: void sys_draw_pixel(x, y, color565) */
      {
        uint16_t x     = (uint16_t)svc_args[0];
        uint16_t y     = (uint16_t)svc_args[1];
        uint16_t color = (uint16_t)svc_args[2];
        if (x < 320U && y < 240U)
        {
          lcdSetWindow(x, y, x, y);
          LCD_DataWrite(color);
          __DSB();
          __ISB();
        }
      }
      break;

    case 10U: /* SVC 10: void sys_draw_sprite(packed_xy, packed_wh, const uint16_t *bitmap) */
      {
        uint16_t x = (uint16_t)(svc_args[0] >> 16);
        uint16_t y = (uint16_t)(svc_args[0] & 0xFFFFU);
        uint16_t w = (uint16_t)(svc_args[1] >> 16);
        uint16_t h = (uint16_t)(svc_args[1] & 0xFFFFU);
        const uint16_t *bmp = (const uint16_t *)svc_args[2];
        uint32_t total_px = (uint32_t)w * (uint32_t)h;

        if (x < 320U && y < 240U && w > 0U && h > 0U &&
            (x + w <= 320U) && (y + h <= 240U) &&
            AppLoader_IsUserBufferValid(bmp, total_px * 2U))
        {
          lcdSetWindow(x, y, x + w - 1U, y + h - 1U);
          while (total_px--)
          {
            LCD_DataWrite(*bmp++);
          }
          __DSB();
          __ISB();
        }
      }
      break;

    case 11U: /* SVC 11: void sys_draw_text(const App_DrawTextParams_t *params) */
      AppLoader_DrawText_Privileged((const App_DrawTextParams_t *)svc_args[0]);
      break;

    case 12U: /* SVC 12: void sys_clear_screen(uint16_t color565) */
      {
        uint16_t color = (uint16_t)(svc_args[0] & 0xFFFFU);
        lcdSetWindow(0U, 0U, 319U, 239U);
        uint32_t total = 320UL * 240UL;
        while (total--)
        {
          LCD_DataWrite(color);
        }
        __DSB();
        __ISB();
      }
      break;

    case 13U: /* SVC 13: bool sys_get_mouse(SDK_MouseState_t *dst) -> USB Mouse or XPT2046 Touch */
      {
        SDK_MouseState_t *dst = (SDK_MouseState_t *)svc_args[0];
        svc_args[0] = (AppLoader_IsUserBufferValid(dst, sizeof(*dst)) &&
                       AppLoader_GetMouse_Privileged(dst)) ? 1U : 0U;
      }
      break;

    case 14U: /* SVC 14: void sys_play_tone(uint16_t freq_hz, uint16_t duration_ms) */
      Beeper_PlayTone((uint16_t)svc_args[0], (uint16_t)svc_args[1]);
      break;

    case 15U: /* SVC 15: bool sys_save_state(const char *filename, const void *data, uint32_t len) */
    case 16U: /* SVC 16: uint32_t sys_load_state(const char *filename, void *dst, uint32_t max_len) */
    case 18U: /* SVC 18: void sys_sleep_ms(uint32_t ms) */
    case SDK_SVC_LCD_BLIT:
    case SDK_SVC_FILE_OPEN:
    case SDK_SVC_FILE_READ:
    case SDK_SVC_FILE_WRITE:
    case SDK_SVC_FILE_SEEK:
    case SDK_SVC_FILE_CLOSE:
    case SDK_SVC_FILE_SIZE:
    case SDK_SVC_DIR_READ:
    case SDK_SVC_FILE_REMOVE:
    case SDK_SVC_EEPROM_READ:
    case SDK_SVC_EEPROM_WRITE:
    case SDK_SVC_FLASH_READ:
    case SDK_SVC_FLASH_ERASE:
    case SDK_SVC_FLASH_WRITE:
      Svc_EnterGate(svc_args, svc_number);
      break;

    case 17U: /* SVC 17: bool sys_get_info(SDK_SysInfo_t *dst) */
      {
        SDK_SysInfo_t *dst = (SDK_SysInfo_t *)svc_args[0];
        if (AppLoader_IsUserBufferValid(dst, sizeof(*dst)))
        {
          AppLoader_GetSysInfo_Privileged(dst);
          svc_args[0] = 1U;
        }
        else
        {
          svc_args[0] = 0U;
        }
      }
      break;

    case SDK_SVC_RANDOM:
      svc_args[0] = AppSvc_Random();
      break;

    case SDK_SVC_INPUT_STATE:
      svc_args[0] = AppSvc_GetInputState((SDK_InputState_t *)svc_args[0]) ? 1U : 0U;
      break;

    case SDK_SVC_AUDIO_PLAY:
      svc_args[0] = (uint32_t)AppSvc_AudioPlay((const SDK_Note_t *)svc_args[0], svc_args[1], svc_args[2]);
      break;

    case SDK_SVC_AUDIO_STOP:
      AppSvc_AudioStop();
      svc_args[0] = (uint32_t)SDK_OK;
      break;

    case SDK_SVC_AUDIO_STATUS:
      svc_args[0] = Beeper_IsSequencePlaying() ? 1U : 0U;
      break;

    case SDK_SVC_GPIO_GET_CAPS:
      svc_args[0] = (uint32_t)AppSvc_GpioGetCaps((SDK_GpioCaps_t *)svc_args[0]);
      break;

    case SDK_SVC_GPIO_CONFIG:
      svc_args[0] = (uint32_t)AppSvc_GpioConfig(svc_args[0], svc_args[1]);
      break;

    case SDK_SVC_GPIO_WRITE:
      svc_args[0] = (uint32_t)AppSvc_GpioWrite(svc_args[0], svc_args[1]);
      break;

    case SDK_SVC_GPIO_READ:
      svc_args[0] = (uint32_t)AppSvc_GpioRead(svc_args[0]);
      break;

    default:
      break;
  }
}
/* USER CODE END 1 */

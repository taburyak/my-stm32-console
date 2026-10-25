/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "cmsis_os.h"
#include "fatfs.h"
#include "usb_host.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include "ili9341_simple.h"
#include "XPT2046_touch.h"
#include "AT24Cxx.h"
#include "w25qxx.h"
#include <string.h>
#include "lvgl.h"
#include "lv_port_console.h"
#include "status_led.h"
#include "buttons_manager.h"
#include "beeper.h"
#include "system_post.h"
#include "sram_ext.h"
#include "sdcard_fatfs.h"
#include "usb_host_manager.h"
#include "os_tasks.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* Memory parameters for Step 2 */
#define INTERNAL_HEAP_SIZE    (configTOTAL_HEAP_SIZE)   /* According to the CubeMX settings */
#define EXTERNAL_SRAM_BASE    (0x68000000UL)            /* Bank 3's main address */
#define EXTERNAL_SRAM_SIZE    (1024 * 1024)             /* 1 megabyte */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
CRC_HandleTypeDef hcrc;

I2C_HandleTypeDef hi2c1;

SD_HandleTypeDef hsd;
DMA_HandleTypeDef hdma_sdio_rx;
DMA_HandleTypeDef hdma_sdio_tx;

SPI_HandleTypeDef hspi1;

UART_HandleTypeDef huart3;

DMA_HandleTypeDef hdma_memtomem_dma2_stream0;
SRAM_HandleTypeDef hsram1;
SRAM_HandleTypeDef hsram3;

/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 1024 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* USER CODE BEGIN PV */
osThreadId_t sandboxTaskHandle;

const osThreadAttr_t sandboxTask_attributes = {
  .name = "sandboxTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityLow,
};

/* Define an array for the internal part of the heap */
uint8_t ucHeapInternal[INTERNAL_HEAP_SIZE];
/* Region Configuration for Heap_5 */
const HeapRegion_t xHeapRegions[] = {
    { ucHeapInternal, INTERNAL_HEAP_SIZE },                                                           // Region 1: Internal SRAM (~0x20000000)
    { (uint8_t *)(SRAM_EXT_BASE_ADDR + SRAM_HEAP_OFFSET), (SRAM_EXT_SIZE_BYTES - SRAM_HEAP_OFFSET) }, // Region 2: External SRAM (0x68000000)
    { NULL, 0 }                                                                                       // The Terminator
};

/* Framebuffer in External SRAM (Bank 3) */
uint16_t *frame_buffer = NULL; // It will be allocated via the FreeRTOS heap

extern ApplicationTypeDef Appli_state;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_CRC_Init(void);
static void MX_FSMC_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_I2C1_Init(void);
static void MX_SPI1_Init(void);
static void MX_SDIO_SD_Init(void);
void StartDefaultTask(void *argument);

/* USER CODE BEGIN PFP */
void SandboxTask(void *argument);
void MPU_Config_UserSandbox(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* Redirecting `printf` for GCC and Picolibc (CMake toolchain) */
int __io_putchar(int ch)
{
    HAL_UART_Transmit(&huart3, (uint8_t *)&ch, 1, HAL_MAX_DELAY);
    return ch;
}

int _write(int file, char *ptr, int len)
{
    HAL_UART_Transmit(&huart3, (uint8_t*) ptr, len, HAL_MAX_DELAY);
    return len;
}

static lv_obj_t * g_usb_status_label  = NULL;
static lv_obj_t * g_file_list_obj     = NULL;
static uint32_t   g_status_hold_until = 0;

/* Static buffers in .bss to keep LVGL callback stack usage near zero! */
static char s_clicked_item[STORAGE_MAX_NAME_LEN];
static char s_next_path[STORAGE_MAX_PATH_LEN];
static char s_title_buf[STORAGE_MAX_PATH_LEN + 16U];

static void list_btn_event_cb(lv_event_t * e)
{
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t * obj = lv_event_get_target(e);

  if (code == LV_EVENT_CLICKED && g_file_list_obj != NULL && obj != NULL)
  {
    const char * raw_name = lv_list_get_btn_text(g_file_list_obj, obj);
    if (raw_name == NULL) return;

    /* Copy button text into static buffer (zero stack pressure) */
    strncpy(s_clicked_item, raw_name, sizeof(s_clicked_item) - 1U);
    s_clicked_item[sizeof(s_clicked_item) - 1U] = '\0';

    Beeper_Click();
    g_status_hold_until = HAL_GetTick() + 2000U;

    /* Check if user clicked ".. (Root)" to go back */
    if (strcmp(s_clicked_item, ".. (Root)") == 0)
    {
      if (g_usb_status_label != NULL)
      {
        lv_label_set_text(g_usb_status_label, "Opening: / ...");
      }
      OSTasks_RequestDirList("/");
      return;
    }

    /* Check if clicked item is a directory in cache */
    for (uint32_t i = 0; i < g_dir_cache.count; i++)
    {
      if (strcmp(g_dir_cache.items[i].name, s_clicked_item) == 0)
      {
        if (g_dir_cache.items[i].is_dir)
        {
          if (strcmp(g_dir_cache.path, "/") == 0)
          {
            snprintf(s_next_path, sizeof(s_next_path), "/%s", s_clicked_item);
          }
          else
          {
            snprintf(s_next_path, sizeof(s_next_path), "%.60s/%.50s", g_dir_cache.path, s_clicked_item);
          }

          if (g_usb_status_label != NULL)
          {
            lv_label_set_text_fmt(g_usb_status_label, "Dir: %s", s_clicked_item);
          }
          OSTasks_RequestDirList(s_next_path);
          return;
        }
        break;
      }
    }

    /* It is a regular file */
    if (g_usb_status_label != NULL)
    {
      lv_label_set_text_fmt(g_usb_status_label, "File: %s", s_clicked_item);
    }
  }
}

/**
 * @brief Creates the LVGL file list container and bottom HUD status label
 */
static void Create_File_Explorer_UI(lv_obj_t *parent)
{
  g_file_list_obj = lv_list_create(parent);
  lv_obj_set_size(g_file_list_obj, 300, 190);
  lv_obj_align(g_file_list_obj, LV_ALIGN_TOP_MID, 0, 8);

  g_usb_status_label = lv_label_create(parent);
  lv_obj_align(g_usb_status_label, LV_ALIGN_BOTTOM_LEFT, 10, -6);
  lv_label_set_text(g_usb_status_label, "USB: Waiting for device...");

  lv_list_add_text(g_file_list_obj, "Loading SD card...");
}

/**
 * @brief Populates the LVGL list widget from the async g_dir_cache buffer
 */
static void Populate_File_List_From_Cache(void)
{
  if (g_file_list_obj == NULL) return;

  lv_group_t * grp = lv_group_get_default();
  if (grp != NULL)
  {
    lv_group_remove_all_objs(grp);
  }

  lv_obj_clean(g_file_list_obj);

  snprintf(s_title_buf, sizeof(s_title_buf), "Folder: %s", g_dir_cache.path);
  lv_list_add_text(g_file_list_obj, s_title_buf);

  if (!g_dir_cache.io_success)
  {
    lv_list_add_text(g_file_list_obj, "Failed to open directory");
    return;
  }

  /* Add ".. (Root)" navigation button if inside a subfolder */
  if (strcmp(g_dir_cache.path, "/") != 0)
  {
    lv_obj_t * up_btn = lv_list_add_btn(g_file_list_obj, LV_SYMBOL_UP, ".. (Root)");
    lv_obj_add_event_cb(up_btn, list_btn_event_cb, LV_EVENT_CLICKED, NULL);
    if (grp != NULL) lv_group_add_obj(grp, up_btn);
  }

  for (uint32_t i = 0; i < g_dir_cache.count; i++)
  {
    const char *icon = g_dir_cache.items[i].is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE;
    lv_obj_t * btn = lv_list_add_btn(g_file_list_obj, icon, g_dir_cache.items[i].name);
    lv_obj_add_event_cb(btn, list_btn_event_cb, LV_EVENT_CLICKED, NULL);

    if (grp != NULL)
    {
      lv_group_add_obj(grp, btn);
    }
  }

  if (g_dir_cache.count == 0)
  {
    lv_list_add_text(g_file_list_obj, "(Empty directory)");
  }
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_CRC_Init();
  MX_FSMC_Init();
  MX_USART3_UART_Init();
  MX_I2C1_Init();
  MX_SPI1_Init();
  MX_SDIO_SD_Init();
  MX_FATFS_Init();
  /* USER CODE BEGIN 2 */

  // IMPORTANT: Configure and enable the MPU before the OS starts
  MPU_Config_UserSandbox();
  printf("MPU Sandbox Initialized.\r\n");

  // IMPORTANT: Let's initialize Heap_5 manually with our regions (Internal + External memory)
  vPortDefineHeapRegions(xHeapRegions);
  printf("Heap_5 Initialized Manually.\r\n");
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  // Let's create our test sandbox task
  sandboxTaskHandle = osThreadNew(SandboxTask, NULL, &sandboxTask_attributes);

  // Set tag 1 so that the FreeRTOS hook switches this task to Unprivileged mode!
  vTaskSetApplicationTaskTag((TaskHandle_t)sandboxTaskHandle, (void*) 1);
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief CRC Initialization Function
  * @param None
  * @retval None
  */
static void MX_CRC_Init(void)
{

  /* USER CODE BEGIN CRC_Init 0 */

  /* USER CODE END CRC_Init 0 */

  /* USER CODE BEGIN CRC_Init 1 */

  /* USER CODE END CRC_Init 1 */
  hcrc.Instance = CRC;
  if (HAL_CRC_Init(&hcrc) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN CRC_Init 2 */

  /* USER CODE END CRC_Init 2 */

}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 100000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief SDIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_SDIO_SD_Init(void)
{

  /* USER CODE BEGIN SDIO_Init 0 */

  /* USER CODE END SDIO_Init 0 */

  /* USER CODE BEGIN SDIO_Init 1 */

  /* USER CODE END SDIO_Init 1 */
  hsd.Instance = SDIO;
  hsd.Init.ClockEdge = SDIO_CLOCK_EDGE_RISING;
  hsd.Init.ClockBypass = SDIO_CLOCK_BYPASS_DISABLE;
  hsd.Init.ClockPowerSave = SDIO_CLOCK_POWER_SAVE_DISABLE;
  hsd.Init.BusWide = SDIO_BUS_WIDE_1B;
  hsd.Init.HardwareFlowControl = SDIO_HARDWARE_FLOW_CONTROL_DISABLE;
  hsd.Init.ClockDiv = 4;
  /* USER CODE BEGIN SDIO_Init 2 */

  /* USER CODE END SDIO_Init 2 */

}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 115200;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

/**
  * Enable DMA controller clock
  * Configure DMA for memory to memory transfers
  *   hdma_memtomem_dma2_stream0
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA2_CLK_ENABLE();

  /* Configure DMA request hdma_memtomem_dma2_stream0 on DMA2_Stream0 */
  hdma_memtomem_dma2_stream0.Instance = DMA2_Stream0;
  hdma_memtomem_dma2_stream0.Init.Channel = DMA_CHANNEL_0;
  hdma_memtomem_dma2_stream0.Init.Direction = DMA_MEMORY_TO_MEMORY;
  hdma_memtomem_dma2_stream0.Init.PeriphInc = DMA_PINC_ENABLE;
  hdma_memtomem_dma2_stream0.Init.MemInc = DMA_MINC_DISABLE;
  hdma_memtomem_dma2_stream0.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
  hdma_memtomem_dma2_stream0.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
  hdma_memtomem_dma2_stream0.Init.Mode = DMA_NORMAL;
  hdma_memtomem_dma2_stream0.Init.Priority = DMA_PRIORITY_VERY_HIGH;
  hdma_memtomem_dma2_stream0.Init.FIFOMode = DMA_FIFOMODE_ENABLE;
  hdma_memtomem_dma2_stream0.Init.FIFOThreshold = DMA_FIFO_THRESHOLD_FULL;
  hdma_memtomem_dma2_stream0.Init.MemBurst = DMA_MBURST_SINGLE;
  hdma_memtomem_dma2_stream0.Init.PeriphBurst = DMA_PBURST_SINGLE;
  if (HAL_DMA_Init(&hdma_memtomem_dma2_stream0) != HAL_OK)
  {
    Error_Handler( );
  }

  /* DMA interrupt init */
  /* DMA2_Stream3_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);
  /* DMA2_Stream6_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream6_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream6_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOE, LD0_Pin|LD1_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LCD_BL_GPIO_Port, LCD_BL_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(T_CLK_GPIO_Port, T_CLK_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, T_CS_Pin|T_MOSI_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(BEEPER_GPIO_Port, BEEPER_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOG, W25_CS_Pin|LD2_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(DS18B20_GPIO_Port, DS18B20_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : LD0_Pin LD1_Pin */
  GPIO_InitStruct.Pin = LD0_Pin|LD1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /*Configure GPIO pins : KEY_0_Pin KEY_1_Pin KEY_2_Pin KEY_3_Pin */
  GPIO_InitStruct.Pin = KEY_0_Pin|KEY_1_Pin|KEY_2_Pin|KEY_3_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOF, &GPIO_InitStruct);

  /*Configure GPIO pin : LCD_BL_Pin */
  GPIO_InitStruct.Pin = LCD_BL_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LCD_BL_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : T_CLK_Pin DS18B20_Pin */
  GPIO_InitStruct.Pin = T_CLK_Pin|DS18B20_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : T_CS_Pin T_MOSI_Pin */
  GPIO_InitStruct.Pin = T_CS_Pin|T_MOSI_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : T_MISO_Pin */
  GPIO_InitStruct.Pin = T_MISO_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(T_MISO_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : T_PEN_Pin */
  GPIO_InitStruct.Pin = T_PEN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(T_PEN_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : BEEPER_Pin */
  GPIO_InitStruct.Pin = BEEPER_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(BEEPER_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : W25_CS_Pin LD2_Pin */
  GPIO_InitStruct.Pin = W25_CS_Pin|LD2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI9_5_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);

  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* FSMC initialization function */
static void MX_FSMC_Init(void)
{

  /* USER CODE BEGIN FSMC_Init 0 */

  /* USER CODE END FSMC_Init 0 */

  FSMC_NORSRAM_TimingTypeDef Timing = {0};

  /* USER CODE BEGIN FSMC_Init 1 */

  /* USER CODE END FSMC_Init 1 */

  /** Perform the SRAM1 memory initialization sequence
  */
  hsram1.Instance = FSMC_NORSRAM_DEVICE;
  hsram1.Extended = FSMC_NORSRAM_EXTENDED_DEVICE;
  /* hsram1.Init */
  hsram1.Init.NSBank = FSMC_NORSRAM_BANK4;
  hsram1.Init.DataAddressMux = FSMC_DATA_ADDRESS_MUX_DISABLE;
  hsram1.Init.MemoryType = FSMC_MEMORY_TYPE_SRAM;
  hsram1.Init.MemoryDataWidth = FSMC_NORSRAM_MEM_BUS_WIDTH_16;
  hsram1.Init.BurstAccessMode = FSMC_BURST_ACCESS_MODE_DISABLE;
  hsram1.Init.WaitSignalPolarity = FSMC_WAIT_SIGNAL_POLARITY_LOW;
  hsram1.Init.WrapMode = FSMC_WRAP_MODE_DISABLE;
  hsram1.Init.WaitSignalActive = FSMC_WAIT_TIMING_BEFORE_WS;
  hsram1.Init.WriteOperation = FSMC_WRITE_OPERATION_ENABLE;
  hsram1.Init.WaitSignal = FSMC_WAIT_SIGNAL_DISABLE;
  hsram1.Init.ExtendedMode = FSMC_EXTENDED_MODE_DISABLE;
  hsram1.Init.AsynchronousWait = FSMC_ASYNCHRONOUS_WAIT_DISABLE;
  hsram1.Init.WriteBurst = FSMC_WRITE_BURST_DISABLE;
  hsram1.Init.PageSize = FSMC_PAGE_SIZE_NONE;
  /* Timing */
  Timing.AddressSetupTime = 2;
  Timing.AddressHoldTime = 15;
  Timing.DataSetupTime = 5;
  Timing.BusTurnAroundDuration = 0;
  Timing.CLKDivision = 16;
  Timing.DataLatency = 17;
  Timing.AccessMode = FSMC_ACCESS_MODE_A;
  /* ExtTiming */

  if (HAL_SRAM_Init(&hsram1, &Timing, NULL) != HAL_OK)
  {
    Error_Handler( );
  }

  /** Perform the SRAM3 memory initialization sequence
  */
  hsram3.Instance = FSMC_NORSRAM_DEVICE;
  hsram3.Extended = FSMC_NORSRAM_EXTENDED_DEVICE;
  /* hsram3.Init */
  hsram3.Init.NSBank = FSMC_NORSRAM_BANK3;
  hsram3.Init.DataAddressMux = FSMC_DATA_ADDRESS_MUX_DISABLE;
  hsram3.Init.MemoryType = FSMC_MEMORY_TYPE_SRAM;
  hsram3.Init.MemoryDataWidth = FSMC_NORSRAM_MEM_BUS_WIDTH_16;
  hsram3.Init.BurstAccessMode = FSMC_BURST_ACCESS_MODE_DISABLE;
  hsram3.Init.WaitSignalPolarity = FSMC_WAIT_SIGNAL_POLARITY_LOW;
  hsram3.Init.WrapMode = FSMC_WRAP_MODE_DISABLE;
  hsram3.Init.WaitSignalActive = FSMC_WAIT_TIMING_BEFORE_WS;
  hsram3.Init.WriteOperation = FSMC_WRITE_OPERATION_ENABLE;
  hsram3.Init.WaitSignal = FSMC_WAIT_SIGNAL_DISABLE;
  hsram3.Init.ExtendedMode = FSMC_EXTENDED_MODE_DISABLE;
  hsram3.Init.AsynchronousWait = FSMC_ASYNCHRONOUS_WAIT_DISABLE;
  hsram3.Init.WriteBurst = FSMC_WRITE_BURST_DISABLE;
  hsram3.Init.PageSize = FSMC_PAGE_SIZE_NONE;
  /* Timing */
  Timing.AddressSetupTime = 2;
  Timing.AddressHoldTime = 15;
  Timing.DataSetupTime = 8;
  Timing.BusTurnAroundDuration = 0;
  Timing.CLKDivision = 16;
  Timing.DataLatency = 17;
  Timing.AccessMode = FSMC_ACCESS_MODE_A;
  /* ExtTiming */

  if (HAL_SRAM_Init(&hsram3, &Timing, NULL) != HAL_OK)
  {
    Error_Handler( );
  }

  /* USER CODE BEGIN FSMC_Init 2 */

  /* USER CODE END FSMC_Init 2 */
}

/* USER CODE BEGIN 4 */
void MPU_Config_UserSandbox(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  // Turn off the MPU while configuring it
  HAL_MPU_Disable();

  // Region 0: Flash Memory (Code) - Read-only and executable
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x08000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_1MB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_PRIV_RO_URO; // Read-only for all
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE; // Executable
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  // Region 1: Internal SRAM (Stacks and Variables) - Read and Write
  MPU_InitStruct.Number = MPU_REGION_NUMBER1;
  MPU_InitStruct.BaseAddress = 0x20000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_128KB;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE; // Execution from RAM disabled (security)
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  // Region 2: External SRAM (Sandbox) — Read, Write, Execute
  MPU_InitStruct.Number = MPU_REGION_NUMBER2;
  MPU_InitStruct.BaseAddress = 0x68000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_1MB;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  // Enable the MPU. The background region will allow the kernel to access the peripherals (0x40000000)
  // But our task (via tag 1) won't have access there!
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

void SandboxTask(void *argument)
{
  // This task will run in Unprivileged Mode!
  for(;;)
  {
    // 1. SAFE CALL: We don't access the registers directly; instead, we make a request to the kernel via SVC

    // Since the task is unprivileged, the standard osDelay/vTaskDelay may cause an error,
    // if it attempts to access kernel variables that the MPU has restricted access to.
    // Therefore, we create an artificial delay using a loop:
    for(volatile uint32_t i = 0; i < 2000000; i++) {}

    // 2. PROTECTION DEMONSTRATION:
    // Uncomment the line below. This is direct access to the GPIO peripherals.
    // Since we are in the Sandbox, this call SHOULD trigger a MemManage Fault (HardFault)!
  }
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  /* Filter interrupts: process ONLY our 4 buttons KEY_0..KEY_3 */
  if (GPIO_Pin == KEY_0_Pin || GPIO_Pin == KEY_1_Pin || GPIO_Pin == KEY_2_Pin || GPIO_Pin == KEY_3_Pin)
  {
      Buttons_EXTI_Callback(GPIO_Pin);
  }
}
/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* init code for USB_HOST */
  MX_USB_HOST_Init();
  /* USER CODE BEGIN 5 */
  printf("[OS] GuiRenderTask (DefaultTask) Started.\r\n");

  printf("\r\n----------------------------------------\r\n");
  printf("  STM32F407 Console OS - Multi-Tasking  \r\n");
  printf("----------------------------------------\r\n");

  USBHostManager_Init();
  printf("[INIT] USB Host Manager Ready.\r\n");

  uint32_t sys_freq = HAL_RCC_GetSysClockFreq();
  printf("System Clock Frequency: %lu MHz\r\n", sys_freq / 1000000);

  /* 1. Run Power-On Self-Test (POST) */
  POST_Status_t post_status = POST_RunSelfTest();

  /* 2. Initialize feedback subsystems */
  StatusLED_Init();
  Buttons_Init();
  Beeper_Init();
  lcdInit();
  lcdBacklightOn();
  lcdSetOrientation(LCD_ORIENTATION_LANDSCAPE); // 320x240

  /* 3. Output diagnostic report to UART3 terminal */
  POST_PrintSummary();

  /* 4. Process POST status result */
  if (post_status == POST_ERR_SRAM || post_status == POST_ERR_CCMRAM || post_status == POST_ERR_DISPLAY)
  {
    StatusLED_Set(STATUS_LED_ERROR, STATUS_LED_ON);
    Beeper_ErrorAlert();
  }
  else
  {
    Beeper_BootMelody();
  }

  /* 5. Full Peripherals Initialization */
  if (W25qxx_Init())
  {
    printf("[INIT] W25QXX Flash Ready.\r\n");
  }
  else
  {
    printf("[INIT] W25QXX Flash FAILED.\r\n");
  }

  if (SDCard_InitSystemDirs())
  {
    printf("[INIT] SD Card FileSystem Ready.\r\n");
  }
  else
  {
    printf("[INIT] SD Card FileSystem FAILED.\r\n");
  }

  /* 6. LVGL Initialization (Owned exclusively by this task) */
  LVGL_Port_Init();
  printf("[INIT] LVGL Ready.\r\n");

  /* 7. Spawn InputAudioTask & StorageIoTask in CCM RAM */
  OSTasks_Init();

  /* 8. Create UI and request async root directory listing from StorageIoTask */
  Create_File_Explorer_UI(lv_scr_act());
  OSTasks_RequestDirList("/");

  uint32_t last_heartbeat_tick = 0;
  Gui_EventMsg_t gui_evt;

  /* Infinite GUI & Render Loop (~66 FPS / 15ms period) */
  for(;;)
  {
    /* 1. Check for completed async events from StorageIoTask */
    while (OSTasks_GetGuiEvent(&gui_evt))
    {
      if (gui_evt.type == GUI_EVT_DIR_READY || gui_evt.type == GUI_EVT_STORAGE_ERROR)
      {
        Populate_File_List_From_Cache();
      }
    }

    /* 2. Non-blocking Heartbeat on LD0 (1 Hz) */
    if ((HAL_GetTick() - last_heartbeat_tick) >= 500)
    {
      last_heartbeat_tick = HAL_GetTick();
      StatusLED_Toggle(STATUS_LED_HEARTBEAT);
    }

    /* 3. Visual USB Debug Monitor on LCD (every 100ms) */
    static uint32_t last_usb_ui_tick = 0;
    static uint32_t prev_irq_cnt = 0xFFFFFFFF;
    static uint8_t  prev_host_id = 0xFF;
    static uint8_t  prev_hid_st  = 0xFF;

    if ((HAL_GetTick() - last_usb_ui_tick) >= 100 && HAL_GetTick() >= g_status_hold_until)
    {
      last_usb_ui_tick = HAL_GetTick();

      if (g_usb_dbg.irq_event_count != prev_irq_cnt ||
          g_usb_dbg.last_host_id != prev_host_id ||
          g_usb_dbg.hid_state != prev_hid_st)
      {
        prev_irq_cnt = g_usb_dbg.irq_event_count;
        prev_host_id = g_usb_dbg.last_host_id;
        prev_hid_st  = g_usb_dbg.hid_state;

        if (USBHostManager_IsKeyboardReady())
        {
          char ch_disp = (g_usb_dbg.last_ascii >= 32 && g_usb_dbg.last_ascii <= 126)
                         ? (char)g_usb_dbg.last_ascii : '?';
          lv_label_set_text_fmt(g_usb_status_label,
                                "KBD St:%d | Sc:0x%02X '%c' Cnt:%lu",
                                g_usb_dbg.hid_state,
                                g_usb_dbg.last_scancode,
                                ch_disp,
                                (unsigned long)g_usb_dbg.irq_event_count);
        }
        else if (USBHostManager_IsMouseReady())
        {
          char b_l = (g_usb_dbg.mouse_btns & 0x01) ? 'L' : '-';
          char b_m = (g_usb_dbg.mouse_btns & 0x04) ? 'M' : '-';
          char b_r = (g_usb_dbg.mouse_btns & 0x02) ? 'R' : '-';

          lv_label_set_text_fmt(g_usb_status_label,
                                "M X:%d Y:%d %c%c%c [%02X %02X %02X %02X]",
                                (int)g_usb_dbg.mouse_x,
                                (int)g_usb_dbg.mouse_y,
                                b_l, b_m, b_r,
                                g_usb_dbg.raw_report[0],
                                g_usb_dbg.raw_report[1],
                                g_usb_dbg.raw_report[2],
                                g_usb_dbg.raw_report[3]);
        }
        else if (USBHostManager_IsGamepadReady())
        {
          lv_label_set_text_fmt(g_usb_status_label,
                                "PAD [%02X %02X %02X %02X %02X %02X %02X]",
                                g_usb_dbg.raw_report[0],
                                g_usb_dbg.raw_report[1],
                                g_usb_dbg.raw_report[2],
                                g_usb_dbg.raw_report[3],
                                g_usb_dbg.raw_report[4],
                                g_usb_dbg.raw_report[5],
                                g_usb_dbg.raw_report[6]);
        }
        else
        {
          lv_label_set_text_fmt(g_usb_status_label,
                                "USB State:%d Type:%d HidSt:%d",
                                g_usb_dbg.last_host_id,
                                g_usb_dbg.detected_dev_type,
                                g_usb_dbg.hid_state);
        }
      }
    }

    /* 4. LVGL Graphics Rendering */
    lv_timer_handler();

    /* 5. Pause 15ms (~66 FPS frame pacing) */
    osDelay(15);
  }
  /* USER CODE END 5 */
}

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM4 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM4)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */
  lv_tick_inc(1);
  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

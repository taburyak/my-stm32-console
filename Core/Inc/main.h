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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define LD0_Pin GPIO_PIN_3
#define LD0_GPIO_Port GPIOE
#define LD1_Pin GPIO_PIN_4
#define LD1_GPIO_Port GPIOE
#define KEY_0_Pin GPIO_PIN_6
#define KEY_0_GPIO_Port GPIOF
#define KEY_0_EXTI_IRQn EXTI9_5_IRQn
#define KEY_1_Pin GPIO_PIN_7
#define KEY_1_GPIO_Port GPIOF
#define KEY_1_EXTI_IRQn EXTI9_5_IRQn
#define KEY_2_Pin GPIO_PIN_8
#define KEY_2_GPIO_Port GPIOF
#define KEY_2_EXTI_IRQn EXTI9_5_IRQn
#define KEY_3_Pin GPIO_PIN_9
#define KEY_3_GPIO_Port GPIOF
#define KEY_3_EXTI_IRQn EXTI9_5_IRQn
#define LCD_BL_Pin GPIO_PIN_10
#define LCD_BL_GPIO_Port GPIOF
#define T_CLK_Pin GPIO_PIN_5
#define T_CLK_GPIO_Port GPIOA
#define T_CS_Pin GPIO_PIN_0
#define T_CS_GPIO_Port GPIOB
#define T_MOSI_Pin GPIO_PIN_1
#define T_MOSI_GPIO_Port GPIOB
#define T_MISO_Pin GPIO_PIN_2
#define T_MISO_GPIO_Port GPIOB
#define T_PEN_Pin GPIO_PIN_11
#define T_PEN_GPIO_Port GPIOF
#define T_PEN_EXTI_IRQn EXTI15_10_IRQn
#define W25_CS_Pin GPIO_PIN_8
#define W25_CS_GPIO_Port GPIOG
#define DS18B20_Pin GPIO_PIN_15
#define DS18B20_GPIO_Port GPIOA
#define LD2_Pin GPIO_PIN_9
#define LD2_GPIO_Port GPIOG

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */

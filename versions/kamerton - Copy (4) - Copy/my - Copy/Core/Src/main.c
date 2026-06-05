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
#include "fatfs.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "waveplayer.h"
#include "AUDIO.h"
#include "LCD1602.h"
#include "microphone.h"
#include "recorder.h"
#include "chord.h"
#include "flash_recorder.h"
#include "ttp229.h"
#include "joystick.h"
#include "sampler.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef enum { APP_STATE_PLAYING = 0, APP_STATE_MUTED } AppState_t;
typedef enum {
    APP_MODE_GENERATOR = 0,
    APP_MODE_TUNER,
    APP_MODE_CHORD,
    APP_MODE_ARPEGGIO,
    APP_MODE_WARMUP,
    APP_MODE_RECORDER,
    APP_MODE_SAMPLER,
    APP_MODE_COUNT
} AppMode_t;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define NUM_NOTES    7u
#define NUM_OCTAVES  4u
#define BASE_OCTAVE  2u
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
ADC_HandleTypeDef hadc2;
DMA_HandleTypeDef hdma_adc1;

CRC_HandleTypeDef hcrc;

I2S_HandleTypeDef hi2s3;
DMA_HandleTypeDef hdma_spi3_tx;

SPI_HandleTypeDef hspi2;

TIM_HandleTypeDef htim3;

I2C_HandleTypeDef hi2c1;   /* shared by LCD (PCF8574) and AUDIO_LINK (CS43L22) */

/* USER CODE BEGIN PV */
/* Frequency table: centihz (Hz×100), octaves 2-5, notes C..B */
const uint32_t TONE_FREQUENCIES[NUM_OCTAVES][NUM_NOTES] = {
    {  6541,  7342,  8241,  8731,  9800, 11000, 12347 },  /* octave 2 */
    { 13082, 14683, 16482, 17462, 19600, 22000, 24694 },  /* octave 3 */
    { 26163, 29366, 32963, 34923, 39200, 44000, 49388 },  /* octave 4 */
    { 52326, 58733, 65927, 69846, 78400, 88000, 98776 },  /* octave 5 */
};
const char *NOTE_NAMES[NUM_NOTES] = { "Ut","Re","Mi","Fa","So","La","Si" };

volatile uint8_t currentNote   = 5u;   /* La/A — A4=440 Hz */
volatile uint8_t currentOctave = 2u;   /* index 2 = octave 4 */
uint8_t          currentVolume = 50u;
uint8_t          currentPreset = 0u;
AppState_t       appState      = APP_STATE_PLAYING;
AppMode_t        appMode       = APP_MODE_GENERATOR;

uint8_t barHeights[LCD_COLS] = { 0 };  /* EQ bar heights for row 3 */

/* Sampler */
static Sampler_t sampler;

/* ── Mode navigation menu (joystick left/right) ────────────────────────── */
static const char * const MODE_NAMES[APP_MODE_COUNT] = {
    "Generator",
    "Tuner",
    "Chord",
    "Arpeggio",
    "Warmup",
    "Recorder",
    "Sampler",
};
static uint8_t  s_menu_active  = 0;    /* 1 = menu overlay is shown           */
static uint8_t  s_menu_cursor  = 0;    /* index being previewed (0..COUNT-1)  */
static uint32_t s_menu_tick    = 0;    /* HAL tick of last joystick move       */
#define MENU_TIMEOUT_MS  2500u         /* auto-confirm after 2.5 s idle        */

/* -----------------------------------------------------------------------
 * Arpeggiator state (chord mode, keys 1-7)
 * --------------------------------------------------------------------- */
typedef enum { ARP_IDLE = 0, ARP_PLAYING, ARP_REST } ArpState_t;
typedef struct { uint8_t oct; uint8_t note; } ArpNote_t;

#define ARP_NOTE_MS   800u   /* each note duration (slowed from 500 ms) */
#define ARP_REST_MS  1500u   /* silence between repetitions */
#define ARP_REPS        2u   /* total number of repetitions */

/* Diatonic chords in C major — {octave index, note index (0=C..6=B)} */
static const ArpNote_t ARP_CHORDS[7][3] = {
    {{2u,0u},{2u,2u},{2u,4u}},  /* I:   C4-E4-G4 */
    {{2u,1u},{2u,3u},{2u,5u}},  /* II:  D4-F4-A4 */
    {{2u,2u},{2u,4u},{2u,6u}},  /* III: E4-G4-B4 */
    {{2u,3u},{2u,5u},{3u,0u}},  /* IV:  F4-A4-C5 */
    {{2u,4u},{2u,6u},{3u,1u}},  /* V:   G4-B4-D5 */
    {{2u,5u},{3u,0u},{3u,2u}},  /* VI:  A4-C5-E5 */
    {{2u,6u},{3u,1u},{3u,3u}},  /* VII: B4-D5-F5 */
};
static const char *ARP_CHORD_NAMES[7] = {"C","Dm","Em","F","G","Am","Bdim"};

static ArpState_t arp_state     = ARP_IDLE;
static uint32_t   arp_timer     = 0u;
static uint8_t    arp_note_idx  = 0u;
static uint8_t    arp_rep       = 0u;
static uint8_t    arp_chord_idx = 0u;

/* -----------------------------------------------------------------------
 * Chord-mode strum — keys 1-7 trigger a quick 3-note strum (non-blocking).
 * Each note plays for STRUM_NOTE_MS ms then advances to the next chord tone.
 * After all 3 notes, synthesis returns to silent (mic listening) mode.
 * --------------------------------------------------------------------- */
typedef enum { STRUM_IDLE = 0, STRUM_PLAYING } StrumState_t;
static StrumState_t strum_state     = STRUM_IDLE;
static uint32_t     strum_timer     = 0u;
static uint8_t      strum_note_idx  = 0u;
static uint8_t      strum_chord_idx = 0u;
#define STRUM_NOTE_MS  150u   /* ms per strum note */

/* -----------------------------------------------------------------------
 * Recording countdown — 3-second delay before capture begins
 * --------------------------------------------------------------------- */
static bool     rec_counting      = false;
static uint8_t  rec_countdown     = 0u;
static uint32_t rec_count_timer   = 0u;

/* -----------------------------------------------------------------------
 * Vocal Warm-up mode — 4 standard exercises (relative scale degrees)
 *
 * Degree encoding (maps to the 7-note scale used by NOTE_NAMES[]):
 *   0 = Ut/C, 1 = Re/D, 2 = Mi/E, 3 = Fa/F, 4 = Sol/G,
 *   5 = La/A, 6 = Si/B   — all in octave index 2 (displayed C4-B4)
 *   7 = Ut/C upper octave — octave index 3 (displayed C5)
 * 255 = end-of-sequence sentinel
 *
 * FSM states:
 *   WU_IDLE    — not playing
 *   WU_PLAYING — note sounds for WU_NOTE_MS (600 ms)
 *   WU_GAP     — 100 ms muted micro-pause between notes (simulates articulation)
 *   WU_REST    — WU_REST_MS silence after sequence completes
 * --------------------------------------------------------------------- */
#define WU_NOTE_MS        600u   /* note-on duration                         */
#define WU_GAP_MS         100u   /* inter-note muted gap (articulation)       */
#define WU_REST_MS        800u   /* post-sequence silence                     */
#define WU_TRANS_REST_MS  500u   /* pause between transposed repetitions      */
#define WU_TRANS_MAX        3u   /* steps to ascend before turning back       */
#define WU_SEQ_COUNT        7u
/* Sequences at indices 2-4 use the transposition loop */
#define wu_has_transpose(i) ((i) >= 2u && (i) <= 4u)

static const uint8_t WARMUP_1[] = {0, 1, 2, 3, 4, 3, 2, 1, 0, 255};   /* 5-tone scale up+down  */
static const uint8_t WARMUP_2[] = {0, 2, 4, 7, 4, 2, 0, 255};          /* Octave arpeggio       */
static const uint8_t WARMUP_3[] = {0, 2, 4, 2, 0, 255};                 /* Major triad bounce    */
static const uint8_t WARMUP_4[] = {4, 3, 2, 1, 0, 255};                 /* 5-tone descending     */
static const uint8_t WARMUP_5[] = {7, 4, 2, 0, 255};                    /* Octave drop triad     */
static const uint8_t WARMUP_6[] = {0, 7, 0, 255};                       /* Root-Octave-Root      */
static const uint8_t WARMUP_7[] = {2, 1, 0, 255};                       /* Descending 3rd        */

static const uint8_t * const WARMUP_LIST[WU_SEQ_COUNT] = {
    WARMUP_1, WARMUP_2, WARMUP_3, WARMUP_4, WARMUP_5, WARMUP_6, WARMUP_7
};
static const char * const WARMUP_NAMES[WU_SEQ_COUNT] = {
    "5-Tone Scale  ", "Octave Arpeg  ", "Major Triad   ",
    "5-Tone Desc   ", "Octave Drop   ", "Root-Oct-Root ", "Descend 3rd   "
};

typedef enum { WU_IDLE = 0, WU_PLAYING, WU_GAP, WU_TRANS_REST, WU_REST } WarmupState_t;
static WarmupState_t wu_state      = WU_IDLE;
static uint32_t      wu_timer      = 0u;
static uint8_t       wu_seq_idx    = 0u;
static uint8_t       wu_note_idx   = 0u;
static uint8_t       wu_transpose  = 0u;   /* current scale-degree offset */
static int8_t        wu_trans_dir  = 1;    /* +1 ascending, -1 descending */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_I2S3_Init(void);
static void MX_CRC_Init(void);
static void MX_ADC1_Init(void);
static void MX_ADC2_Init(void);
static void MX_SPI2_Init(void);
static void MX_TIM3_Init(void);
static void MX_I2C1_Init(void);

/* USER CODE BEGIN PFP */
static void Apply_Mode_Change(AppMode_t new_mode);
static void Handle_Key_TTP229(uint8_t k);
static void Task_Keypad(void);
static void Task_Sampler(void);
static void Task_Joystick(void);
static void Task_LCD_ModeMenu(void);
static void Task_LCD_Sampler(void);
static void Task_Audio(void);
static void Task_Arpeggiator(void);
static void Task_LCD(void);
static void Task_LCD_Tuner(void);
static void Task_LCD_Chord(void);
static void Task_LCD_Arpeggio(void);
static void Task_LCD_Recorder(void);
static uint8_t Calculate_Real_Volume(uint8_t displayVol);
static void    Apply_Volume(void);
static void    Update_LEDs(void);
static void    Task_Strum(void);
static void    Task_RecCountdown(void);
static void    Task_Warmup(void);
static void    Task_LCD_Warmup(void);
static void    wu_apply_degree(uint8_t deg);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

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
  MX_I2S3_Init();
  MX_SPI2_Init();
  MX_FATFS_Init();
  MX_CRC_Init();
  MX_ADC1_Init();
  MX_ADC2_Init();
  MX_TIM3_Init();
  /* USER CODE BEGIN 2 */

  /* Init on-board LEDs (PD12-PD15) */
  {
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOD_CLK_ENABLE();
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_12|GPIO_PIN_13|GPIO_PIN_14|GPIO_PIN_15, GPIO_PIN_RESET);
    g.Pin   = GPIO_PIN_12|GPIO_PIN_13|GPIO_PIN_14|GPIO_PIN_15;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOD, &g);
  }

  MX_I2C1_Init();      /* must come before LCD_Init — LCD uses hi2c1 */
  TTP229_Init();
  Joystick_Init();

  LCD_Init();
  LCD_LoadGraphIcons();

  AUDIO_PLAYER_Init();
  AUDIO_OUT_SetFrequency(44100u);
  AUDIO_PLAYER_SetPreset(currentPreset);
  AUDIO_PLAYER_Start(0u);
  Apply_Volume();

  MIC_Init();
  MIC_Start();

  REC_Init();
  CHORD_Init();
  FLASH_REC_Init();
  Sampler_Init(&sampler);
  /* USER CODE END 2 */

  /* Infinite loop */
  while (1)
  {
    Task_Audio();
    Task_Keypad();
    Task_Joystick();
    Task_Arpeggiator();
    Task_Strum();
    Task_RecCountdown();
    Task_Warmup();
    Task_Sampler();
    MIC_Process();
    CHORD_Process();
    /* Mode-menu overlay takes priority when joystick is navigating */
    if (s_menu_active) {
        Task_LCD_ModeMenu();
    } else {
        switch (appMode) {
            case APP_MODE_TUNER:    Task_LCD_Tuner();    break;
            case APP_MODE_CHORD:    Task_LCD_Chord();    break;
            case APP_MODE_ARPEGGIO: Task_LCD_Arpeggio(); break;
            case APP_MODE_WARMUP:   Task_LCD_Warmup();   break;
            case APP_MODE_RECORDER: Task_LCD_Recorder(); break;
            case APP_MODE_SAMPLER:  Task_LCD_Sampler();  break;
            default:                Task_LCD();          break;
        }
    }
  }
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
  RCC_OscInitStruct.PLL.PLLN = 192;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ = 8;
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

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV2;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = ENABLE;
  hadc1.Init.ContinuousConvMode = ENABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
  hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIGCONV_T3_TRGO;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_13;
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_3CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief SPI2 Initialization Function (SD card)
  *   PB10 = SCK, PB14 = MISO, PB15 = MOSI, PB12 = CS (GPIO)
  */
static void MX_SPI2_Init(void)
{
    __HAL_RCC_SPI2_CLK_ENABLE();

    hspi2.Instance               = SPI2;
    hspi2.Init.Mode              = SPI_MODE_MASTER;
    hspi2.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi2.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi2.Init.CLKPolarity       = SPI_POLARITY_LOW;
    hspi2.Init.CLKPhase          = SPI_PHASE_1EDGE;
    hspi2.Init.NSS               = SPI_NSS_SOFT;
    hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_256; /* slow for init */
    hspi2.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi2.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi2.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi2.Init.CRCPolynomial     = 10;
    if (HAL_SPI_Init(&hspi2) != HAL_OK)
        Error_Handler();
}

/**
  * @brief ADC2 Initialization Function (joystick axes)
  *   PC1 = ADC2_IN11 (X), PC2 = ADC2_IN12 (Y) — configured at runtime by joystick.c
  */
static void MX_ADC2_Init(void)
{
    __HAL_RCC_ADC2_CLK_ENABLE();

    /* Configure PC1 and PC2 as analog inputs */
    GPIO_InitTypeDef g = {0};
    g.Pin  = GPIO_PIN_1 | GPIO_PIN_2;
    g.Mode = GPIO_MODE_ANALOG;
    g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOC, &g);

    hadc2.Instance                   = ADC2;
    hadc2.Init.ClockPrescaler        = ADC_CLOCK_SYNC_PCLK_DIV2;
    hadc2.Init.Resolution            = ADC_RESOLUTION_12B;
    hadc2.Init.ScanConvMode          = DISABLE;
    hadc2.Init.ContinuousConvMode    = DISABLE;
    hadc2.Init.DiscontinuousConvMode = DISABLE;
    hadc2.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_NONE;
    hadc2.Init.ExternalTrigConv      = ADC_SOFTWARE_START;
    hadc2.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
    hadc2.Init.NbrOfConversion       = 1;
    hadc2.Init.DMAContinuousRequests = DISABLE;
    hadc2.Init.EOCSelection          = ADC_EOC_SINGLE_CONV;
    if (HAL_ADC_Init(&hadc2) != HAL_OK)
        Error_Handler();
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
  * @brief I2S3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2S3_Init(void)
{

  /* USER CODE BEGIN I2S3_Init 0 */

  /* USER CODE END I2S3_Init 0 */

  /* USER CODE BEGIN I2S3_Init 1 */

  /* USER CODE END I2S3_Init 1 */
  hi2s3.Instance = SPI3;
  hi2s3.Init.Mode = I2S_MODE_MASTER_TX;
  hi2s3.Init.Standard = I2S_STANDARD_PHILIPS;
  hi2s3.Init.DataFormat = I2S_DATAFORMAT_16B;
  hi2s3.Init.MCLKOutput = I2S_MCLKOUTPUT_ENABLE;
  hi2s3.Init.AudioFreq = I2S_AUDIOFREQ_44K;
  hi2s3.Init.CPOL = I2S_CPOL_LOW;
  hi2s3.Init.ClockSource = I2S_CLOCK_PLL;
  hi2s3.Init.FullDuplexMode = I2S_FULLDUPLEXMODE_DISABLE;
  if (HAL_I2S_Init(&hi2s3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2S3_Init 2 */

  /* USER CODE END I2S3_Init 2 */

}

/**
  * @brief TIM3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM3_Init(void)
{

  /* USER CODE BEGIN TIM3_Init 0 */

  /* USER CODE END TIM3_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM3_Init 1 */

  /* USER CODE END TIM3_Init 1 */
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 83;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 62;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim3, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM3_Init 2 */

  /* USER CODE END TIM3_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA1_CLK_ENABLE();
  __HAL_RCC_DMA2_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Stream5_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Stream5_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream5_IRQn);
  /* DMA2_Stream0_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);

}

/**
  * @brief I2C1 Initialization (shared by LCD PCF8574 and CS43L22 audio codec)
  *   PB6 = SCL (AF4, OD), PB9 = SDA (AF4, OD), 100 kHz
  */
static void MX_I2C1_Init(void)
{
    hi2c1.Instance             = I2C1;
    hi2c1.Init.ClockSpeed      = 100000;
    hi2c1.Init.DutyCycle       = I2C_DUTYCYCLE_2;
    hi2c1.Init.OwnAddress1     = 0;
    hi2c1.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.OwnAddress2     = 0;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&hi2c1) != HAL_OK)
    {
        Error_Handler();
    }
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
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_0, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOE, GPIO_PIN_8, GPIO_PIN_SET);   /* TTP229 SCL idle HIGH */

  /*Configure GPIO pin Output Level — SD CS deselected (HIGH) */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_SET);

  /*Configure GPIO pin : PC0 */
  GPIO_InitStruct.Pin = GPIO_PIN_0;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pin : PA0 */
  GPIO_InitStruct.Pin = GPIO_PIN_0;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : PE8 */
  GPIO_InitStruct.Pin = GPIO_PIN_8;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /*Configure GPIO pin : PE9 — TTP229 SDO (input, pull-up; chip pulls low when key pressed) */
  GPIO_InitStruct.Pin = GPIO_PIN_9;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /*Configure GPIO pin : PE10 — Joystick button (input, pull-up; button active LOW) */
  GPIO_InitStruct.Pin = GPIO_PIN_10;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /*Configure GPIO pins : PB10 PB14 PB15 — SPI2 (SCK, MISO, MOSI) */
  GPIO_InitStruct.Pin = GPIO_PIN_10|GPIO_PIN_14|GPIO_PIN_15;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF5_SPI2;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : PB12 — SD card CS (output, push-pull, starts HIGH) */
  GPIO_InitStruct.Pin = GPIO_PIN_12;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : PB9 */
  GPIO_InitStruct.Pin = GPIO_PIN_9;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF4_I2C1;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI0_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI0_IRQn);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* -----------------------------------------------------------------------
 * TTP229 key mapping (16 keys, k = 1..16)
 *
 * ┌──────┬──────┬──────┬──────┐
 * │  1   │  2   │  3   │  4   │  1-7  = notes Ut Re Mi Fa Sol La Si
 * │  Ut  │  Re  │  Mi  │  Fa  │  8    = octave cycle
 * ├──────┼──────┼──────┼──────┤  9    = mute toggle
 * │  5   │  6   │  7   │  8   │  10   = volume +5 %
 * │  Sol │  La  │  Si  │  Oct │  11   = volume -5 %
 * ├──────┼──────┼──────┼──────┤  12   = preset cycle
 * │  9   │  10  │  11  │  12  │  13   = mode cycle
 * │  Mut │ Vol+ │ Vol- │ Prst │  14   = Record toggle
 * ├──────┼──────┼──────┼──────┤  15   = Play toggle
 * │  13  │  14  │  15  │  16  │  16   = Stop / Sampler function key
 * │ Mode │ Rec  │ Play │Stop/F│
 * └──────┴──────┴──────┴──────┘
 *
 * In SAMPLER mode:
 *   Keys 1-15 = pads
 *   Key 16    = function key (hold + tap pad = record; erase handled in sampler.c)
 * --------------------------------------------------------------------- */
static void Handle_Key_TTP229(uint8_t k)
{
    /* In Sampler mode, keys 1-12 and 16 are handled by Sampler_Update.
     * Keys 13-15 (mode change, record, play) still go through here. */
    if (appMode == APP_MODE_SAMPLER && k != 13 && k != 14 && k != 15)
        return;

    switch (k)
    {
        /* ── Notes 1-7 ────────────────────────────────────────────── */
        case 1: case 2: case 3: case 4:
        case 5: case 6: case 7:
            if (appMode == APP_MODE_ARPEGGIO) {
                arp_chord_idx = k - 1u;
                arp_note_idx  = 0u;
                arp_rep       = 0u;
                arp_state     = ARP_PLAYING;
                currentNote   = ARP_CHORDS[arp_chord_idx][0u].note;
                currentOctave = ARP_CHORDS[arp_chord_idx][0u].oct;
                AUDIO_PLAYER_SetSilence(false);
                AUDIO_PLAYER_NoteChange();
                arp_timer = HAL_GetTick();
            } else if (appMode == APP_MODE_WARMUP) {
                wu_seq_idx   = k - 1u;
                wu_note_idx  = 0u;
                wu_transpose = 0u;
                wu_trans_dir = 1;
                wu_state     = WU_PLAYING;
                wu_apply_degree(WARMUP_LIST[wu_seq_idx][0]);
                wu_timer = HAL_GetTick();
            } else {
                currentNote = k - 1u;
                for (int i = 0; i < (int)LCD_COLS; i++) barHeights[i] = 0u;
                AUDIO_PLAYER_NoteChange();
            }
            break;

        /* ── 8: octave cycle ──────────────────────────────────────── */
        case 8:
            currentOctave = (currentOctave + 1u) % NUM_OCTAVES;
            for (int i = 0; i < (int)LCD_COLS; i++) barHeights[i] = 0u;
            AUDIO_PLAYER_NoteChange();
            break;

        /* ── 9: mute toggle ───────────────────────────────────────── */
        case 9:
            appState = (appState == APP_STATE_PLAYING) ? APP_STATE_MUTED : APP_STATE_PLAYING;
            Apply_Volume();
            break;

        /* ── 10: volume +5 % ──────────────────────────────────────── */
        case 10:
            if (currentVolume <= 95u) currentVolume += 5u;
            Apply_Volume();
            break;

        /* ── 11: volume -5 % ──────────────────────────────────────── */
        case 11:
            if (currentVolume >= 5u) currentVolume -= 5u;
            Apply_Volume();
            break;

        /* ── 12: preset cycle ─────────────────────────────────────── */
        case 12:
            currentPreset = (currentPreset + 1u) % NUM_PRESETS;
            AUDIO_PLAYER_SetPreset(currentPreset);
            AUDIO_PLAYER_NoteChange();
            break;

        /* ── 13: mode cycle (forward) ────────────────────────────── */
        case 13:
            Apply_Mode_Change((AppMode_t)((appMode + 1u) % APP_MODE_COUNT));
            s_menu_active = 0;   /* dismiss joystick menu if open */
            break;

        /* ── 14: Record toggle ────────────────────────────────────── */
        case 14:
            if (rec_counting) {
                rec_counting = false;
            } else if (REC_GetState() == REC_STATE_RECORDING) {
                REC_StopRecord();
                if (appMode == APP_MODE_RECORDER &&
                    REC_GetSamplesRecorded() > 0u &&
                    !FLASH_REC_IsFull()) {
                    AUDIO_PLAYER_SetVolume(0u);
                    FLASH_REC_SaveFromRecorder();
                    Apply_Volume();
                }
            } else {
                if (appMode == APP_MODE_RECORDER) {
                    rec_counting    = true;
                    rec_countdown   = 3u;
                    rec_count_timer = HAL_GetTick();
                } else {
                    REC_StartRecord();
                }
            }
            break;

        /* ── 15: Play toggle ──────────────────────────────────────── */
        case 15:
            if (REC_GetState() == REC_STATE_PLAYING) {
                REC_StopPlay();
            } else if (appMode == APP_MODE_RECORDER) {
                if (REC_GetSamplesRecorded() == 0u) break;
                uint8_t slot = FLASH_REC_GetLastSavedSlot();
                if (slot != 0xFFu) {
                    uint32_t n         = FLASH_REC_GetSlotSampleCount(slot);
                    const int16_t *ptr = FLASH_REC_GetSlotDataPtr(slot);
                    if (ptr != NULL && n > 0u) REC_LoadExternal(ptr, n);
                }
                REC_StartPlay();
            } else {
                REC_StartPlay();
            }
            break;

        /* ── 16: Stop all ─────────────────────────────────────────── */
        case 16:
            REC_StopRecord();
            REC_StopPlay();
            rec_counting = false;
            break;

        default: break;
    }
}

static void Task_Keypad(void)
{
    TTP229_Update();
    for (uint8_t k = 1u; k <= 16u; k++) {
        if (TTP229_JustPressed(k))
            Handle_Key_TTP229(k);
    }
}

/* -----------------------------------------------------------------------
 * Apply_Mode_Change — shared cleanup when switching to a new appMode
 * --------------------------------------------------------------------- */
static void Apply_Mode_Change(AppMode_t new_mode)
{
    AppMode_t prev = appMode;
    if (new_mode == prev) return;

    appMode = new_mode;
    LCD_Clear();

    /* Clean up leaving mode */
    if (prev == APP_MODE_RECORDER) {
        rec_counting = false;
        REC_StopRecord();
        REC_StopPlay();
    }
    if (prev == APP_MODE_ARPEGGIO)  arp_state   = ARP_IDLE;
    if (prev == APP_MODE_CHORD)     strum_state = STRUM_IDLE;
    if (prev == APP_MODE_WARMUP) {
        wu_state     = WU_IDLE;
        wu_transpose = 0u;
        wu_trans_dir = 1;
        AUDIO_PLAYER_SetSilence(true);
    }

    /* Init entering mode */
    if (appMode == APP_MODE_CHORD)   CHORD_Init();
    if (appMode == APP_MODE_SAMPLER) Sampler_Init(&sampler);

    AUDIO_PLAYER_SetSilence(appMode != APP_MODE_GENERATOR);
    Update_LEDs();
}

/* -----------------------------------------------------------------------
 * Task_Joystick — mode navigation menu + recorder play
 * --------------------------------------------------------------------- */
static void Task_Joystick(void)
{
    Joystick_Update();

    /* Auto-confirm menu after timeout */
    if (s_menu_active &&
        (HAL_GetTick() - s_menu_tick) >= MENU_TIMEOUT_MS)
    {
        s_menu_active = 0;
        Apply_Mode_Change((AppMode_t)s_menu_cursor);
        /* Apply_Mode_Change calls LCD_Clear(), so next frame renders fresh */
        return;
    }

    JoyEvent_t ev = Joystick_GetEvent();
    if (ev == JOY_NONE) return;

    /* ── LEFT: previous mode ──────────────────────────────────────── */
    if (ev == JOY_LEFT) {
        if (!s_menu_active) {
            s_menu_active = 1;
            s_menu_cursor = (uint8_t)appMode;
            LCD_Clear();
        }
        s_menu_cursor = (s_menu_cursor == 0)
                        ? (APP_MODE_COUNT - 1u)
                        : s_menu_cursor - 1u;
        s_menu_tick = HAL_GetTick();
        return;
    }

    /* ── RIGHT: next mode ─────────────────────────────────────────── */
    if (ev == JOY_RIGHT) {
        if (!s_menu_active) {
            s_menu_active = 1;
            s_menu_cursor = (uint8_t)appMode;
            LCD_Clear();
        }
        s_menu_cursor = (s_menu_cursor + 1u) % APP_MODE_COUNT;
        s_menu_tick = HAL_GetTick();
        return;
    }

    /* ── PRESS: confirm selected mode ────────────────────────────── */
    if (ev == JOY_PRESS) {
        if (s_menu_active) {
            Apply_Mode_Change((AppMode_t)s_menu_cursor);
            s_menu_active = 0;
        } else {
            /* Not in menu: press = play/stop last recording */
            if (appMode == APP_MODE_RECORDER) {
                if (REC_GetState() == REC_STATE_PLAYING) {
                    REC_StopPlay();
                } else {
                    uint8_t slot       = FLASH_REC_GetLastSavedSlot();
                    uint32_t n         = FLASH_REC_GetSlotSampleCount(slot);
                    const int16_t *ptr = FLASH_REC_GetSlotDataPtr(slot);
                    if (ptr && n > 0u) { REC_LoadExternal(ptr, n); REC_StartPlay(); }
                }
            }
        }
        return;
    }

    /* ── UP / DOWN: cancel menu without switching ─────────────────── */
    if (ev == JOY_UP || ev == JOY_DOWN) {
        if (s_menu_active) {
            s_menu_active = 0;   /* cancel preview, stay in current mode */
        }
    }
}

/* -----------------------------------------------------------------------
 * Task_Sampler — delegates to Sampler_Update when in sampler mode
 * --------------------------------------------------------------------- */
static void Task_Sampler(void)
{
    if (appMode != APP_MODE_SAMPLER) return;
    Sampler_Update(&sampler);
}

/* -----------------------------------------------------------------------
 * Task_LCD_ModeMenu — joystick mode-selection overlay (20×4)
 *
 * Shows up to 3 entries: prev / selected(►) / next
 * Appears when joystick moves left/right, auto-confirms after MENU_TIMEOUT_MS.
 *
 * Row 0: "──── SELECT MODE ────"
 * Row 1:  previous mode (or blank if cursor = 0)
 * Row 2: "► Current Mode      "  ← highlighted with arrow
 * Row 3:  next mode (or blank if cursor = last)
 * --------------------------------------------------------------------- */
static void Task_LCD_ModeMenu(void)
{
    static uint8_t last_cursor = 0xFF;   /* force full redraw on first call */

    /* Only redraw when cursor changed */
    if (s_menu_cursor == last_cursor) return;
    last_cursor = s_menu_cursor;

    Task_Audio();

    LCD_SetCursor(0, 0);
    LCD_WriteString("---- SELECT MODE ----");

    Task_Audio();

    /* Previous mode (row 1) */
    char row[21];
    if (s_menu_cursor > 0) {
        snprintf(row, sizeof(row), "  %-18s",
                 MODE_NAMES[s_menu_cursor - 1u]);
    } else {
        snprintf(row, sizeof(row), "%-20s", "");
    }
    LCD_SetCursor(1, 0); LCD_WriteString(row);

    Task_Audio();

    /* Selected mode (row 2) — arrow */
    snprintf(row, sizeof(row), "> %-18s", MODE_NAMES[s_menu_cursor]);
    LCD_SetCursor(2, 0); LCD_WriteString(row);

    Task_Audio();

    /* Next mode (row 3) */
    if (s_menu_cursor < APP_MODE_COUNT - 1u) {
        snprintf(row, sizeof(row), "  %-18s",
                 MODE_NAMES[s_menu_cursor + 1u]);
    } else {
        snprintf(row, sizeof(row), "%-20s", "");
    }
    LCD_SetCursor(3, 0); LCD_WriteString(row);
}

/* -----------------------------------------------------------------------
 * Sampler LCD (20×4)
 *
 * Row 0: "=== PO-33 SAMPLER ==="
 * Row 1: "1234567 89 0123456  " — 15 pad states (R=ready E=empty *)
 * Row 2: " PAD xx RECORDING   " or PLAYING or EMPTY
 * Row 3: "[16+tap]=REC [tap]=>"
 * --------------------------------------------------------------------- */
static void Task_LCD_Sampler(void)
{
    static uint32_t lastUpdate = 0u;
    if (HAL_GetTick() - lastUpdate < 150u) return;
    lastUpdate = HAL_GetTick();

    Task_Audio();
    LCD_SetCursor(0, 0);
    LCD_WriteString("=== PO-33 SAMPLER===");

    /* Row 1: 15 pad state chars */
    char row1[21];
    for (uint8_t p = 0; p < SAMPLER_PADS; p++) {
        char c;
        switch (sampler.state[p]) {
            case SPAD_READY:     c = '*'; break;
            case SPAD_RECORDING: c = 'R'; break;
            case SPAD_PLAYING:   c = '>'; break;
            default:             c = '.'; break;
        }
        row1[p] = c;
    }
    row1[15] = ' '; row1[16] = ' '; row1[17] = ' '; row1[18] = ' '; row1[19] = ' ';
    row1[20] = '\0';
    LCD_SetCursor(1, 0); LCD_WriteString(row1);

    Task_Audio();

    char row2[21];
    if (sampler.active_pad && sampler.active_pad <= SAMPLER_PADS) {
        uint8_t p = sampler.active_pad;
        switch (sampler.state[p-1]) {
            case SPAD_RECORDING:
                snprintf(row2, sizeof(row2), "PAD %02u: RECORDING  ", p);
                break;
            case SPAD_PLAYING:
                snprintf(row2, sizeof(row2), "PAD %02u: PLAYING >> ", p);
                break;
            default:
                snprintf(row2, sizeof(row2), "%-20s", "                    ");
                break;
        }
    } else {
        snprintf(row2, sizeof(row2), "%-20s", "  tap a pad (1-15)  ");
    }
    LCD_SetCursor(2, 0); LCD_WriteString(row2);

    Task_Audio();
    LCD_SetCursor(3, 0);
    LCD_WriteString(sampler.fn_held ? "[16]=erase/rec pad  "
                                    : "16+pad:rec tap:play ");
}

static void Task_Audio(void)
{
    AUDIO_PLAYER_Process(true);
}

/* -----------------------------------------------------------------------
 * Arpeggiator — advances through diatonic chord notes on a 500 ms clock.
 * Two full repetitions (note1→note2→note3→1.5 s rest) then stops.
 * --------------------------------------------------------------------- */
static void Task_Arpeggiator(void)
{
    if (arp_state == ARP_IDLE) return;

    uint32_t elapsed = HAL_GetTick() - arp_timer;

    if (arp_state == ARP_PLAYING) {
        if (elapsed < ARP_NOTE_MS) return;

        arp_note_idx++;
        if (arp_note_idx >= 3u) {
            /* All three notes played — enter rest */
            arp_note_idx = 0u;
            arp_rep++;
            AUDIO_PLAYER_SetSilence(true);
            if (arp_rep >= ARP_REPS) {
                arp_state = ARP_IDLE;
                return;
            }
            arp_state = ARP_REST;
            arp_timer = HAL_GetTick();
        } else {
            /* Advance to next note in the chord */
            currentNote   = ARP_CHORDS[arp_chord_idx][arp_note_idx].note;
            currentOctave = ARP_CHORDS[arp_chord_idx][arp_note_idx].oct;
            AUDIO_PLAYER_SetSilence(false);
            AUDIO_PLAYER_NoteChange();
            arp_timer = HAL_GetTick();
        }
    } else if (arp_state == ARP_REST) {
        if (elapsed < ARP_REST_MS) return;

        /* Start next repetition */
        arp_state     = ARP_PLAYING;
        arp_note_idx  = 0u;
        currentNote   = ARP_CHORDS[arp_chord_idx][0u].note;
        currentOctave = ARP_CHORDS[arp_chord_idx][0u].oct;
        AUDIO_PLAYER_SetSilence(false);
        AUDIO_PLAYER_NoteChange();
        arp_timer = HAL_GetTick();
    }
}

/* -----------------------------------------------------------------------
 * Chord mode strum — advances through 3 chord tones at STRUM_NOTE_MS each.
 * Runs only when appMode == APP_MODE_CHORD; called from main loop.
 * After all 3 notes, returns to silence so the microphone can listen again.
 * --------------------------------------------------------------------- */
static void Task_Strum(void)
{
    if (strum_state == STRUM_IDLE) return;
    if (HAL_GetTick() - strum_timer < STRUM_NOTE_MS) return;

    strum_note_idx++;
    if (strum_note_idx >= 3u) {
        /* All three chord tones played — silence synthesis, mic can listen */
        strum_state = STRUM_IDLE;
        AUDIO_PLAYER_SetSilence(true);
        return;
    }

    /* Advance to next chord tone */
    currentNote   = ARP_CHORDS[strum_chord_idx][strum_note_idx].note;
    currentOctave = ARP_CHORDS[strum_chord_idx][strum_note_idx].oct;
    AUDIO_PLAYER_SetSilence(false);
    AUDIO_PLAYER_NoteChange();
    strum_timer = HAL_GetTick();
}

/* -----------------------------------------------------------------------
 * Generator LCD (20×4): note+freq / preset / volume bar / EQ
 * --------------------------------------------------------------------- */
static void Task_LCD(void)
{
    static uint32_t lastUpdate = 0u;
    if (HAL_GetTick() - lastUpdate < 100u) return;
    lastUpdate = HAL_GetTick();

    Task_Audio();

    float freqHz = (float)TONE_FREQUENCIES[currentOctave][currentNote] / 100.0f;
    int   octNum = (int)(BASE_OCTAVE + currentOctave);
    bool  playing = (appState == APP_STATE_PLAYING);

    /* Row 0: note + frequency */
    char row0[21];
    snprintf(row0, sizeof(row0), "%-2s%d    %8.2f Hz  ",
             NOTE_NAMES[currentNote], octNum, freqHz);
    LCD_SetCursor(0, 0); LCD_WriteString(row0);
    Task_Audio();

    /* Row 1: preset or MUTED */
    char row1[21];
    if (playing)
        snprintf(row1, sizeof(row1), "Timbre: %-12s", AUDIO_PRESETS[currentPreset].fullname);
    else
        snprintf(row1, sizeof(row1), "%-20s", "    ** MUTED **");
    LCD_SetCursor(1, 0); LCD_WriteString(row1);
    Task_Audio();

    /* Row 2: volume % + 8-char block bar */
    char vhdr[13];
    snprintf(vhdr, sizeof(vhdr), "Volume: %3d%%", (int)currentVolume);
    LCD_SetCursor(2, 0); LCD_WriteString(vhdr);
    uint8_t filled = playing ? (uint8_t)(currentVolume * 8u / 100u) : 0u;
    for (uint8_t s = 0u; s < 8u; s++)
        LCD_SendData((s < filled) ? 0xFFu : ' ');
    Task_Audio();

    /* Row 3: animated EQ bar graph */
    LCD_SetCursor(3, 0);
    uint8_t noteOctIdx = (uint8_t)(currentOctave * NUM_NOTES + currentNote);
    uint8_t peakCol    = (uint8_t)(1u + noteOctIdx * 18u / (NUM_OCTAVES * NUM_NOTES - 1u));
    uint8_t peakH = 0u;
    if (playing && currentVolume > 0u) {
        peakH = (uint8_t)(currentVolume * 8u / 100u);
        if (peakH == 0u) peakH = 1u;
    }
    for (int col = 0; col < (int)LCD_COLS; col++) {
        int dist = abs(col - (int)peakCol);
        uint8_t target = 0u;
        if      (dist == 0) target = peakH;
        else if (dist == 1) target = (peakH > 2u) ? peakH - 2u : 0u;
        else if (dist == 2) target = (peakH > 4u) ? peakH - 4u : 0u;
        if (playing && target > 0u) {
            int jitter = (rand() % 3) - 1;
            int t = (int)target + jitter;
            target = (t > 0) ? (uint8_t)t : 0u;
        }
        if (barHeights[col] < target)      barHeights[col] = target;
        else if (barHeights[col] > 0u)     barHeights[col]--;
        LCD_SendData((barHeights[col] > 0u && barHeights[col] <= 8u)
                     ? barHeights[col] - 1u : ' ');
        Task_Audio();
    }
}

/* -----------------------------------------------------------------------
 * Guitar String Tuner LCD (20×4)
 * --------------------------------------------------------------------- */
static void Task_LCD_Tuner(void)
{
    static const float  STR_FREQ[6] = { 329.63f, 246.94f, 196.00f,
                                         146.83f, 110.00f,  82.41f };
    static const char * STR_NOTE[6] = { "E4","B3","G3","D3","A2","E2" };

    typedef enum { HINT_NONE=0, HINT_IN_TUNE, HINT_RAISE, HINT_LOWER } HintState_t;
#define HINT_ENTER_CNT   4
#define IN_TUNE_CENTS   30.0f
#define EXIT_TUNE_CENTS 45.0f
#define LOCK_CENTS       3.0f
#define LOCK_MS        500u

    static uint32_t    lastUpdate  = 0u;
    static float       smoothFreq  = 0.0f;
    static int         stableCount = 0;
    static HintState_t hintState   = HINT_NONE;
    static uint8_t     hintPending = 0u;

    static uint8_t  bestStr      = 0u;
    static uint8_t  strCand      = 0u;
    static uint8_t  strCandCnt   = 0u;

    static float    lockRef      = 0.0f;
    static uint32_t lockStart    = 0u;
    static bool     isLocked     = false;

    static float    smooth_cents  = 0.0f;
    static bool     cents_valid   = false;

#define TUNER_ALPHA      0.20f
#define TUNER_STABLE_MIN 2
#define CENTS_EMA_ALPHA  0.20f
#define NEEDLE_DEADZONE  4.5f

    if (HAL_GetTick() - lastUpdate < 200u) return;
    lastUpdate = HAL_GetTick();

    Task_Audio();

    float rawFreq  = MIC_GetFrequency();
    uint8_t sigValid = MIC_IsSignalValid();

    if (sigValid && rawFreq > 50.0f) {
        smoothFreq  = (smoothFreq < 1.0f) ? rawFreq
                      : (TUNER_ALPHA * rawFreq + (1.0f - TUNER_ALPHA) * smoothFreq);
        if (stableCount < 30) stableCount++;
    } else {
        stableCount  = 0;
        smoothFreq   = 0.0f;
        isLocked     = false;
    }
    bool stable = (stableCount >= TUNER_STABLE_MIN);

    if (stable) {
        uint8_t newCand = 0u;
        float   bestDist = 1e9f;
        for (uint8_t s = 0u; s < 6u; s++) {
            float d = fabsf(log2f(smoothFreq / STR_FREQ[s]));
            if (d < bestDist) { bestDist = d; newCand = s; }
        }
        if (newCand == bestStr) {
            strCandCnt = 0u;
        } else if (newCand == strCand) {
            if (++strCandCnt >= 3u) {
                bestStr    = newCand;
                strCandCnt = 0u;
                isLocked   = false;
            }
        } else {
            strCand    = newCand;
            strCandCnt = 1u;
        }
    }

    float cents = stable ? (1200.0f * log2f(smoothFreq / STR_FREQ[bestStr])) : 0.0f;

    if (stable && smoothFreq > 0.0f) {
        if (!cents_valid) {
            smooth_cents = cents;
            cents_valid  = true;
        } else {
            smooth_cents = CENTS_EMA_ALPHA * cents
                         + (1.0f - CENTS_EMA_ALPHA) * smooth_cents;
        }
    } else {
        smooth_cents = 0.0f;
        cents_valid  = false;
    }

    if (stable) {
        if (fabsf(cents - lockRef) <= LOCK_CENTS) {
            if (!isLocked && (HAL_GetTick() - lockStart >= LOCK_MS))
                isLocked = true;
        } else {
            lockRef   = cents;
            lockStart = HAL_GetTick();
            isLocked  = false;
        }
    }

    HintState_t desired = HINT_NONE;
    if (stable) {
        if (hintState == HINT_IN_TUNE)
            desired = (fabsf(cents) <= EXIT_TUNE_CENTS) ? HINT_IN_TUNE
                      : (cents < 0.0f ? HINT_RAISE : HINT_LOWER);
        else
            desired = (fabsf(cents) <= IN_TUNE_CENTS) ? HINT_IN_TUNE
                      : (cents < 0.0f ? HINT_RAISE  : HINT_LOWER);
    }
    if (desired == hintState) {
        hintPending = 0u;
    } else if (++hintPending >= HINT_ENTER_CNT) {
        hintState   = desired;
        hintPending = 0u;
    }

    LCD_SetCursor(0, 0);
    LCD_WriteString("== GUITAR TUNER ==  ");

    Task_Audio();

    char row1[21];
    if (stable)
        snprintf(row1, sizeof(row1), "Note: %-3s   %7.2f Hz ",
                 STR_NOTE[bestStr], smoothFreq);
    else
        snprintf(row1, sizeof(row1), "%-20s", "Play a string...    ");
    LCD_SetCursor(1, 0); LCD_WriteString(row1);

    Task_Audio();

    {
        char needle[21];
        memset(needle, ' ', 20);
        needle[20] = '\0';
        needle[10] = '|';

        if (cents_valid) {
            if (fabsf(smooth_cents) <= NEEDLE_DEADZONE) {
                needle[10] = isLocked ? '*' : '+';
            } else {
                float offset = smooth_cents * 9.5f / 100.0f;
                int npos = 10 + (int)(offset + (offset >= 0.0f ? 0.5f : -0.5f));
                if (npos < 0)  npos = 0;
                if (npos > 19) npos = 19;
                needle[npos] = 'O';
                needle[10]   = '|';
            }
        }
        LCD_SetCursor(2, 0); LCD_WriteString(needle);
    }

    Task_Audio();

    const char *hint;
    if (isLocked)
        hint = " **** LOCKED **** ";
    else {
        switch (hintState) {
            case HINT_IN_TUNE: hint = " **** IN TUNE! **** "; break;
            case HINT_RAISE:   hint = "  >> RAISE UP !! >> "; break;
            case HINT_LOWER:   hint = "  >> LOWER DN !! >> "; break;
            default:           hint = "  -- play string -- "; break;
        }
    }
    LCD_SetCursor(3, 0); LCD_WriteString(hint);
}

/* -----------------------------------------------------------------------
 * Chord LCD (20×4)
 * --------------------------------------------------------------------- */
static void Task_LCD_Chord(void)
{
    static uint32_t lastUpdate = 0u;
    if (HAL_GetTick() - lastUpdate < 200u) return;
    lastUpdate = HAL_GetTick();

    Task_Audio();

    LCD_SetCursor(0, 0);
    LCD_WriteString("   Chord Detector   ");

    Task_Audio();

    ChordResult_t res = CHORD_GetResult();

    if (res.valid) {
        int len   = (int)strlen(res.name);
        int left  = (20 - len) / 2;
        int right = 20 - left - len;
        char row1[21];
        snprintf(row1, sizeof(row1), "%*s%s%*s", left, "", res.name, right, "");
        LCD_SetCursor(1, 0); LCD_WriteString(row1);
    } else {
        LCD_SetCursor(1, 0); LCD_WriteString("                    ");
    }

    Task_Audio();

    LCD_SetCursor(2, 0); LCD_WriteString("                    ");

    Task_Audio();

    LCD_SetCursor(3, 0); LCD_WriteString("  Play a chord...   ");
}

/* -----------------------------------------------------------------------
 * Arpeggiator LCD (20×4)
 * --------------------------------------------------------------------- */
static void Task_LCD_Arpeggio(void)
{
    static const char NL[7] = {'C','D','E','F','G','A','B'};

    static uint32_t lastUpdate = 0u;
    if (HAL_GetTick() - lastUpdate < 200u) return;
    lastUpdate = HAL_GetTick();

    Task_Audio();
    LCD_SetCursor(0, 0); LCD_WriteString("                    ");
    Task_Audio();

    if (arp_state != ARP_IDLE) {
        char row1[21];
        snprintf(row1, sizeof(row1), "  Chord: %-11s", ARP_CHORD_NAMES[arp_chord_idx]);
        LCD_SetCursor(1, 0); LCD_WriteString(row1);
        Task_Audio();
        char row2[21];
        snprintf(row2, sizeof(row2), "  Note:  %c%u          ",
                 NL[currentNote], (unsigned)(BASE_OCTAVE + currentOctave));
        LCD_SetCursor(2, 0); LCD_WriteString(row2);
    } else {
        LCD_SetCursor(1, 0); LCD_WriteString("   Tuning Fork      ");
        Task_Audio();
        LCD_SetCursor(2, 0); LCD_WriteString("   Press 1-7        ");
    }

    Task_Audio();
    LCD_SetCursor(3, 0); LCD_WriteString("                    ");
}

/* -----------------------------------------------------------------------
 * Recorder LCD (20×4)
 * --------------------------------------------------------------------- */
static void Task_LCD_Recorder(void)
{
    static uint32_t lastUpdate = 0u;
    if (HAL_GetTick() - lastUpdate < 100u) return;
    lastUpdate = HAL_GetTick();

    Task_Audio();
    Update_LEDs();

    RecorderState_t st = REC_GetState();

    LCD_SetCursor(0, 0);
    LCD_WriteString("== RECORDER ========");

    Task_Audio();

    char row1[21];
    if (rec_counting) {
        snprintf(row1, sizeof(row1), "Starting in...  [%u] ", (unsigned)rec_countdown);
    } else {
        switch (st) {
            case REC_STATE_IDLE:      snprintf(row1, sizeof(row1), "%-20s", "Status: IDLE        "); break;
            case REC_STATE_RECORDING: snprintf(row1, sizeof(row1), "%-20s", "Status: RECORDING..."); break;
            case REC_STATE_READY:     snprintf(row1, sizeof(row1), "%-20s", "Status: READY       "); break;
            case REC_STATE_PLAYING:   snprintf(row1, sizeof(row1), "%-20s", "Status: PLAYING...  "); break;
            default:                  snprintf(row1, sizeof(row1), "%-20s", "                    "); break;
        }
    }
    LCD_SetCursor(1, 0); LCD_WriteString(row1);

    Task_Audio();

    LCD_SetCursor(2, 0); LCD_WriteString("                    ");

    Task_Audio();

    LCD_SetCursor(3, 0);
    if (rec_counting)
        LCD_WriteString("[*]Cancel           ");
    else
        LCD_WriteString("[*]Record  [0]Play  ");
}

/* -----------------------------------------------------------------------
 * Volume helpers
 * --------------------------------------------------------------------- */
static uint8_t Calculate_Real_Volume(uint8_t displayVol)
{
    if (displayVol == 0u) return 0u;
    uint8_t real = (uint8_t)(45u + displayVol / 2u);
    return (real > 100u) ? 100u : real;
}

static void Apply_Volume(void)
{
    uint8_t codecVol = (appState == APP_STATE_PLAYING)
                       ? Calculate_Real_Volume(currentVolume) : 0u;
    AUDIO_PLAYER_SetVolume(codecVol);
    Update_LEDs();
}

static void Update_LEDs(void)
{
    RecorderState_t rs = REC_GetState();

    bool green = ((appState == APP_STATE_PLAYING) &&
                  (appMode == APP_MODE_GENERATOR || appMode == APP_MODE_TUNER))
                 || (rs == REC_STATE_PLAYING);
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_12, green ? GPIO_PIN_SET : GPIO_PIN_RESET);

    bool red = (appState == APP_STATE_MUTED) || (rs == REC_STATE_RECORDING);
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_14, red ? GPIO_PIN_SET : GPIO_PIN_RESET);

    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_13,
        (appMode == APP_MODE_TUNER   || appMode == APP_MODE_CHORD ||
         appMode == APP_MODE_ARPEGGIO || appMode == APP_MODE_WARMUP)
        ? GPIO_PIN_SET : GPIO_PIN_RESET);

    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_15,
        (appMode == APP_MODE_RECORDER || appMode == APP_MODE_SAMPLER)
        ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* -----------------------------------------------------------------------
 * Task_RecCountdown — ticks once per second, fires REC_StartRecord at 0
 * --------------------------------------------------------------------- */
static void Task_RecCountdown(void)
{
    if (!rec_counting) return;
    if (HAL_GetTick() - rec_count_timer < 1000u) return;

    rec_count_timer = HAL_GetTick();
    rec_countdown--;

    if (rec_countdown == 0u) {
        rec_counting     = false;
        REC_StartRecord();
    }
}

/* -----------------------------------------------------------------------
 * wu_apply_degree — play a note from a transposed scale degree
 * --------------------------------------------------------------------- */
static void wu_apply_degree(uint8_t deg)
{
    if (deg == 255u) return;
    uint8_t total = deg + wu_transpose;
    uint8_t note  = total % 7u;
    uint8_t oct   = 2u + total / 7u;
    if (oct >= NUM_OCTAVES) oct = NUM_OCTAVES - 1u;
    currentNote   = note;
    currentOctave = oct;
    AUDIO_PLAYER_SetSilence(false);
    AUDIO_PLAYER_NoteChange();
}

/* -----------------------------------------------------------------------
 * Task_Warmup — non-blocking FSM
 * --------------------------------------------------------------------- */
static void Task_Warmup(void)
{
    if (wu_state == WU_IDLE) return;

    uint32_t elapsed = HAL_GetTick() - wu_timer;

    if (wu_state == WU_PLAYING) {
        if (elapsed < WU_NOTE_MS) return;
        AUDIO_PLAYER_SetSilence(true);
        wu_state = WU_GAP;
        wu_timer = HAL_GetTick();

    } else if (wu_state == WU_GAP) {
        if (elapsed < WU_GAP_MS) return;
        wu_note_idx++;
        uint8_t next_deg = WARMUP_LIST[wu_seq_idx][wu_note_idx];
        if (next_deg != 255u) {
            wu_apply_degree(next_deg);
            wu_state = WU_PLAYING;
        } else {
            if (wu_has_transpose(wu_seq_idx)) {
                if (wu_trans_dir > 0) {
                    if (wu_transpose < WU_TRANS_MAX) {
                        wu_transpose++;
                    } else {
                        wu_trans_dir = -1;
                        wu_transpose--;
                    }
                    wu_state = WU_TRANS_REST;
                } else {
                    if (wu_transpose > 0u) {
                        wu_transpose--;
                        wu_state = WU_TRANS_REST;
                    } else {
                        wu_state = WU_REST;
                    }
                }
            } else {
                wu_state = WU_REST;
            }
        }
        wu_timer = HAL_GetTick();

    } else if (wu_state == WU_TRANS_REST) {
        if (elapsed < WU_TRANS_REST_MS) return;
        wu_note_idx = 0u;
        wu_apply_degree(WARMUP_LIST[wu_seq_idx][0]);
        wu_state = WU_PLAYING;
        wu_timer = HAL_GetTick();

    } else if (wu_state == WU_REST) {
        if (elapsed < WU_REST_MS) return;
        wu_state     = WU_IDLE;
        wu_transpose = 0u;
        wu_trans_dir = 1;
    }
}

/* -----------------------------------------------------------------------
 * Vocal Warm-Up LCD (20x4)
 * --------------------------------------------------------------------- */
static void Task_LCD_Warmup(void)
{
    static const char * const SOLFEGE[8] = {
        "Ut","Re","Mi","Fa","Sol","La","Si","Ut'"
    };

    static uint32_t lastUpdate = 0u;
    if (HAL_GetTick() - lastUpdate < 200u) return;
    lastUpdate = HAL_GetTick();

    Task_Audio();
    LCD_SetCursor(0, 0);
    LCD_WriteString("  Vocal Warm-Up     ");

    Task_Audio();

    char row1[21];
    if (wu_state != WU_IDLE)
        snprintf(row1, sizeof(row1), "%-20s", WARMUP_NAMES[wu_seq_idx]);
    else
        snprintf(row1, sizeof(row1), "%-20s", "Press 1-7 to start  ");
    LCD_SetCursor(1, 0); LCD_WriteString(row1);

    Task_Audio();

    char row2[21];
    if (wu_state == WU_PLAYING || wu_state == WU_GAP) {
        uint8_t raw = WARMUP_LIST[wu_seq_idx][wu_note_idx];
        uint8_t si  = (raw <= 7u) ? raw : 7u;
        if (wu_has_transpose(wu_seq_idx) && wu_transpose > 0u)
            snprintf(row2, sizeof(row2), "%-5s [%2u]  +%u         ",
                     SOLFEGE[si], (unsigned)(wu_note_idx + 1u),
                     (unsigned)wu_transpose);
        else
            snprintf(row2, sizeof(row2), "%-5s [%2u]             ",
                     SOLFEGE[si], (unsigned)(wu_note_idx + 1u));
    } else if (wu_state == WU_TRANS_REST) {
        snprintf(row2, sizeof(row2), "(transposing +%u)    ",
                 (unsigned)wu_transpose);
    } else if (wu_state == WU_REST) {
        snprintf(row2, sizeof(row2), "%-20s", "(rest...)           ");
    } else {
        snprintf(row2, sizeof(row2), "%-20s", "                    ");
    }
    LCD_SetCursor(2, 0); LCD_WriteString(row2);

    Task_Audio();
    LCD_SetCursor(3, 0);
    LCD_WriteString("1-7: sequence       ");
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
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
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

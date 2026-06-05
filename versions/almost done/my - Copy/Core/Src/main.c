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
#include "pdm2pcm_glo.h"
#include "usb_host.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "waveplayer.h"
#include "AUDIO.h"
#include "LCD1602.h"
#include "microphone.h"
#include "recorder.h"
#include "chord.h"
#include "flash_recorder.h"
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
CRC_HandleTypeDef hcrc;

I2C_HandleTypeDef hi2c1;

I2S_HandleTypeDef hi2s3;
DMA_HandleTypeDef hdma_spi3_tx;

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
static void MX_I2C1_Init(void);
static void MX_I2S3_Init(void);
static void MX_CRC_Init(void);
void MX_USB_HOST_Process(void);

/* USER CODE BEGIN PFP */
static void Keypad_GPIO_Init(void);
static char Keypad_Scan(void);
static void Handle_Key(char key);
static void Task_Keypad(void);
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
  MX_I2C1_Init();
  MX_I2S3_Init();
  MX_FATFS_Init();
  MX_USB_HOST_Init();
  MX_CRC_Init();
  MX_PDM2PCM_Init();
  /* USER CODE BEGIN 2 */
  Keypad_GPIO_Init();

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
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */
    MX_USB_HOST_Process();

    /* USER CODE BEGIN 3 */
    Task_Audio();
    Task_Keypad();
    Task_Arpeggiator();
    Task_Strum();
    Task_RecCountdown();
    Task_Warmup();
    MIC_Process();
    CHORD_Process();
    switch (appMode) {
        case APP_MODE_TUNER:    Task_LCD_Tuner();    break;
        case APP_MODE_CHORD:    Task_LCD_Chord();    break;
        case APP_MODE_ARPEGGIO: Task_LCD_Arpeggio(); break;
        case APP_MODE_WARMUP:   Task_LCD_Warmup();   break;
        case APP_MODE_RECORDER: Task_LCD_Recorder(); break;
        default:                Task_LCD();          break;
    }
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
  __HAL_CRC_DR_RESET(&hcrc);
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
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Stream5_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Stream5_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream5_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_0, GPIO_PIN_RESET);

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

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI0_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI0_IRQn);

}

/* USER CODE BEGIN 4 */

/* -----------------------------------------------------------------------
 * Keypad GPIO — PE8-PE11 rows (out), PE12-PE15 cols (in, pull-up)
 * --------------------------------------------------------------------- */
static const uint16_t KP_ROWS[4] = { GPIO_PIN_8, GPIO_PIN_9, GPIO_PIN_10, GPIO_PIN_11 };
static const uint16_t KP_COLS[4] = { GPIO_PIN_12, GPIO_PIN_13, GPIO_PIN_14, GPIO_PIN_15 };
static const char KP_MAP[4][4]   = {
    { '1','2','3','A' }, { '4','5','6','B' },
    { '7','8','9','C' }, { '*','0','#','D' }
};

static void Keypad_GPIO_Init(void)
{
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();

    /* LEDs: PD12 green, PD13 orange, PD14 red, PD15 blue */
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_12|GPIO_PIN_13|GPIO_PIN_14|GPIO_PIN_15, GPIO_PIN_RESET);
    g.Pin = GPIO_PIN_12|GPIO_PIN_13|GPIO_PIN_14|GPIO_PIN_15;
    g.Mode = GPIO_MODE_OUTPUT_PP; g.Pull = GPIO_NOPULL; g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOD, &g);

    /* Keypad rows — output, start HIGH (idle) */
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_11, GPIO_PIN_SET);
    g.Pin  = GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_11;
    g.Mode = GPIO_MODE_OUTPUT_PP; g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOE, &g);

    /* Keypad cols — input, pull-up */
    g.Pin  = GPIO_PIN_12|GPIO_PIN_13|GPIO_PIN_14|GPIO_PIN_15;
    g.Mode = GPIO_MODE_INPUT; g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOE, &g);
}

static char Keypad_Scan(void)
{
    for (uint8_t r = 0u; r < 4u; r++) {
        for (uint8_t i = 0u; i < 4u; i++) HAL_GPIO_WritePin(GPIOE, KP_ROWS[i], GPIO_PIN_SET);
        HAL_GPIO_WritePin(GPIOE, KP_ROWS[r], GPIO_PIN_RESET);
        for (volatile uint32_t d = 0u; d < 200u; d++) {}
        for (uint8_t c = 0u; c < 4u; c++) {
            if (HAL_GPIO_ReadPin(GPIOE, KP_COLS[c]) == GPIO_PIN_RESET) {
                for (uint8_t i = 0u; i < 4u; i++) HAL_GPIO_WritePin(GPIOE, KP_ROWS[i], GPIO_PIN_SET);
                return KP_MAP[r][c];
            }
        }
    }
    return 0;
}

/*
 * Keypad — all 16 keys assigned, same in every mode unless noted
 * ┌────┬────┬────┬────┐
 * │ 1  │ 2  │ 3  │ A  │   1-7  notes Ut Re Mi Fa Sol La Si
 * │ Ut │ Re │ Mi │Vol+│   8    octave cycle  2→3→4→5→2
 * ├────┼────┼────┼────┤   9    mute toggle
 * │ 4  │ 5  │ 6  │ B  │   A    volume +5 %
 * │ Fa │Sol │ La │Vol-│   B    volume -5 %
 * ├────┼────┼────┼────┤   C    preset cycle (Sine/Sqr/Saw/Clar/Organ)
 * │ 7  │ 8  │ 9  │ C  │   D    mode cycle  Gen→Tuner→Chord→Recorder
 * │ Si │Oct │Mut │Prs │
 * ├────┼────┼────┼────┤   Bottom row (* 0 # ) = Recorder controls
 * │ *  │ 0  │ #  │ D  │   *    Record toggle (start / stop recording)
 * │Rec │Play│Stop│Mode│   0    Play  toggle (start / stop playback)
 * └────┴────┴────┴────┘   #    Stop  all    (abort record or play)
 */
static void Handle_Key(char key)
{
    switch (key) {
        /* ── Notes 1-7 ─────────────────────────────────────────── */
        case '1': case '2': case '3': case '4':
        case '5': case '6': case '7':
            if (appMode == APP_MODE_ARPEGGIO) {
                /* Arpeggio mode: start non-blocking diatonic arpeggio (800 ms/note) */
                arp_chord_idx = (uint8_t)(key - '1');
                arp_note_idx  = 0u;
                arp_rep       = 0u;
                arp_state     = ARP_PLAYING;
                currentNote   = ARP_CHORDS[arp_chord_idx][0u].note;
                currentOctave = ARP_CHORDS[arp_chord_idx][0u].oct;
                AUDIO_PLAYER_SetSilence(false);
                AUDIO_PLAYER_NoteChange();
                arp_timer = HAL_GetTick();
            } else if (appMode == APP_MODE_WARMUP) {
                /* Warmup mode: keys 1-7 trigger one of the 7 sequences */
                wu_seq_idx   = (uint8_t)(key - '1');
                wu_note_idx  = 0u;
                wu_transpose = 0u;
                wu_trans_dir = 1;
                wu_state     = WU_PLAYING;
                wu_apply_degree(WARMUP_LIST[wu_seq_idx][0]);
                wu_timer = HAL_GetTick();
            } else {
                currentNote = (uint8_t)(key - '1');
                for (int i = 0; i < (int)LCD_COLS; i++) barHeights[i] = 0u;
                AUDIO_PLAYER_NoteChange();
            }
            break;

        /* ── 8: octave cycle UP ──────────────────────────────────── */
        case '8':
            currentOctave = (currentOctave + 1u) % NUM_OCTAVES;
            for (int i = 0; i < (int)LCD_COLS; i++) barHeights[i] = 0u;
            AUDIO_PLAYER_NoteChange();
            break;

        /* ── 9: mute toggle ─────────────────────────────────────── */
        case '9':
            appState = (appState == APP_STATE_PLAYING) ? APP_STATE_MUTED : APP_STATE_PLAYING;
            Apply_Volume();
            break;

        /* ── A: volume +5 % ─────────────────────────────────────── */
        case 'A':
            if (currentVolume <= 95u) currentVolume += 5u;
            Apply_Volume();
            break;

        /* ── B: volume -5 % ─────────────────────────────────────── */
        case 'B':
            if (currentVolume >= 5u) currentVolume -= 5u;
            Apply_Volume();
            break;

        /* ── C: preset cycle (or erase all flash recordings in recorder mode) */
        case 'C':
            currentPreset = (currentPreset + 1u) % NUM_PRESETS;
            AUDIO_PLAYER_SetPreset(currentPreset);
            AUDIO_PLAYER_NoteChange();
            break;

        /* ── *: Record toggle (recorder mode: 3-s countdown first) ── */
        case '*':
            if (rec_counting) {
                /* Cancel the pending countdown */
                rec_counting = false;
            } else if (REC_GetState() == REC_STATE_RECORDING) {
                REC_StopRecord();
                /* Auto-save to flash when in recorder mode and flash has a free slot */
                if (appMode == APP_MODE_RECORDER &&
                    REC_GetSamplesRecorded() > 0u &&
                    !FLASH_REC_IsFull()) {
                    /* Mute audio during blocking flash erase + write (~1-2 s) */
                    AUDIO_PLAYER_SetVolume(0u);
                    FLASH_REC_SaveFromRecorder();
                    Apply_Volume();

                }
            } else {
                if (appMode == APP_MODE_RECORDER) {
                    /* Start 3-second countdown then begin capture */
                    rec_counting    = true;
                    rec_countdown   = 3u;
                    rec_count_timer = HAL_GetTick();
                } else {
                    REC_StartRecord();
                }
            }
            break;

        /* ── 0: Play toggle ─────────────────────────────────────── */
        case '0':
            if (REC_GetState() == REC_STATE_PLAYING) {
                REC_StopPlay();
            } else if (appMode == APP_MODE_RECORDER) {
                if (REC_GetSamplesRecorded() == 0u) break;  /* nothing recorded */
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

        /* ── #: Stop all (abort record or playback) ─────────────── */
        case '#':
            REC_StopRecord();
            REC_StopPlay();
            break;
        case 'D': {
            AppMode_t prev = appMode;
            appMode = (AppMode_t)((appMode + 1u) % APP_MODE_COUNT);
            LCD_Clear();
            if (prev == APP_MODE_RECORDER) {
                rec_counting = false;   /* cancel any pending countdown */
                REC_StopRecord();
                REC_StopPlay();
            }
            /* Stop arpeggiator when leaving arpeggio mode */
            if (prev == APP_MODE_ARPEGGIO) {
                arp_state = ARP_IDLE;
            }
            /* Stop chord strum when leaving chord mode */
            if (prev == APP_MODE_CHORD) {
                strum_state = STRUM_IDLE;
            }
            /* Stop warmup sequence when leaving warmup mode */
            if (prev == APP_MODE_WARMUP) {
                wu_state     = WU_IDLE;
                wu_transpose = 0u;
                wu_trans_dir = 1;
                AUDIO_PLAYER_SetSilence(true);
            }
            /* Reset chord detector when entering chord mode — prevents stale display */
            if (appMode == APP_MODE_CHORD) {
                CHORD_Init();
            }
            AUDIO_PLAYER_SetSilence(appMode != APP_MODE_GENERATOR);
            Update_LEDs();
            break;
        }
        default: break;
    }
}

static void Task_Keypad(void)
{
    static uint32_t lastScan  = 0u;
    static char     prevKey   = 0;
    static char     confirmed = 0;
    if (HAL_GetTick() - lastScan < 20u) return;
    lastScan = HAL_GetTick();
    char key = Keypad_Scan();
    if (key != 0 && key == prevKey && key != confirmed) { confirmed = key; Handle_Key(key); }
    if (key == 0) confirmed = 0;
    prevKey = key;
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
 *
 * Detects which of the 6 standard guitar strings is being played and
 * shows how far it is from the correct pitch.
 *
 * Standard tuning (thinnest → thickest):
 *   String 1: E4  329.63 Hz
 *   String 2: B3  246.94 Hz
 *   String 3: G3  196.00 Hz
 *   String 4: D3  146.83 Hz
 *   String 5: A2  110.00 Hz
 *   String 6: E2   82.41 Hz
 *
 * Row 0: "== GUITAR TUNER ==  "
 * Row 1: "Str 1: E4  329.6 Hz "  — closest string + measured freq
 * Row 2: "Dev:  -14 cents     "  — deviation from that string's target
 * Row 3: " **** IN TUNE! **** "  — hint (changes slowly via state machine)
 *         "  >> RAISE UP !! >> "
 *         "  >> LOWER DN !! >> "
 *
 * Hint state machine (hysteresis)
 * --------------------------------
 * The displayed hint only changes after HINT_ENTER_CNT consecutive frames
 * (each 350 ms) agree on the new state.  This prevents the display from
 * flickering between "IN TUNE / RAISE / LOWER" as the note decays.
 * In-tune zone: ±30 cents.  Must leave ±45 cents to exit that zone.
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

    /* Sticky string selection — requires 3 consecutive frames to switch */
    static uint8_t  bestStr      = 0u;
    static uint8_t  strCand      = 0u;
    static uint8_t  strCandCnt   = 0u;

    /* Visual lock-on: ±3 cents held for 500 ms */
    static float    lockRef      = 0.0f;
    static uint32_t lockStart    = 0u;
    static bool     isLocked     = false;

    /* EMA-smoothed cents for the needle display.
     * smoothFreq already low-passes the raw frequency; smooth_cents adds a
     * second EMA pass on the derived deviation.  Together they eliminate the
     * high-frequency jitter that comes from frame-to-frame detection noise. */
    static float    smooth_cents  = 0.0f;
    static bool     cents_valid   = false;

#define TUNER_ALPHA      0.20f
#define TUNER_STABLE_MIN 2
/* Needle display parameters */
#define CENTS_EMA_ALPHA  0.20f   /* smoothing factor: lower = smoother, more lag */
#define NEEDLE_DEADZONE  4.5f    /* ±cents within which the centre '+' is shown  */

    if (HAL_GetTick() - lastUpdate < 200u) return;
    lastUpdate = HAL_GetTick();

    Task_Audio();

    float rawFreq  = MIC_GetFrequency();
    bool  sigValid = MIC_IsSignalValid();

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

    /* ── Sticky string selection ────────────────────────────────── */
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

    /* ── EMA smoothing on cents for jitter-free needle display ─────
     * Exponential Moving Average: new = α·raw + (1-α)·prev
     * α = CENTS_EMA_ALPHA (0.20) gives a time constant of ~5 frames
     * = ~1 s at 200 ms/frame, which is visually very stable.
     * Reset to raw on signal loss so the needle snaps home cleanly. */
    if (stable && smoothFreq > 0.0f) {
        if (!cents_valid) {
            smooth_cents = cents;   /* seed with first valid reading */
            cents_valid  = true;
        } else {
            smooth_cents = CENTS_EMA_ALPHA * cents
                         + (1.0f - CENTS_EMA_ALPHA) * smooth_cents;
        }
    } else {
        smooth_cents = 0.0f;
        cents_valid  = false;
    }

    /* ── Visual lock-on: ±3 cents for 500 ms ────────────────────── */
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

    /* ── Hint state machine ─────────────────────────────────────── */
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

    /* ── Row 0 ───────────────────────────────────────────────────── */
    LCD_SetCursor(0, 0);
    LCD_WriteString("== GUITAR TUNER ==  ");

    Task_Audio();

    /* ── Row 1: string name (letter only) + frequency (no Hz) ───── */
    char row1[21];
    if (stable)
        snprintf(row1, sizeof(row1), "Note: %-3s   %7.2f Hz ",
                 STR_NOTE[bestStr], smoothFreq);
    else
        snprintf(row1, sizeof(row1), "%-20s", "Play a string...    ");
    LCD_SetCursor(1, 0); LCD_WriteString(row1);

    Task_Audio();

    /* ── Row 2: graphical needle meter (EMA-smoothed) ───────────────
     * Uses smooth_cents (double-EMA filtered) to eliminate jitter.
     *
     * Layout (20 chars, 0-based):
     *   Position 10 = '|'  — perfect pitch reference tick
     *   Position N  = 'O'  — needle, driven by smooth_cents
     *   Dead-zone ±NEEDLE_DEADZONE cents → show '+' (or '*' if locked)
     *   at position 10 instead of 'O', preventing the centre marker
     *   from flickering when the string is nearly in tune.
     *
     *   Scale: 9.5 positions per 100 cents (±100 cents = ±9.5 columns)
     * ─────────────────────────────────────────────────────────── */
    {
        char needle[21];
        memset(needle, ' ', 20);
        needle[20] = '\0';
        needle[10] = '|';   /* permanent centre reference tick */

        if (cents_valid) {
            if (fabsf(smooth_cents) <= NEEDLE_DEADZONE) {
                /* Dead-zone: show a stable centre character */
                needle[10] = isLocked ? '*' : '+';
            } else {
                /* Map smooth_cents to a needle column (0-19) */
                float offset = smooth_cents * 9.5f / 100.0f;
                int npos = 10 + (int)(offset + (offset >= 0.0f ? 0.5f : -0.5f));
                if (npos < 0)  npos = 0;
                if (npos > 19) npos = 19;
                needle[npos] = 'O';
                needle[10]   = '|';   /* keep centre tick visible */
            }
        }
        LCD_SetCursor(2, 0); LCD_WriteString(needle);
    }

    Task_Audio();

    /* ── Row 3: locked indicator overrides hint ──────────────────── */
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
 *
 * Idle/detecting:
 *   Row 0: "   Chord Detector   "
 *   Row 1:  <chord name centred, blank until first valid detection>
 *   Row 2:  blank
 *   Row 3:  blank
 *
 * Arpeggiator active:
 *   Row 0: "   Chord Detector   "
 *   Row 1:  "  Arp: Em   [1/2]  "
 *   Row 2:  "  Now: E4          "
 *   Row 3:   blank
 * --------------------------------------------------------------------- */
static void Task_LCD_Chord(void)
{
    static uint32_t lastUpdate = 0u;
    if (HAL_GetTick() - lastUpdate < 200u) return;
    lastUpdate = HAL_GetTick();

    Task_Audio();

    /* Row 0: static header */
    LCD_SetCursor(0, 0);
    LCD_WriteString("   Chord Detector   ");

    Task_Audio();

    ChordResult_t res = CHORD_GetResult();

    /* Row 1: detected chord name centred */
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

    /* Row 2: blank */
    LCD_SetCursor(2, 0); LCD_WriteString("                    ");

    Task_Audio();

    /* Row 3: usage hint */
    LCD_SetCursor(3, 0); LCD_WriteString("  Play a chord...   ");
}

/* -----------------------------------------------------------------------
 * Arpeggiator LCD (20×4) — dedicated mode, keys 1-7 trigger diatonic arps
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
 *
 * Row 0: "== RECORDER ========"
 * Row 1: "Status: RECORDING..."   or IDLE / READY / PLAYING
 * Row 2: "Len: 1.5s /  2.0s  "   (recorded / max) or playback position
 * Row 3: "[0]Rec [*]Play [D]>>"
 * --------------------------------------------------------------------- */
static void Task_LCD_Recorder(void)
{
    static uint32_t lastUpdate = 0u;
    if (HAL_GetTick() - lastUpdate < 100u) return;
    lastUpdate = HAL_GetTick();

    Task_Audio();
    Update_LEDs();   /* keep LED colour consistent with recorder state */

    RecorderState_t st = REC_GetState();

    /* Row 0 */
    LCD_SetCursor(0, 0);
    LCD_WriteString("== RECORDER ========");

    Task_Audio();

    /* Row 1: status */
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

    /* Row 3: keypad hint */
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

    /* Green: synthesis playing OR recorder playback */
    bool green = ((appState == APP_STATE_PLAYING) &&
                  (appMode == APP_MODE_GENERATOR || appMode == APP_MODE_TUNER))
                 || (rs == REC_STATE_PLAYING);
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_12, green ? GPIO_PIN_SET : GPIO_PIN_RESET);

    /* Red: muted OR actively recording */
    bool red = (appState == APP_STATE_MUTED) || (rs == REC_STATE_RECORDING);
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_14, red ? GPIO_PIN_SET : GPIO_PIN_RESET);

    /* Orange: tuner, chord, arpeggio, or warmup mode */
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_13,
        (appMode == APP_MODE_TUNER   || appMode == APP_MODE_CHORD ||
         appMode == APP_MODE_ARPEGGIO || appMode == APP_MODE_WARMUP)
        ? GPIO_PIN_SET : GPIO_PIN_RESET);

    /* Blue: recorder mode */
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_15,
        (appMode == APP_MODE_RECORDER) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* -----------------------------------------------------------------------
 * Task_RecCountdown — ticks once per second, fires REC_StartRecord at 0
 * Called from main loop; does nothing when rec_counting == false.
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
 *
 * total = deg + wu_transpose
 * note  = total % 7   (0=C..6=B in the 7-note diatonic scale)
 * oct   = 2 + total/7 (index 2=C4, 3=C5, 4=C6)
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
 *
 * All sequences:   WU_PLAYING 600ms -> WU_GAP 100ms -> next note or end
 *                  end -> WU_REST 800ms -> WU_IDLE
 *
 * Sequences 3/4/5 also loop with transposition:
 *   end -> WU_TRANS_REST 500ms -> repeat at +1 step, up to +3, then back
 *   route: offset 0->1->2->3->2->1->0, then WU_REST
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
            /* Sequence finished */
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
 *
 * Row 0: "  Vocal Warm-Up     "
 * Row 1: sequence name  or  "Press 1-7 to start  "
 * Row 2: current note + transpose indicator while playing
 * Row 3: "1-7: sequence       "
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
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
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


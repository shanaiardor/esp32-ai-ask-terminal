#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/i2s_std.h"
#include "esp_adc/adc_oneshot.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Physical Display (AXS15231B QSPI 172x640) */
#define LCD_HOST                SPI3_HOST
#define LCD_PHYS_W              172
#define LCD_PHYS_H              640
#define LCD_DMA_LINES           64
#define LCD_DMA_BUF_BYTES       (LCD_PHYS_W * LCD_DMA_LINES * 2)
#define LCD_FULL_BUF_BYTES      (LCD_PHYS_W * LCD_PHYS_H * 2)

#define PIN_LCD_CS              GPIO_NUM_9
#define PIN_LCD_PCLK            GPIO_NUM_10
#define PIN_LCD_DATA0           GPIO_NUM_11
#define PIN_LCD_DATA1           GPIO_NUM_12
#define PIN_LCD_DATA2           GPIO_NUM_13
#define PIN_LCD_DATA3           GPIO_NUM_14
#define PIN_LCD_TE              GPIO_NUM_21
#define PIN_LCD_BL_PWM          GPIO_NUM_42

/* System I2C Bus (I2C_NUM_0): TCA9554, PCF85063, QMI8658, ES8311 */
#define SYS_I2C_PORT            I2C_NUM_0
#define PIN_SYS_I2C_SCL         GPIO_NUM_48
#define PIN_SYS_I2C_SDA         GPIO_NUM_47
#define I2C_ADDR_TCA9554        0x20
#define I2C_ADDR_PCF85063       0x51
#define I2C_ADDR_QMI8658        0x6B
#define I2C_ADDR_ES8311         0x18

/* Touch I2C Bus (I2C_NUM_1): AXS15231B Touch */
#define TOUCH_I2C_PORT          I2C_NUM_1
#define PIN_TOUCH_I2C_SCL       GPIO_NUM_18
#define PIN_TOUCH_I2C_SDA       GPIO_NUM_17
#define I2C_ADDR_TOUCH          0x3B

/* Audio I2S Bus (ES8311 DAC + ES7210 ADC) */
#define AUDIO_I2S_PORT          I2S_NUM_0
#define PIN_I2S_MCLK            GPIO_NUM_7
#define PIN_I2S_BCLK            GPIO_NUM_15
#define PIN_I2S_WS              GPIO_NUM_46
#define PIN_I2S_DOUT            GPIO_NUM_45
#define PIN_I2S_DIN             GPIO_NUM_6
#define AUDIO_SAMPLE_RATE       24000

/* TCA9554 I/O Expander Pin Masks (V2 / Rev1.1) */
#define EXIO_PIN_TOUCH_INT      (1U << 0)
#define EXIO_PIN_BL_EN          (1U << 1)
#define EXIO_PIN_IMU_INT1       (1U << 2)
#define EXIO_PIN_IMU_INT2       (1U << 3)
#define EXIO_PIN_RTC_INT        (1U << 4)
#define EXIO_PIN_LCD_RST        (1U << 5)
#define EXIO_PIN_SYS_EN         (1U << 6)
#define EXIO_PIN_NS_MODE        (1U << 7)

/* Buttons, Interrupts, Battery ADC */
#define PIN_BOOT_BTN            GPIO_NUM_0
#define PIN_PWR_BTN             GPIO_NUM_16
#define PIN_EXIO_INT            GPIO_NUM_8
#define BAT_ADC_UNIT            ADC_UNIT_1
#define BAT_ADC_CHANNEL         ADC_CHANNEL_3

/* Display Rotation & Orientation Mode */
typedef enum {
    DISP_ROT_0 = 0,     /* Portrait 172x640 */
    DISP_ROT_90 = 1,    /* Landscape 640x172 */
    DISP_ROT_180 = 2,   /* Portrait inverted 172x640 */
    DISP_ROT_270 = 3    /* Landscape inverted 640x172 */
} disp_rot_t;

typedef enum {
    ROT_MODE_AUTO_4WAY = 0,
    ROT_MODE_LANDSCAPE_ONLY = 1,
    ROT_MODE_LOCKED = 2
} rot_mode_t;

#ifdef __cplusplus
}
#endif

#endif /* BOARD_CONFIG_H */

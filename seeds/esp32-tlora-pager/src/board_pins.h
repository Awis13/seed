// LILYGO T-Lora Pager pin map — verified against
// framework-arduinoespressif32/variants/lilygo_tlora_pager/pins_arduino.h
// and Xinyuan-LilyGO/LilyGoLib docs/hardware/lilygo-t-lora-pager.md (2026-08-07).
// Do not invent numbers; change only with a citation.

#pragma once

// I2C bus (shared: XL9555, TCA8418, BQ*, ES8311, RTC, BHI260, DRV2605)
#define PIN_I2C_SDA         3
#define PIN_I2C_SCL         2

// Fuel gauge (same part as T-Embed; LilyGo 1500 mAh pack)
#define BQ27220_ADDR        0x55

// Shared SPI (display, LoRa, SD, NFC) — park every CS HIGH before first byte
#define PIN_SPI_MOSI        34
#define PIN_SPI_MISO        33
#define PIN_SPI_SCK         35

#define PIN_SD_CS           21
#define PIN_LORA_CS         36
#define PIN_LORA_RST        47
#define PIN_LORA_BUSY       48
#define PIN_LORA_IRQ        14
#define PIN_NFC_CS          39
#define PIN_NFC_INT         5

// ST7796U SPI panel 480x222 (controller 320x480 with 49px gap)
#define PIN_DISP_CS         38
#define PIN_DISP_DC         37
#define PIN_DISP_RST        (-1)   // not connected
#define PIN_DISP_BL         42     // AW9364 one-wire brightness (0..16)

// ES8311 I2S (pins_arduino.h)
#define PIN_I2S_WS          18
#define PIN_I2S_SCK         11
#define PIN_I2S_MCLK        10
#define PIN_I2S_SDOUT       45
#define PIN_I2S_SDIN        17

// Inputs
#define PIN_KB_INT          6
#define PIN_KB_BACKLIGHT    46
#define PIN_ROTARY_A        40
#define PIN_ROTARY_B        41
#define PIN_ROTARY_C        7
#define PIN_RTC_INT         1
#define PIN_SENSOR_INT      8

// GNSS u-blox MIA-M10Q — UART 38400 8N1 (factory default), PPS strobe.
// Verified against TLora-Pager schematic + MIA-M10Q bring-up notes (2026-08-09).
#define PIN_GPS_RX          4      // ESP32 RX <- module TX
#define PIN_GPS_TX          12     // ESP32 TX -> module RX
#define PIN_GPS_PPS         13     // 1 Hz fix strobe (not driven yet)

// XL9555 @ 0x20 — expander GPIO indices (not ESP pins)
#define XL9555_ADDR         0x20
#define EXP_DRV_EN          0
#define EXP_AMP_EN          1
#define EXP_KB_RST          2
#define EXP_LORA_EN         3
#define EXP_GPS_EN          4
#define EXP_NFC_EN          5
// 6 = display RST on some docs, NC on this board
#define EXP_GPS_RST         7
#define EXP_KB_EN           8
#define EXP_GPIO_EN         9
#define EXP_SD_DET          10
#define EXP_SD_PULLEN       11
#define EXP_SD_EN           12

// Panel geometry after LilyGo rotation 0 (landscape, keyboard below)
#define DISP_NATIVE_W       222
#define DISP_NATIVE_H       480
#define DISP_LANDSCAPE_W    480
#define DISP_LANDSCAPE_H    222
#define DISP_GAP_Y          49     // CGRAM row offset in landscape

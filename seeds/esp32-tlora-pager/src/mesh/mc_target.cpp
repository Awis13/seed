// SX1262 bring-up for seed. Shares display SPI (FSPI). Park all CS high first.
#include "mc_target.h"
#include "board_pins.h"
#include "hw_ui.h"

#include <RadioLib.h>
#include <SPI.h>
#include <Wire.h>
#include <helpers/ESP32Board.h>
#include <helpers/radiolib/CustomSX1262.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <new>

#ifndef P_LORA_NSS
#define P_LORA_NSS   PIN_LORA_CS
#define P_LORA_DIO_1 PIN_LORA_IRQ
#define P_LORA_RESET PIN_LORA_RST
#define P_LORA_BUSY  PIN_LORA_BUSY
#define P_LORA_SCLK  PIN_SPI_SCK
#define P_LORA_MISO  PIN_SPI_MISO
#define P_LORA_MOSI  PIN_SPI_MOSI
#endif

class SeedTLoraBoard : public ESP32Board {
public:
  void begin();
  uint16_t getBattMilliVolts() override { return 0; }
  const char *getManufacturerName() const override { return "LilyGo"; }
};

static SeedTLoraBoard board;
static Module *s_mod = nullptr;
static uint8_t s_radio_mem[sizeof(CustomSX1262)];
static uint8_t s_driver_mem[sizeof(CustomSX1262Wrapper)];
static bool s_ready = false;

VolatileRTCClock fallback_clock;
ArduinoMillis mesh_ms_clock;
StdRNG mesh_rng;

void SeedTLoraBoard::begin() {
  startup_reason = BD_STARTUP_NORMAL;
  // LORA_EN via XL9555 pin 3 (seed UI already powers rails; re-assert).
  Wire.beginTransmission(XL9555_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) == 0 && Wire.requestFrom((int)XL9555_ADDR, 1) == 1) {
    uint8_t o0 = Wire.read();
    o0 |= (1u << 3);
    Wire.beginTransmission(XL9555_ADDR);
    Wire.write(0x02);
    Wire.write(o0);
    Wire.endTransmission();
  }
  delay(15);
  pinMode(PIN_DISP_CS, OUTPUT);
  digitalWrite(PIN_DISP_CS, HIGH);
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);
  pinMode(PIN_NFC_CS, OUTPUT);
  digitalWrite(PIN_NFC_CS, HIGH);
  pinMode(PIN_LORA_CS, OUTPUT);
  digitalWrite(PIN_LORA_CS, HIGH);
  pinMode(PIN_LORA_RST, OUTPUT);
  digitalWrite(PIN_LORA_RST, HIGH);
}

bool mesh_radio_init() {
  SPIClass *spi = hw_ui_spi();
  if (!spi) {
    Serial.println("[mesh] no shared SPI (UI not ready)");
    return false;
  }
  board.begin();
  digitalWrite(PIN_DISP_CS, HIGH);
  digitalWrite(PIN_LORA_CS, HIGH);

  s_mod = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, *spi);
  auto *radio = new (s_radio_mem) CustomSX1262(s_mod);
  auto *drv = new (s_driver_mem) CustomSX1262Wrapper(*radio, board);

  bool ok = radio->std_init(spi);
  if (!ok) {
    Serial.println("[mesh] SX1262 std_init FAILED");
    return false;
  }
  s_ready = true;
  (void)drv;
  Serial.println("[mesh] SX1262 OK (shared SPI)");
  return true;
}

mesh::Radio &mesh_get_radio() {
  return *reinterpret_cast<CustomSX1262Wrapper *>(s_driver_mem);
}

void mesh_radio_set_params(float freq, float bw, uint8_t sf, uint8_t cr) {
  if (!s_ready) return;
  reinterpret_cast<CustomSX1262Wrapper *>(s_driver_mem)->setParams(freq, bw, sf, cr);
}

void mesh_radio_set_tx_power(int8_t dbm) {
  if (!s_ready) return;
  reinterpret_cast<CustomSX1262Wrapper *>(s_driver_mem)->setTxPower(dbm);
}

uint32_t mesh_radio_rng_seed() {
  if (!s_ready) return (uint32_t)millis();
  return reinterpret_cast<CustomSX1262 *>(s_radio_mem)->random(0x7FFFFFFF);
}

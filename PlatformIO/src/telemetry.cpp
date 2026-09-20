#include "telemetry.h"

#include <Arduino.h>
#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace ecu {

TelemetryStore g_telemetry;

// Link is considered live if a frame decoded within this window.
static constexpr uint32_t LINK_TIMEOUT_MS = 1000;

static const ChannelMeta kMeta[CH_COUNT] = {
    {"rpm",       "Engine Speed",     "rpm",     0,    8000},
    {"map",       "Manifold Press",   "kPa",     0,    250},
    {"tps",       "Throttle",         "%",       0,    100},
    {"clt",       "Coolant Temp",     "\xC2\xB0""C", -40, 130},
    {"iat",       "Intake Air Temp",  "\xC2\xB0""C", -40, 100},
    {"lambda",    "Lambda",           "\xCE\xBB", 0.6f, 1.4f},
    {"batt",      "Battery",          "V",       8,    16},
    {"inj_duty",  "Injector Duty",    "%",       0,    100},
    {"ign_adv",   "Ignition Adv",     "\xC2\xB0", -10, 45},
    {"oil_press", "Oil Pressure",     "Bar",     0,    8},
    {"fuel_press","Fuel Pressure",    "Bar",     0,    6},
    {"vss",       "Vehicle Speed",    "km/h",    0,    300},
    {"tpp",       "Pedal Position",   "%",       0,    100},
};

const ChannelMeta &channel_meta(Channel ch) {
  if (ch >= CH_COUNT) ch = CH_RPM;
  return kMeta[ch];
}

void begin() {
  g_telemetry.begin();
}

void TelemetryStore::begin() {
  if (this->mutex_ == nullptr) {
    this->mutex_ = xSemaphoreCreateMutex();
  }
  memset(&this->state_, 0, sizeof(this->state_));
}

void TelemetryStore::set(Channel ch, float value) {
  if (ch >= CH_COUNT || this->mutex_ == nullptr) return;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(this->mutex_), portMAX_DELAY);
  this->state_.values[ch] = value;
  this->state_.valid[ch] = true;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(this->mutex_));
}

void TelemetryStore::set_ecu_version(EcuUnit unit, bool firmware, uint8_t major, uint8_t minor) {
  if (this->mutex_ == nullptr) return;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(this->mutex_), portMAX_DELAY);
  EcuUnitVersion &u = unit == UNIT_COM ? this->state_.ident.com
                    : unit == UNIT_DSP ? this->state_.ident.dsp : this->state_.ident.cpu;
  if (firmware) { u.fw_major = major; u.fw_minor = minor; }
  else          { u.bl_major = major; u.bl_minor = minor; }
  u.valid = true;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(this->mutex_));
}

void TelemetryStore::mark_frame() {
  if (this->mutex_ == nullptr) return;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(this->mutex_), portMAX_DELAY);
  this->state_.frame_count++;
  this->state_.last_update_ms = millis();
  this->state_.link_ok = true;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(this->mutex_));
}

void TelemetryStore::refresh_link_(uint32_t now_ms) {
  if (this->state_.link_ok &&
      (now_ms - this->state_.last_update_ms) > LINK_TIMEOUT_MS) {
    this->state_.link_ok = false;
  }
}

TelemetryState TelemetryStore::snapshot() {
  TelemetryState copy{};
  if (this->mutex_ == nullptr) return copy;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(this->mutex_), portMAX_DELAY);
  this->refresh_link_(millis());
  copy = this->state_;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(this->mutex_));
  return copy;
}

}  // namespace ecu

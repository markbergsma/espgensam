#pragma once

/// @file text_sensor.h
/// @brief ESPHome TextSensor entity implementation for Genelec SAM bus operational status.
///
/// ===================================================================================
/// ARCHITECTURAL DESIGN RATIONALE
/// ===================================================================================
/// 1. Bus Operational Status (GenSAMBusStatusSensor):
///    Exposes the real-time operational state of the Genelec SAM RS-485 bus to Home Assistant
///    as a diagnostic text sensor entity (`entity_category: diagnostic`).
///    - "Active": Hub is active standalone master; running live telemetry polling loop
///    - "GLM Active": External GLM controller active on bus (espgensam yielded)
///    - "Discovering": Active RACE discovery, pinging, or device interrogation in progress
///    - "Configuring": Device parameter configuration phase (audio source, crossover)
///    - "Standby": System in low-power amplifier sleep (<0.5W, bus quiet)
///    - "Offline": Standalone idle, no monitors discovered on the bus
///
/// 2. Bidirectional & State Machine Updates:
///    The sensor is updated automatically by GenSAMHub whenever active state machine phases
///    advance in race.cpp, whenever power state changes, or when external GLM wire frames
///    are snooped.
/// ===================================================================================

#include "esphome/core/component.h"
#include "esphome/components/text_sensor/text_sensor.h"

namespace esphome {
namespace gensam {

class GenSAMHub;

/// @brief Diagnostic text sensor reporting the current Genelec SAM bus state.
class GenSAMBusStatusSensor : public text_sensor::TextSensor {
 public:
  /// @brief Set the parent GenSAMHub instance.
  /// @param hub Pointer to the GenSAMHub.
  void set_hub(GenSAMHub *hub) { hub_ = hub; }

 protected:
  GenSAMHub *hub_{nullptr};
};

}  // namespace gensam
}  // namespace esphome

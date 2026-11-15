#pragma once
// MQTT link to Home Assistant and the monitoring stack (plan B6a).
//
// Topics (base = sensors/<loc>/<dev>, settings mqtt_loc / mqtt_dev):
//   base                         state JSON every 10 s and after changes. Telegraf stores its numeric fields
//                                (iot_<field>{location, device}); HA reads it through value templates.
//   base/status/availability     "online" / "offline" (retained, last will)
//   base/cmd/<name>              commands from HA: setpoint, mode, preset, dhw_boost
//   homeassistant/<component>/<node>/<object>/config   discovery (retained), sent after every connect and
//                                whenever HA announces "online" on homeassistant/status.
// Commands are range-checked and act like the buttons on the device; the thermostat never depends on MQTT.
#include <stdbool.h>
#include <stddef.h>

void mqtt_ha_start(void);
// Call after the MQTT settings changed: reconnects with the new settings.
void mqtt_ha_restart(void);
// Short human-readable state for the web page and the device ("connected", "disabled", "error: ...").
void mqtt_ha_status(char *out, size_t size);
// Bench: prints the state JSON and a discovery payload on the serial console ("MQTTSTATE ...", "MQTTDISC ...").
void mqtt_ha_dump(void);

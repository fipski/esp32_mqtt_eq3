#ifndef EQ3_HA_H
#define EQ3_HA_H

#include <cJSON.h>


cJSON* generate_ha_therm_payload (char mac[6]);

cJSON* generate_ha_valve_payload (char mac[6]);

cJSON* generate_ha_battery_payload (char mac[6]);

#endif
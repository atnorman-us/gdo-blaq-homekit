#pragma once

#include "gdo.h"   // <-- REQUIRED so the types exist

#ifdef __cplusplus
extern "C" {
#endif

void notify_homekit_current_door_state_change(gdo_door_state_t door);
void notify_homekit_light(gdo_light_state_t light);
void notify_homekit_target_door_state_change(uint8_t tgt);

#ifdef __cplusplus
}
#endif


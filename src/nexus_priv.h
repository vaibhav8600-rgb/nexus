/* Internal helpers shared inside the NEXUS module. Not part of the API. */
#ifndef NEXUS_PRIV_H_
#define NEXUS_PRIV_H_

#include <zephyr/kernel.h>
#include <stdint.h>

/**
 * The one work queue NEXUS runs on. With the display built this is ZMK's
 * display queue, so LVGL is only ever touched from a single thread and NEXUS
 * adds zero threads of its own (Sections 89, 122).
 */
struct k_work_q *nexus_workq(void);

/* Set by src/nexus.c as subsystems report in. */
void nexus_health_set_display(bool ok);
void nexus_health_set_backlight(bool ok);
void nexus_health_set_buzzer(bool ok);
void nexus_health_set_button(bool ok);
void nexus_health_set_games(bool ok);

/* status.c internals used by zmk_events.c only. */
struct nexus_status *nexus_status_mut(void);
void nexus_status_mark(uint32_t changed);
void nexus_status_peripheral_battery(uint8_t source, uint8_t level);
void nexus_status_init(void);
void nexus_status_seed(void);

/* Buzzer HAL - only sound/sound.c calls these. */
int nexus_buzzer_init(void);
bool nexus_buzzer_ready(void);
int nexus_buzzer_tone(uint16_t freq_hz);

int nexus_button_init(void);
int nexus_backlight_init(void);

#endif /* NEXUS_PRIV_H_ */

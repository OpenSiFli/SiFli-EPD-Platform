#ifndef EPD_APP_CARD_H
#define EPD_APP_CARD_H

#include <stdbool.h>

void app_card_process(void);
bool app_card_prompt_visible(void);
void app_card_request(void);
/* Hide the overlay before entering a lock page; retain pending card work. */
void app_card_suspend(void);

#endif

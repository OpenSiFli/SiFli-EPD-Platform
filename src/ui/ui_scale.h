#ifndef UI_SCALE_H
#define UI_SCALE_H
#include <stdbool.h>
#include <stdint.h>

/* Prepare before saving; applying a prepared scale cannot allocate or fail. */
bool ui_scale_prepare(unsigned index);
void ui_scale_apply(unsigned index);
unsigned ui_scale_percent_at(unsigned index);

#endif

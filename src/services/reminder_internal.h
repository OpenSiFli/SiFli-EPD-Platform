#ifndef REMINDER_INTERNAL_H
#define REMINDER_INTERNAL_H
#include <stdbool.h>
void reminder_start(void);
void reminder_time_changed(void);
bool reminder_dismiss(const char *app, const char *id);
bool reminder_remove_app(const char *app);
#endif

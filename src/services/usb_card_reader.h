#ifndef USB_CARD_READER_H
#define USB_CARD_READER_H
#include <stdbool.h>

typedef struct
{
    bool supported, connected, enabled, ready, busy;
    int error;
} usb_card_reader_status_t;

void usb_card_reader_status(usb_card_reader_status_t *status);
/* UI thread: sharing is a runtime operation, never a saved boot preference. */
bool usb_card_reader_set_enabled(bool enabled);
bool usb_card_reader_active(void);
/* Resident SD/USB event signal; safe from interrupt context. */
void usb_card_reader_notify(void);
#endif

/* SPDX-License-Identifier: Apache-2.0 */
#include "../formats/book_internal.h"
/* Legacy HTML is detected in a SAX callback. Finish that bounded probe chunk
 * before replacing its parser: this libxml version accesses input->cur[1]
 * after callbacks, while xmlStopParser replaces cur with a one-byte string. */
static void book_compute_probe_stop(xmlParserCtxtPtr parser) { (void)parser; }
#define xmlStopParser book_compute_probe_stop
#include "../formats/book_svg_markup.c"

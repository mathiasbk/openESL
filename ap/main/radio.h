#pragma once
#include <stdbool.h>
#include "esl_link.h"

/* Between radio.c (the radio task, the same for every link) and the driver the
 * build uses (Kconfig "Radio driver"). The driver defines radio_link and hands
 * every received frame to the task with one of the two calls below; the task
 * answers through radio_link.send. See components/esl_core/include/esl_link.h. */

extern esl_link_t radio_link;

/* From the driver's receive interrupt. Returns false if the queue was full (the
 * frame is then counted as dropped). */
bool radio_rx_from_isr(const esl_link_rx_t *f);
/* The same from a task (a driver that reads its frames from a UART, say) */
bool radio_rx(const esl_link_rx_t *f);

/*
 * main.c - start-up and main loop for ZBS243. All logic is in app.c.
 */
#include "app.h"
#include "hal/hal.h"

void main(void)
{
    hal_init();
    hal_uart_puts("OpenESL tag\r\n");
    app_init();
    for (;;)
        power_sleep_s(app_poll_once());
}

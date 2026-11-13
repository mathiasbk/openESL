/* test_spimodem.c - the SPI modem's framing (radio_spimodem.c) without a device: the transmit
 * command with its inverted XOR, and the records one read() returns. Built and run by
 * "make test". */
#include "radio_spimodem.c"

int ap_verbose;
void ap_log(const char *fmt, ...) { (void)fmt; }
void ap_debug(const char *fmt, ...) { (void)fmt; }
int64_t ap_clock_us(void) { return 0; }
void esl_link_sending(esl_link_t *l, const esl_link_rx_t *req, int64_t now_us) { (void)l, (void)req, (void)now_us; }

static int fails;
#define CHECK(c, what) do { if (c) printf("ok   spi-modem: %s\n", what); else { printf("FAIL spi-modem: %s\n", what); fails++; } } while (0)

int main(void)
{
    /* an IDLE: FCF 0x8C41, seq, PAN, dst long, src short 0, payload 02 01 0F 00, FCS room */
    uint8_t psdu[] = {0x41, 0x8C, 0x07, 0x53, 0x45, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0x02, 0x01, 0x0F, 0x00, 0xAA, 0xBB};
    uint8_t b[4 + ESL_PSDU_MAX + 1];
    size_t n = tx_frame(b, psdu, sizeof(psdu));
    uint8_t x = 0xFF;
    for (size_t i = 0; i < sizeof(psdu) - 2; i++)
        x ^= psdu[i];
    CHECK(n == 4 + sizeof(psdu) - 2 + 1, "the command is the frame without its FCS, and the check byte");
    CHECK(b[0] == 0x88 && b[1] == RATE_250K, "transmit at 250 kbps");
    CHECK(b[2] == sizeof(psdu) - 1 && b[3] == 0, "the length counts the FCS, minus one");
    CHECK(!memcmp(b + 4, psdu, sizeof(psdu) - 2), "the frame as it is");
    CHECK(b[n - 1] == x, "the check byte is 0xFF ^ XOR of the frame");
    CHECK(tx_frame(b, psdu, 2) == 0 && tx_frame(b, psdu, ESL_PSDU_MAX + 1) == 0, "no frame, or too long, is refused");

    static uint8_t buf[REC_LEN * REC_MAX];
    esl_link_rx_t f[REC_MAX];
    uint8_t poll[18 + ESL_MHR_LEN];
    for (size_t i = 0; i < sizeof(poll); i++)
        poll[i] = (uint8_t)i;
    for (int k = 0; k < 3; k++) {
        uint8_t *rec = buf + REC_LEN * k;
        rec[1] = k == 1 ? 3 : sizeof(poll);      /* the middle one too short to be a frame */
        memcpy(rec + 2, poll, sizeof(poll));
        rec[2 + rec[1]] = (uint8_t)(-60 - k);
    }
    size_t got = rx_records(buf, 3, f, REC_MAX);
    CHECK(got == 2, "a count of records: the short one is dropped");
    CHECK(f[0].len == sizeof(poll) + ESL_FCS_LEN && !memcmp(f[0].psdu, poll, sizeof(poll)), "a frame with room for its FCS");
    CHECK(f[0].rssi == -60 && f[1].rssi == -62, "the RSSI after each frame");
    CHECK(rx_records(buf, 3 * REC_LEN, f, REC_MAX) == 2, "a count of bytes gives the same");
    CHECK(rx_records(buf, 3, f, 1) == 1, "never more than there is room for");
    printf(fails ? "spi-modem: %d failed\n" : "all spi-modem tests passed\n", fails);
    return fails != 0;
}

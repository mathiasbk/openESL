/* test_ble.c - ble.c without an adapter: a Gicisky advertisement and an LE advertising report
 * read, and the ATT client against a fake tag on a socket pair (discovery of FEF0, FEF1, FEF2
 * and their CCCDs, writes with response, a notification that comes before the write's
 * response). Built and run by "make test". */
#include "ble.c"

ap_t ap;
int ap_verbose;
void ap_log(const char *fmt, ...) { (void)fmt; }
void ap_debug(const char *fmt, ...) { (void)fmt; }
int64_t ap_clock_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
uint64_t ap_now_ms(void) { return (uint64_t)ap_clock_us() / 1000; }

static int fails;
#define CHECK(c, what) do { if (c) printf("ok   ble: %s\n", what); else { printf("FAIL ble: %s\n", what); fails++; } } while (0)

/* The fake tag: FEF0 at 0x10-0x20, FEF1 value 0x12 (CCCD 0x13), FEF2 value 0x15 (CCCD 0x16) */
static int cccd_writes, fef1_writes, fef2_writes;

static void reply(int fd, const uint8_t *b, size_t n)
{
    if (write(fd, b, n) != (ssize_t)n)
        perror("fake tag");
}

static void *fake_tag(void *arg)
{
    int fd = *(int *)arg;
    uint8_t q[600];
    ssize_t n;
    while ((n = read(fd, q, sizeof(q))) > 0) {
        uint16_t from = n >= 3 ? get16(q + 1) : 0;
        switch (q[0]) {
        case ATT_MTU_REQ: {
            uint8_t r[3] = {ATT_MTU_RSP, 0xF7, 0x00}; /* 247 */
            reply(fd, r, 3);
            break;
        }
        case ATT_FIND_TYPE_REQ: {
            uint8_t r[5] = {ATT_FIND_TYPE_RSP, 0x10, 0x00, 0x20, 0x00};
            if (n == 9 && get16(q + 7) == 0xFEF0)
                reply(fd, r, 5);
            break;
        }
        case ATT_READ_TYPE_REQ: {
            /* one characteristic per answer, so the client has to ask again */
            uint8_t r1[9] = {ATT_READ_TYPE_RSP, 7, 0x11, 0x00, 0x12, 0x12, 0x00, 0xF1, 0xFE};
            uint8_t r2[9] = {ATT_READ_TYPE_RSP, 7, 0x14, 0x00, 0x0C, 0x15, 0x00, 0xF2, 0xFE};
            uint8_t e[5] = {ATT_ERROR, ATT_READ_TYPE_REQ, q[1], q[2], ATT_ERR_NOT_FOUND};
            if (from <= 0x11)
                reply(fd, r1, 9);
            else if (from <= 0x14)
                reply(fd, r2, 9);
            else
                reply(fd, e, 5);
            break;
        }
        case ATT_FIND_INFO_REQ: {
            uint8_t r[6] = {ATT_FIND_INFO_RSP, 1, 0, 0, 0x02, 0x29};
            put16(r + 2, from == 0x13 ? 0x13 : 0x16);
            reply(fd, r, 6);
            break;
        }
        case ATT_WRITE_REQ: {
            uint8_t ok = ATT_WRITE_RSP;
            uint16_t h = get16(q + 1);
            if (h == 0x13 || h == 0x16)
                cccd_writes++;
            if (h == 0x12) {
                fef1_writes++;
                /* the tag's answer before the write's response, as a busy link can deliver it */
                uint8_t note[6] = {ATT_NOTIFY, 0x12, 0x00, 0x01, 0xF4, 0x00};
                reply(fd, note, sizeof(note));
            }
            if (h == 0x15)
                fef2_writes++;
            reply(fd, &ok, 1);
            break;
        }
        }
    }
    return NULL;
}

int main(void)
{
    /* advertisements */
    uint8_t adv[] = {0x02, 0x01, 0x06, 0x06, 0xFF, 0x53, 0x50, 0x2E, 0x11, 0x22};
    CHECK(adv_model(adv, sizeof(adv)) == ESL_MODEL_294, "a Gicisky advertisement names the 2.9\" BWRY");
    uint8_t other[] = {0x02, 0x01, 0x06, 0x05, 0xFF, 0x4C, 0x00, 0x2E, 0x11};
    CHECK(adv_model(other, sizeof(other)) == 0, "another maker's is not one");
    uint8_t broken[] = {0x09, 0xFF, 0x53, 0x50};
    CHECK(adv_model(broken, sizeof(broken)) == 0, "a field longer than the advertisement is not read");

    static uint8_t buf[16];
    esl_push_out_t out = {NULL, NULL};
    esl_push_init(&lb.push, &out, buf, sizeof(buf));
    uint8_t ev[64] = {HCI_EVENT_PKT, EVT_LE_META, 0, LE_ADV_REPORT, 1, 0x00, 0x01, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
                      sizeof(adv)};
    memcpy(ev + 14, adv, sizeof(adv));
    ev[14 + sizeof(adv)] = (uint8_t)-61;
    scan_event(ev, 15 + sizeof(adv), 1000);
    const heard_t *h = heard_get(0x112233445566ULL);
    CHECK(h && h->addr_type == BDADDR_LE_RANDOM && h->rssi == -61, "an advertising report: the tag's MAC, address type and RSSI");
    CHECK(!esl_push_get(&lb.push, 0x112233445566ULL), "without --ble-auto-add it is not added");
    lb.auto_add = true;
    scan_event(ev, 15 + sizeof(adv) - 3, 2000);
    CHECK(!esl_push_get(&lb.push, 0x112233445566ULL), "a report cut short is dropped");
    CHECK(parse_mac("11:22:33:44:55:66") == 0x112233445566ULL && parse_mac("") == 0, "--ble-tag is read as on the ESP32");

    /* ATT against the fake tag */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) {
        perror("socketpair");
        return 1;
    }
    pthread_t t;
    pthread_create(&t, NULL, fake_tag, &sv[1]);
    att_t *a = &lb.att;
    att_reset(a, sv[0], 0x112233445566ULL);
    CHECK(att_setup(a), "setup: MTU, FEF0, FEF1, FEF2 and their CCCDs");
    CHECK(a->mtu == 247, "the MTU is the tag's");
    CHECK(a->fef1_val == 0x12 && a->fef2_val == 0x15, "the value handles of FEF1 and FEF2");
    CHECK(a->fef1_cccd == 0x13 && a->fef2_cccd == 0x16, "their CCCDs");
    CHECK(cccd_writes == 2, "notifications are turned on for both");

    uint8_t cmd[1] = {0x01};
    CHECK(att_write(a, ESL_GICISKY_CMD, cmd, 1) && fef1_writes == 1, "a command goes to FEF1 with a write request");
    uint8_t want[2] = {0x01, 0xF4};
    CHECK(att_wait(a, want, 2), "a notification that came before the write's response is not lost");
    uint8_t chunk[244] = {0};
    CHECK(att_write(a, ESL_GICISKY_DATA, chunk, sizeof(chunk)) && fef2_writes == 1, "a 244-byte chunk fits the MTU");
    uint8_t big[245] = {0};
    CHECK(!att_write(a, ESL_GICISKY_DATA, big, sizeof(big)), "one larger than the MTU is refused");

    close(sv[0]);
    pthread_join(t, NULL);
    close(sv[1]);
    if (fails)
        return 1;
    printf("all ble tests passed\n");
    return 0;
}

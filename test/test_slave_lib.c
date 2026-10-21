/*
 * Public slave library API on the host HAL: the pins are variables, and a
 * minimal bit-banging master drives them and calls hubus_slave_isr() after
 * every change, like the pin change interrupt on a real device.
 */
#include <string.h>

#include "hal/hubus_hal_host.h"
#include "hubus_crc.h"
#include "hubus_frame.h"
#include "hubus_slave.h"
#include "unity.h"

static uint8_t master_low;
static unsigned captures;
static unsigned commands;
static uint8_t last_opcode;
static uint8_t last_len;
static const uint8_t info[] = {0x42, 0x01, 0x0A, 0x0B};

/* ---- wire --------------------------------------------------------------- */

static void wire(uint8_t clk)
{
    int i;

    hubus_host_clk = clk;
    for (i = 0; i < 4; i++) { /* settle: the slave may change its drive in the handler */
        hubus_host_data = (uint8_t)!(master_low || hubus_host_data_low);
        hubus_slave_isr();
    }
    hubus_host_data = (uint8_t)!(master_low || hubus_host_data_low);
}

static void pull(uint8_t low)
{
    master_low = low;
    wire(hubus_host_clk);
}

static void start(void)
{
    pull(0);
    for (int guard = 0; !hubus_host_data && guard < 4; guard++) { /* idle event handshake */
        wire(1);
        wire(0);
    }
    wire(1);
    pull(1);
    wire(0);
}

static void stop(void)
{
    pull(1);
    wire(1);
    pull(0);
    wire(0);
}

static void send_bit(int b)
{
    pull(b ? 1 : 0);
    wire(1);
    wire(0);
}

static int recv_bit(void)
{
    int b;

    wire(1);
    b = hubus_host_data ? 0 : 1;
    wire(0);
    return b;
}

static void send_frame(uint8_t type, uint8_t tlen, uint8_t mcid, const uint8_t *payload, uint8_t len)
{
    uint8_t buf[HUBUS_MAX_FRAME_BYTES];
    size_t n = hubus_frame_encode(buf, type, tlen, mcid, payload, len);
    size_t i;
    int b;

    for (i = 0; i < n; i++) {
        for (b = 7; b >= 0; b--) {
            send_bit((buf[i] >> b) & 1);
        }
    }
}

static void turnaround(void)
{
    pull(0);
    wire(1);
    wire(0);
}

static size_t recv_frame(uint8_t *buf)
{
    size_t total = 24, n = 1;
    uint8_t cur = 1;

    if (!recv_bit()) {
        return 0;
    }
    while (n < total) {
        cur = (uint8_t)((cur << 1) | recv_bit());
        n++;
        if (n % 8 == 0) {
            buf[n / 8 - 1] = cur;
            cur = 0;
            if (n == 16) {
                total = (size_t)(3 + (buf[1] & 0x1F)) * 8;
            }
        }
    }
    return total / 8;
}

/* ---- callbacks ------------------------------------------------------------ */

static void on_capture(void *ctx)
{
    uint8_t status = 0x77;

    (void)ctx;
    captures++;
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_OK, hubus_slave_write(&status, 1, 0)); /* callbacks may write */
}

static void on_command(void *ctx, uint8_t opcode, const uint8_t *args, uint8_t len)
{
    (void)ctx;
    (void)args;
    commands++;
    last_opcode = opcode;
    last_len = len;
}

static hubus_slave_config_t config(uint8_t mcid)
{
    hubus_slave_config_t cfg;

    memset(&cfg, 0, sizeof cfg);
    cfg.mcid = mcid;
    cfg.info = info;
    cfg.info_len = sizeof info;
    cfg.on_status_capture = on_capture;
    cfg.on_command = on_command;
    return cfg;
}

void setUp(void)
{
    master_low = 0;
    captures = 0;
    commands = 0;
    hubus_host_clk = 0;
    hubus_host_data = 1;
    hubus_host_data_low = 0;
    hubus_host_irq_on = 0;
}

void tearDown(void) {}

/* ---- tests ---------------------------------------------------------------- */

static void test_init_enables_interrupts_and_releases_data(void)
{
    hubus_slave_config_t cfg = config(7);

    TEST_ASSERT_TRUE(hubus_slave_init(&cfg));
    TEST_ASSERT_EQUAL_UINT8(1, hubus_host_irq_on);
    TEST_ASSERT_EQUAL_UINT8(0, hubus_host_data_low);
    TEST_ASSERT_FALSE(hubus_slave_is_pending());
}

static void test_init_with_invalid_mcid_leaves_the_slave_silent(void)
{
    hubus_slave_config_t cfg = config(0);
    uint8_t v = 1;

    TEST_ASSERT_FALSE(hubus_slave_init(&cfg));
    TEST_ASSERT_EQUAL_UINT8(0, hubus_host_irq_on);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_DISABLED, hubus_slave_write(&v, 1, 0));
    TEST_ASSERT_EQUAL_UINT8(0, hubus_host_data_low);

    cfg.mcid = 64;
    TEST_ASSERT_FALSE(hubus_slave_init(&cfg));
    cfg = config(5);
    cfg.info_len = 29; /* only 28 application bytes fit next to the 3 protocol bytes */
    TEST_ASSERT_FALSE(hubus_slave_init(&cfg));
}

static void test_write_announces_event_and_request_data_returns_it(void)
{
    hubus_slave_config_t cfg = config(7);
    const uint8_t payload[] = {1, 2, 3};
    uint8_t buf[40];
    hubus_header_t h;
    size_t n;

    TEST_ASSERT_TRUE(hubus_slave_init(&cfg));
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_OK, hubus_slave_write(payload, 3, 1));
    TEST_ASSERT_TRUE(hubus_slave_is_pending());
    TEST_ASSERT_EQUAL_UINT8(1, hubus_host_data_low); /* event signal */

    start(); /* the handshake pulse is part of start() */
    send_frame(HUBUS_M_REQUEST_DATA, HUBUS_TARGET_LEN_EXACT, 7, NULL, 0);
    turnaround();
    n = recv_frame(buf);
    stop();

    TEST_ASSERT_EQUAL_UINT(6, n);
    TEST_ASSERT_TRUE(hubus_frame_decode(buf, n, &h));
    TEST_ASSERT_EQUAL_UINT8(HUBUS_S_DATA, h.type);
    TEST_ASSERT_EQUAL_UINT8(7, h.mcid);
    TEST_ASSERT_EQUAL_UINT8(1, HUBUS_HDR_MORE(&h));
    TEST_ASSERT_EQUAL_MEMORY(payload, buf + 2, 3);
    TEST_ASSERT_FALSE(hubus_slave_is_pending()); /* STOP cleared the buffer */
}

static void test_request_info(void)
{
    hubus_slave_config_t cfg = config(7);
    uint8_t buf[40];
    size_t n;

    TEST_ASSERT_TRUE(hubus_slave_init(&cfg));
    start();
    send_frame(HUBUS_M_REQUEST_INFO, HUBUS_TARGET_LEN_EXACT, 7, NULL, 0);
    turnaround();
    n = recv_frame(buf);
    stop();
    TEST_ASSERT_EQUAL_UINT(3 + 3 + sizeof info, n);
    TEST_ASSERT_TRUE(hubus_frame_decode(buf, n, NULL));
    TEST_ASSERT_EQUAL_UINT8(HUBUS_CAP_STATUS_CAPTURE, buf[2 + HUBUS_INFO_CAPS]);
    TEST_ASSERT_EQUAL_UINT8(HUBUS_RX_BUF_SIZE, buf[2 + HUBUS_INFO_MAX_CMD]);
    TEST_ASSERT_EQUAL_MEMORY(info, &buf[2 + HUBUS_INFO_APP], sizeof info);
}

static void test_status_capture_callback_writes_the_buffer_from_the_isr(void)
{
    hubus_slave_config_t cfg = config(7);
    const uint8_t op = HUBUS_OP_STATUS_CAPTURE;
    uint8_t buf[40];
    size_t n;

    TEST_ASSERT_TRUE(hubus_slave_init(&cfg));
    start();
    send_frame(HUBUS_M_SEND_COMMAND, 0, 0, &op, 1); /* broadcast */
    stop();
    TEST_ASSERT_EQUAL_UINT(1, captures);
    TEST_ASSERT_TRUE(hubus_slave_is_pending());

    start();
    send_frame(HUBUS_M_REQUEST_DATA, HUBUS_TARGET_LEN_EXACT, 7, NULL, 0);
    turnaround();
    n = recv_frame(buf);
    stop();
    TEST_ASSERT_EQUAL_UINT(4, n);
    TEST_ASSERT_EQUAL_UINT8(0x77, buf[2]);
}

static void test_application_command(void)
{
    hubus_slave_config_t cfg = config(7);
    const uint8_t cmd[] = {0x25, 1, 2};

    TEST_ASSERT_TRUE(hubus_slave_init(&cfg));
    start();
    send_frame(HUBUS_M_SEND_COMMAND, HUBUS_TARGET_LEN_EXACT, 7, cmd, 3);
    stop();
    TEST_ASSERT_EQUAL_UINT(1, commands);
    TEST_ASSERT_EQUAL_UINT8(0x25, last_opcode);
    TEST_ASSERT_EQUAL_UINT8(2, last_len);
}

static void test_poll_arbitration_with_mcid_7(void)
{
    hubus_slave_config_t cfg = config(7); /* 000111 */
    const uint8_t v = 1;
    unsigned i;
    unsigned slots = 0;

    TEST_ASSERT_TRUE(hubus_slave_init(&cfg));
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_OK, hubus_slave_write(&v, 1, 0));
    start();
    send_frame(HUBUS_M_POLL_PENDING, 0, 0, NULL, 0);
    turnaround();
    for (i = 0; i < HUBUS_ARB_SLOTS; i++) {
        slots |= (unsigned)recv_bit() << i;
    }
    stop();
    /* arbitration bits 0,0,0,1,1,1 -> slots 6, 8, 10; no handoff */
    TEST_ASSERT_EQUAL_HEX16((1u << 6) | (1u << 8) | (1u << 10), slots);
    TEST_ASSERT_TRUE(hubus_slave_is_pending());
}

static void test_write_limits_and_busy(void)
{
    hubus_slave_config_t cfg = config(7);
    uint8_t big[HUBUS_TX_BUF_SIZE + 1] = {0};
    uint8_t v = 9;
    unsigned i;

    TEST_ASSERT_TRUE(hubus_slave_init(&cfg));
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_TOO_LONG, hubus_slave_write(big, sizeof big, 0));
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_OK, hubus_slave_write(big, HUBUS_TX_BUF_SIZE, 0));

    start();
    send_frame(HUBUS_M_REQUEST_DATA, HUBUS_TARGET_LEN_EXACT, 7, NULL, 0);
    turnaround();
    for (i = 0; i < 10; i++) {
        (void)recv_bit();
    }
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_BUSY, hubus_slave_write(&v, 1, 0)); /* response in progress */
    start();                                                                /* the master aborts */
    send_frame(HUBUS_M_REQUEST_DATA, HUBUS_TARGET_LEN_EXACT, 7, NULL, 0);
    stop();
    TEST_ASSERT_EQUAL_UINT8(HUBUS_WRITE_OK, hubus_slave_write(&v, 1, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_enables_interrupts_and_releases_data);
    RUN_TEST(test_init_with_invalid_mcid_leaves_the_slave_silent);
    RUN_TEST(test_write_announces_event_and_request_data_returns_it);
    RUN_TEST(test_request_info);
    RUN_TEST(test_status_capture_callback_writes_the_buffer_from_the_isr);
    RUN_TEST(test_application_command);
    RUN_TEST(test_poll_arbitration_with_mcid_7);
    RUN_TEST(test_write_limits_and_busy);
    return UNITY_END();
}

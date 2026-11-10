/*
 * The slave examples (examples/slave-*) as they are shipped, on simulated ATtiny85 devices against the real master:
 * the relays sleep in their main loop and wake up by the pin change interrupt, the switch debounces a button in a
 * loop of 1 ms delays, and the sensors measure with the ADC between their delays. The tests drive the inputs (button,
 * temperature sensor) and watch the outputs (relay pin) of the devices.
 */
#include <stdio.h>
#include <string.h>

#include "avr_adc.h"
#include "avr_ioport.h"
#include "avr_slave.h"
#include "fixture.h"
#include "sim_avr.h"
#include "sim_time.h"

#define MCU "attiny85"
#define F_CPU_HZ 8000000u
#define FW(name) HUBUS_AVR_FW_DIR "/" name ".elf"
#define NS_PER_CYCLE (1000000000u / F_CPU_HZ)
#define MS 1000000u

#define BOOT_NS (5u * MS) /* C runtime, the first measurement of the sensors, hubus_slave_init() */

/* MCIDs and hardware identifiers of the examples */
#define RELAY 3u
#define RELAY_SIMPLE 4u
#define SWITCH 5u
#define SENSOR 7u
#define SENSOR_PASSIVE 8u

#define OP_SET 0x10
#define OP_TOGGLE 0x11

#define RELAY_PIN 0     /* PB0 */
#define BUTTON_PIN 1    /* PB1, to ground */
#define DEBOUNCE_NS (10u * MS) /* 10 samples of 1 ms */

static fx_t fx;
static avr_bus_t bus;

void setUp(void)
{
    fx_init(&fx);
    avr_bus_init(&bus, &fx.sim);
}

void tearDown(void)
{
    fx_check(&fx);
    avr_bus_free(&bus);
}

static avr_slave_t *add(const char *elf)
{
    avr_slave_t *d = avr_bus_add(&bus, elf, MCU, F_CPU_HZ);

    /* the button input is open while the button is not pressed (internal pull-up) */
    avr_raise_irq(avr_io_getirq(d->avr, AVR_IOCTL_IOPORT_GETIRQ('B'), BUTTON_PIN), 1);
    return d;
}

static void boot(void)
{
    avr_bus_idle(&bus, BOOT_NS);
    fx_startup(&fx);
}

static void command(uint8_t mcid, uint8_t op, const uint8_t *args, uint8_t len)
{
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_send_command(&fx.master, 6, mcid, op, args, len));
}

static void set_relay(uint8_t mcid, uint8_t on)
{
    command(mcid, OP_SET, &on, 1);
}

/* Level the device drives on PB<pin>. */
static uint8_t output(const avr_slave_t *d, uint8_t pin)
{
    return (uint8_t)(avr_io_getirq(d->avr, AVR_IOCTL_IOPORT_GETIRQ('B'), pin)->value != 0);
}

/* The button drives PB1 from outside. simavr lets the internal pull-up win over a level raised on the pin at every
 * write of PORTB or DDRB, so the level is also set as the external one of the port. */
/* The pins of `mask` are driven from outside with the levels of `value`; the external levels of the port are replaced
 * as a whole, so every driven pin goes into one call. */
static void drive(const avr_slave_t *d, uint8_t mask, uint8_t value)
{
    avr_ioport_external_t ext;
    uint8_t pin;

    ext.name = 'B';
    ext.mask = mask;
    ext.value = value & mask;
    avr_ioctl(d->avr, AVR_IOCTL_IOPORT_SET_EXTERNAL('B'), &ext);
    for (pin = 0; pin < 8; pin++) {
        if (mask & (1u << pin)) {
            avr_raise_irq(avr_io_getirq(d->avr, AVR_IOCTL_IOPORT_GETIRQ('B'), pin), (value >> pin) & 1u);
        }
    }
}

static void press(const avr_slave_t *d, uint8_t pressed)
{
    drive(d, 1u << BUTTON_PIN, pressed ? 0u : 1u << BUTTON_PIN);
}

/* Voltage of the temperature sensor in mV; simavr converts it against the 1.1 V reference the sensors select. */
static void temperature(const avr_slave_t *d, uint16_t mv)
{
    avr_raise_irq(avr_io_getirq(d->avr, AVR_IOCTL_ADC_GETIRQ, ADC_IRQ_TEMP), mv);
}

/* The ADC result for `mv` (simavr: mv * 1023 / 1100). */
static uint16_t adc(uint16_t mv)
{
    return (uint16_t)(mv * 1023u / 1100u);
}

static void read_state(uint8_t mcid, hubus_slave_frame_t *f, uint8_t len)
{
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_request_state(&fx.master, mcid, f));
    TEST_ASSERT_EQUAL_UINT8(mcid, f->mcid);
    TEST_ASSERT_EQUAL_UINT8(len, f->len);
}

static void check_info(uint8_t mcid, uint8_t type, uint8_t variant)
{
    hubus_slave_frame_t f;

    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_request_info(&fx.master, mcid, &f));
    TEST_ASSERT_EQUAL_UINT8(HUBUS_S_INFO, f.type);
    TEST_ASSERT_EQUAL_UINT8(mcid, f.mcid);
    TEST_ASSERT_EQUAL_HEX8(type, f.payload[HUBUS_INFO_HWID + HUBUS_HWID_TYPE]);
    TEST_ASSERT_EQUAL_HEX8(variant, f.payload[HUBUS_INFO_HWID + HUBUS_HWID_VARIANT]);
}

/* Frames of the bursts until no event is announced any more, with their full payload. */
typedef struct {
    uint8_t mcid;
    uint8_t len;
    uint8_t payload[HUBUS_MAX_PAYLOAD];
} event_t;

static event_t *rec_out;
static unsigned rec_n, rec_max;

static void on_record(void *ctx, const hubus_slave_frame_t *f)
{
    (void)ctx;
    if (rec_n < rec_max) {
        rec_out[rec_n].mcid = f->mcid;
        rec_out[rec_n].len = f->len;
        memcpy(rec_out[rec_n].payload, f->payload, f->len);
        rec_n++;
    }
}

static unsigned collect_events(event_t *out, unsigned max)
{
    int rounds;

    rec_out = out;
    rec_n = 0;
    rec_max = max;
    fx.on_record = on_record;
    for (rounds = 0; rounds < 8; rounds++) {
        uint8_t event = 0;
        hubus_burst_stats_t st;

        TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_check_event(&fx.master, &event));
        if (!event && rounds > 0) {
            break;
        }
        TEST_ASSERT_EQUAL(HUBUS_OK, fx_burst(&fx, &st));
        avr_bus_idle(&bus, MS / 2u);
    }
    fx.on_record = NULL;
    return rec_n;
}

static void report(const char *name, const avr_slave_t *d)
{
    printf("%s: %u ISRs, longest ISR %u ns, longest response %u ns, stack %u bytes\n", name, d->isr_count,
           (unsigned)(d->isr_max_cycles * NS_PER_CYCLE), (unsigned)(d->response_max_cycles * NS_PER_CYCLE),
           avr_slave_stack_depth(d));
}

/* ---- relays ------------------------------------------------------------------------------------------------------ */

/* The relay sleeps between transactions; the pin change wakes it up, and the command switches the output. */
static void test_relay_switches_and_reads_back(void)
{
    hubus_slave_frame_t f;
    event_t ev[2];
    avr_slave_t *relay = add(FW("ex_relay"));

    boot();
    check_info(RELAY, 0x81, 0x01);
    TEST_ASSERT_EQUAL_INT(cpu_Sleeping, relay->avr->state);
    TEST_ASSERT_EQUAL_UINT8(0, output(relay, RELAY_PIN));

    set_relay(RELAY, 1);
    TEST_ASSERT_EQUAL_UINT8(1, output(relay, RELAY_PIN));
    read_state(RELAY, &f, 1);
    TEST_ASSERT_EQUAL_UINT8(1, f.payload[0]);

    command(RELAY, OP_TOGGLE, NULL, 0);
    TEST_ASSERT_EQUAL_UINT8(0, output(relay, RELAY_PIN));
    read_state(RELAY, &f, 1);
    TEST_ASSERT_EQUAL_UINT8(0, f.payload[0]);

    /* STATUS_CAPTURE: the output state as an event */
    command(RELAY, OP_TOGGLE, NULL, 0);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_status_capture(&fx.master, 0, 0));
    TEST_ASSERT_EQUAL_UINT(1, collect_events(ev, 2));
    TEST_ASSERT_EQUAL_UINT8(RELAY, ev[0].mcid);
    TEST_ASSERT_EQUAL_UINT8(1, ev[0].len);
    TEST_ASSERT_EQUAL_UINT8(1, ev[0].payload[0]);

    avr_bus_idle(&bus, MS);
    TEST_ASSERT_EQUAL_INT(cpu_Sleeping, relay->avr->state);
    report("relay", relay);
}

/* Without event buffer and state: commands only; REQUEST_STATE and STATUS_CAPTURE get no answer. */
static void test_relay_without_feedback(void)
{
    hubus_slave_frame_t f;
    event_t ev[2];
    avr_slave_t *relay = add(FW("ex_relay_simple"));

    boot();
    check_info(RELAY_SIMPLE, 0x81, 0x02);
    set_relay(RELAY_SIMPLE, 1);
    TEST_ASSERT_EQUAL_UINT8(1, output(relay, RELAY_PIN));
    command(RELAY_SIMPLE, OP_TOGGLE, NULL, 0);
    TEST_ASSERT_EQUAL_UINT8(0, output(relay, RELAY_PIN));
    command(RELAY_SIMPLE, OP_TOGGLE, NULL, 0);
    TEST_ASSERT_EQUAL_UINT8(1, output(relay, RELAY_PIN));

    TEST_ASSERT_EQUAL(HUBUS_ERR_NO_RESPONSE, hubus_master_request_state(&fx.master, RELAY_SIMPLE, &f));
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_status_capture(&fx.master, 0, 0));
    TEST_ASSERT_EQUAL_UINT(0, collect_events(ev, 2));
    TEST_ASSERT_EQUAL_INT(cpu_Sleeping, relay->avr->state);
}

/* ---- switch ------------------------------------------------------------------------------------------------------ */

/* A press is announced once it has been stable for the debounce time, the release as well; a shorter press is not. */
static void test_switch_debounces_and_announces_the_button(void)
{
    hubus_slave_frame_t f;
    event_t ev[4];
    avr_slave_t *sw = add(FW("ex_switch"));

    boot();
    check_info(SWITCH, 0x82, 0x01);
    TEST_ASSERT_EQUAL_UINT(0, collect_events(ev, 4)); /* nothing pressed at power-up */

    press(sw, 1);
    avr_bus_idle(&bus, DEBOUNCE_NS / 2u);
    press(sw, 0);
    avr_bus_idle(&bus, 2u * DEBOUNCE_NS);
    TEST_ASSERT_EQUAL_UINT(0, collect_events(ev, 4));

    press(sw, 1);
    avr_bus_idle(&bus, DEBOUNCE_NS + 3u * MS);
    TEST_ASSERT_EQUAL_UINT(1, collect_events(ev, 4));
    TEST_ASSERT_EQUAL_UINT8(SWITCH, ev[0].mcid);
    TEST_ASSERT_EQUAL_UINT8(1, ev[0].payload[0]);
    read_state(SWITCH, &f, 1);
    TEST_ASSERT_EQUAL_UINT8(1, f.payload[0]);

    press(sw, 0);
    avr_bus_idle(&bus, DEBOUNCE_NS + 3u * MS);
    TEST_ASSERT_EQUAL_UINT(1, collect_events(ev, 4));
    TEST_ASSERT_EQUAL_UINT8(0, ev[0].payload[0]);
    read_state(SWITCH, &f, 1);
    TEST_ASSERT_EQUAL_UINT8(0, f.payload[0]);
    report("switch", sw);
}

/* The button changes while the master keeps the bus busy: the write is refused during a transmission and repeated by
 * the main loop, so the event still arrives. */
static void test_switch_event_while_the_bus_is_busy(void)
{
    hubus_slave_frame_t f;
    event_t ev[4];
    avr_slave_t *sw = add(FW("ex_switch"));
    unsigned i;

    boot();
    press(sw, 1);
    for (i = 0; i < 200u; i++) { /* the debounce time passes during the traffic */
        read_state(SWITCH, &f, 1);
        check_info(SWITCH, 0x82, 0x01);
    }
    TEST_ASSERT_EQUAL_UINT8(1, f.payload[0]);
    TEST_ASSERT_EQUAL_UINT(1, collect_events(ev, 4));
    TEST_ASSERT_EQUAL_UINT8(1, ev[0].payload[0]);
}

/* ---- sensors ----------------------------------------------------------------------------------------------------- */

/* STATUS_CAPTURE copies the latest measurement into the event buffer, big endian. */
static void test_sensor_reports_its_measurement(void)
{
    event_t ev[2];
    avr_slave_t *sensor = add(FW("ex_sensor"));

    temperature(sensor, 300);
    boot();
    check_info(SENSOR, 0x83, 0x01);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_status_capture(&fx.master, 0, 0));
    TEST_ASSERT_EQUAL_UINT(1, collect_events(ev, 2));
    TEST_ASSERT_EQUAL_UINT8(SENSOR, ev[0].mcid);
    TEST_ASSERT_EQUAL_UINT8(2, ev[0].len);
    TEST_ASSERT_EQUAL_HEX16(adc(300), (uint16_t)(ev[0].payload[0] << 8 | ev[0].payload[1]));

    /* a new value after the next measurement (every 100 ms) */
    temperature(sensor, 320);
    avr_bus_idle(&bus, 120u * MS);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_status_capture(&fx.master, 0, 0));
    TEST_ASSERT_EQUAL_UINT(1, collect_events(ev, 2));
    TEST_ASSERT_EQUAL_HEX16(adc(320), (uint16_t)(ev[0].payload[0] << 8 | ev[0].payload[1]));
    report("sensor", sensor);
}

/* The passive sensor exposes its measurement as state, updated once per second; it never announces anything. */
static void test_passive_sensor_state(void)
{
    hubus_slave_frame_t f;
    event_t ev[2];
    avr_slave_t *sensor = add(FW("ex_sensor_passive"));

    temperature(sensor, 290);
    boot();
    check_info(SENSOR_PASSIVE, 0x83, 0x02);
    read_state(SENSOR_PASSIVE, &f, 2);
    TEST_ASSERT_EQUAL_HEX16(adc(290), (uint16_t)(f.payload[0] << 8 | f.payload[1]));

    temperature(sensor, 310);
    avr_bus_idle(&bus, 1100u * MS);
    read_state(SENSOR_PASSIVE, &f, 2);
    TEST_ASSERT_EQUAL_HEX16(adc(310), (uint16_t)(f.payload[0] << 8 | f.payload[1]));
    TEST_ASSERT_EQUAL(HUBUS_ERR_NO_RESPONSE, hubus_master_request_data(&fx.master, SENSOR_PASSIVE, &f));
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_status_capture(&fx.master, 0, 0));
    TEST_ASSERT_EQUAL_UINT(0, collect_events(ev, 2));
}

/* ---- motion sensor ----------------------------------------------------------------------------------------------- */

#define MOTION 12u
#define PIR_PIN 0 /* PB0 */
#define PIR_WARMUP_NS (100u * MS) /* the test firmware; the shipped one waits 30 s */

static void pir(const avr_slave_t *d, uint8_t motion)
{
    drive(d, 1u << PIR_PIN, motion ? 1u << PIR_PIN : 0u);
}

/* The PIR output is ignored during the warm-up; afterwards the current level is announced once, and every change of it
 * once it has been stable for the debounce time. There is no state to read and nothing to command. */
static void test_motion_sensor_announces_changes_after_the_warm_up(void)
{
    hubus_slave_frame_t f;
    event_t ev[4];
    avr_slave_t *m = add(FW("ex_motion"));

    pir(m, 0);
    boot();
    check_info(MOTION, 0x88, 0x01);
    pir(m, 1); /* the module is still warming up and says motion */
    avr_bus_idle(&bus, PIR_WARMUP_NS / 2u);
    TEST_ASSERT_EQUAL_UINT(0, collect_events(ev, 4));
    pir(m, 0);
    avr_bus_idle(&bus, PIR_WARMUP_NS);
    TEST_ASSERT_EQUAL_UINT(1, collect_events(ev, 4)); /* the level after the warm-up */
    TEST_ASSERT_EQUAL_UINT8(MOTION, ev[0].mcid);
    TEST_ASSERT_EQUAL_UINT8(1, ev[0].len);
    TEST_ASSERT_EQUAL_UINT8(0, ev[0].payload[0]);

    pir(m, 1);
    avr_bus_idle(&bus, DEBOUNCE_NS / 2u);
    pir(m, 0); /* a glitch */
    avr_bus_idle(&bus, 2u * DEBOUNCE_NS);
    TEST_ASSERT_EQUAL_UINT(0, collect_events(ev, 4));

    pir(m, 1);
    avr_bus_idle(&bus, DEBOUNCE_NS + 3u * MS);
    TEST_ASSERT_EQUAL_UINT(1, collect_events(ev, 4));
    TEST_ASSERT_EQUAL_UINT8(1, ev[0].payload[0]);
    pir(m, 0);
    avr_bus_idle(&bus, DEBOUNCE_NS + 3u * MS);
    TEST_ASSERT_EQUAL_UINT(1, collect_events(ev, 4));
    TEST_ASSERT_EQUAL_UINT8(0, ev[0].payload[0]);

    TEST_ASSERT_EQUAL(HUBUS_ERR_NO_RESPONSE, hubus_master_request_state(&fx.master, MOTION, &f));
    report("motion", m);
}

/* A sensor that is already high after the warm-up announces 1 once; the level is announced again only when it changes. */
static void test_motion_sensor_high_after_the_warm_up(void)
{
    event_t ev[4];
    avr_slave_t *m = add(FW("ex_motion"));

    pir(m, 1);
    boot();
    avr_bus_idle(&bus, 2u * PIR_WARMUP_NS);
    TEST_ASSERT_EQUAL_UINT(1, collect_events(ev, 4));
    TEST_ASSERT_EQUAL_UINT8(1, ev[0].payload[0]);
    avr_bus_idle(&bus, 50u * MS);
    TEST_ASSERT_EQUAL_UINT(0, collect_events(ev, 4));
}

/* ---- DHT11 ------------------------------------------------------------------------------------------------------- */

#define DHT 13u
#define DHT_PIN 0 /* PB0 */
#define DHT_SEGMENTS (3u + 80u + 1u)

/* A DHT11 on PB0 with its pull-up. The device pulls the line low for the start signal; when it releases the line after
 * at least 18 ms, the sensor answers like the datasheet says: the line is high for 30 us, low for 80, high for 80, then
 * 40 bits of 50 us low and 27 us (0) or 70 us (1) high, and a last 50 us low. simavr cycle timers move the line. */
typedef struct {
    avr_slave_t *dev;
    uint8_t data[5];
    int absent;      /* never answers */
    unsigned reads;  /* start signals answered */
    unsigned starts; /* every release after a long start signal */
    uint64_t low_since;
    uint8_t low;
    uint16_t dur[DHT_SEGMENTS];
    unsigned idx;
    uint8_t level;
} dht_t;

static dht_t dht;

static void dht_set(uint8_t level)
{
    dht.level = level;
    drive(dht.dev, 1u << DHT_PIN, level ? 1u << DHT_PIN : 0u);
}

static avr_cycle_count_t dht_step(struct avr_t *avr, avr_cycle_count_t when, void *param)
{
    (void)param;
    dht_set((uint8_t)!dht.level);
    if (++dht.idx >= DHT_SEGMENTS) {
        return 0;
    }
    return when + avr_usec_to_cycles(avr, dht.dur[dht.idx]);
}

static void dht_set_data(uint8_t humidity, uint8_t temperature)
{
    dht.data[0] = humidity;
    dht.data[1] = 0;
    dht.data[2] = temperature;
    dht.data[3] = 0;
    dht.data[4] = (uint8_t)(humidity + temperature);
}

static void dht_on_ddr(struct avr_irq_t *irq, uint32_t value, void *param)
{
    avr_t *avr = dht.dev->avr;
    unsigned i, k = 0;

    (void)irq;
    (void)param;
    if (value & (1u << DHT_PIN)) {
        dht.low = 1;
        dht.low_since = avr->cycle;
        return;
    }
    if (!dht.low) {
        return;
    }
    dht.low = 0;
    dht_set(1); /* the pull-up */
    if ((avr->cycle - dht.low_since) * 1000u < 18u * 1000u * (avr->frequency / 1000u)) {
        return; /* less than 18 ms: no start signal */
    }
    dht.starts++;
    if (dht.absent) {
        return;
    }
    dht.reads++;
    dht.dur[k++] = 30;
    dht.dur[k++] = 80;
    dht.dur[k++] = 80;
    for (i = 0; i < 40; i++) {
        dht.dur[k++] = 50;
        dht.dur[k++] = (dht.data[i >> 3] >> (7 - (i & 7))) & 1u ? 70 : 27;
    }
    dht.dur[k++] = 50;
    dht.idx = 0;
    avr_cycle_timer_register(avr, avr_usec_to_cycles(avr, dht.dur[0]), dht_step, NULL);
}

static avr_slave_t *add_dht(uint8_t humidity, uint8_t temperature)
{
    avr_slave_t *d = add(FW("ex_dht11"));

    memset(&dht, 0, sizeof dht);
    dht.dev = d;
    dht_set_data(humidity, temperature);
    dht_set(1);
    avr_irq_register_notify(avr_io_getirq(d->avr, AVR_IOCTL_IOPORT_GETIRQ('B'), IOPORT_IRQ_DIRECTION_ALL), dht_on_ddr,
                            NULL);
    return d;
}

#define DHT_FIRST_NS ((uint64_t)1100u * MS) /* the firmware reads once after one second */
#define DHT_PERIOD_NS ((uint64_t)2500u * MS) /* a cycle takes 2.0 s and, when the readings fail, 0.4 s more */

static void check_dht(uint8_t humidity, uint8_t temperature, uint8_t fault)
{
    hubus_slave_frame_t f;

    read_state(DHT, &f, 3);
    TEST_ASSERT_EQUAL_UINT8(humidity, f.payload[0]);
    TEST_ASSERT_EQUAL_UINT8(temperature, f.payload[1]);
    TEST_ASSERT_EQUAL_UINT8(fault, f.payload[2]);
}

/* The state is a fault until the first good reading, then humidity and temperature of the sensor, read once every two
 * seconds. The sensor is only read: no events, no data. */
static void test_dht11_state_follows_the_sensor(void)
{
    hubus_slave_frame_t f;
    event_t ev[2];
    avr_slave_t *d = add_dht(45, 23);

    boot();
    check_info(DHT, 0x86, 0x01);
    check_dht(0, 0, 1);
    avr_bus_idle(&bus, DHT_FIRST_NS);
    TEST_ASSERT_EQUAL_UINT(1, dht.reads);
    check_dht(45, 23, 0);

    dht_set_data(60, 31);
    avr_bus_idle(&bus, DHT_PERIOD_NS);
    check_dht(60, 31, 0);
    TEST_ASSERT_EQUAL_UINT(2, dht.reads);

    TEST_ASSERT_EQUAL(HUBUS_ERR_NO_RESPONSE, hubus_master_request_data(&fx.master, DHT, &f));
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_status_capture(&fx.master, 0, 0));
    TEST_ASSERT_EQUAL_UINT(0, collect_events(ev, 2));
    report("dht11", d);
}

/* A wrong checksum is repeated, then dropped; two failed readings in a row set the fault byte, and the values stay. A
 * sensor that does not answer does the same. */
static void test_dht11_faults(void)
{
    avr_slave_t *d = add_dht(45, 23);

    (void)d;
    boot();
    avr_bus_idle(&bus, DHT_FIRST_NS);
    check_dht(45, 23, 0);

    dht.data[4] ^= 1u; /* wrong checksum */
    avr_bus_idle(&bus, DHT_PERIOD_NS);
    check_dht(45, 23, 0); /* one failed reading is not a fault yet */
    avr_bus_idle(&bus, DHT_PERIOD_NS);
    check_dht(45, 23, 1);

    dht.data[4] ^= 1u;
    avr_bus_idle(&bus, DHT_PERIOD_NS);
    check_dht(45, 23, 0);

    dht.absent = 1;
    avr_bus_idle(&bus, 2u * DHT_PERIOD_NS);
    check_dht(45, 23, 1);
    check_info(DHT, 0x86, 0x01); /* the bus is not affected */
    dht.absent = 0;
    avr_bus_idle(&bus, DHT_PERIOD_NS);
    check_dht(45, 23, 0);

    dht_set_data(101, 23); /* humidity out of range */
    avr_bus_idle(&bus, 2u * DHT_PERIOD_NS);
    check_dht(45, 23, 1);
}

/* The master keeps the bus busy while the sensor is read: the reading is dropped when an edge waits and repeated, the
 * transactions all succeed (the slave never misses an edge), and the reading succeeds once the traffic stops. */
static void test_dht11_does_not_disturb_the_bus(void)
{
    hubus_slave_frame_t f;
    avr_slave_t *d = add_dht(40, 20);
    unsigned i;

    boot();
    for (i = 0; i < 600u; i++) { /* the first reading falls into this traffic */
        read_state(DHT, &f, 3);
        check_info(DHT, 0x86, 0x01);
    }
    avr_bus_idle(&bus, DHT_PERIOD_NS);
    check_dht(40, 20, 0);
    TEST_ASSERT_TRUE(dht.starts >= 1u);
    report("dht11 under traffic", d);
}

/* ---- distance sensor --------------------------------------------------------------------------------------------- */

#define DIST 14u
#define TRIG_PIN 0 /* PB0 */
#define ECHO_PIN 1 /* PB1 */
#define OP_THRESHOLD 0x40
#define SAMPLE_NS (60u * MS)

/* An HC-SR04: when TRIG falls after a pulse of at least 8 us, the echo pin goes high 0.4 ms later and stays high for the
 * time sound needs to the object and back (5.83 us per mm). `mode`: 0 an object at distance_mm, 1 no echo at all, 2 nothing
 * in range (the module raises the echo for 38 ms). */
typedef struct {
    avr_slave_t *dev;
    unsigned distance_mm;
    int mode;
    unsigned triggers;
    unsigned glitch; /* the next measurement sees an object at 100 mm, then the real one again */
    uint64_t trig_rise;
    int step;
} echo_t;

static echo_t echo;

static avr_cycle_count_t echo_step(struct avr_t *avr, avr_cycle_count_t when, void *param)
{
    unsigned mm;

    (void)param;
    if (echo.step == 0) {
        drive(echo.dev, 1u << ECHO_PIN, 1u << ECHO_PIN);
        echo.step = 1;
        mm = echo.glitch ? 100u : echo.distance_mm;
        echo.glitch = 0;
        return when + avr_usec_to_cycles(avr, echo.mode == 2 ? 38000u : (uint32_t)(mm * 5831u / 1000u));
    }
    drive(echo.dev, 1u << ECHO_PIN, 0);
    return 0;
}

static void echo_on_trig(struct avr_irq_t *irq, uint32_t value, void *param)
{
    avr_t *avr = echo.dev->avr;

    (void)irq;
    (void)param;
    if (value) {
        echo.trig_rise = avr->cycle;
        return;
    }
    if ((avr->cycle - echo.trig_rise) * 1000000u < 8u * (uint64_t)avr->frequency || echo.mode == 1) {
        return; /* a pulse shorter than 8 us, or a module that does not answer */
    }
    echo.triggers++;
    echo.step = 0;
    avr_cycle_timer_register(avr, avr_usec_to_cycles(avr, 400u), echo_step, NULL);
}

static avr_slave_t *add_echo(unsigned distance_mm)
{
    avr_slave_t *d = add(FW("ex_distance"));

    memset(&echo, 0, sizeof echo);
    echo.dev = d;
    echo.distance_mm = distance_mm;
    drive(d, 1u << ECHO_PIN, 0);
    avr_irq_register_notify(avr_io_getirq(d->avr, AVR_IOCTL_IOPORT_GETIRQ('B'), TRIG_PIN), echo_on_trig, NULL);
    return d;
}

static void set_threshold(uint16_t mm)
{
    uint8_t args[2] = {(uint8_t)(mm >> 8), (uint8_t)mm};

    command(DIST, OP_THRESHOLD, args, 2);
}

/* The five bytes of state and event: near, distance, threshold. */
static void check_distance(const uint8_t *p, uint8_t near, unsigned mm, unsigned tolerance, unsigned threshold)
{
    unsigned d = (unsigned)(p[1] << 8 | p[2]);
    unsigned t = (unsigned)(p[3] << 8 | p[4]);

    TEST_ASSERT_EQUAL_UINT8(near, p[0]);
    TEST_ASSERT_TRUE_MESSAGE(d + tolerance >= mm && d <= mm + tolerance, "distance outside the tolerance");
    TEST_ASSERT_EQUAL_UINT(threshold, t);
}

static void check_distance_state(uint8_t near, unsigned mm, unsigned tolerance, unsigned threshold)
{
    hubus_slave_frame_t f;

    read_state(DIST, &f, 5);
    check_distance(f.payload, near, mm, tolerance, threshold);
}

/* The distance can be read at any time; the power-up is announced with a threshold of 0, so that the master sends it. */
static void test_distance_is_read_and_the_power_up_announced(void)
{
    hubus_slave_frame_t f;
    event_t ev[2];
    avr_slave_t *d = add_echo(500);

    boot();
    check_info(DIST, 0x87, 0x01);
    avr_bus_idle(&bus, 2u * SAMPLE_NS);
    check_distance_state(0, 500, 6, 0);
    TEST_ASSERT_EQUAL_UINT(1, collect_events(ev, 2));
    TEST_ASSERT_EQUAL_UINT8(DIST, ev[0].mcid);
    TEST_ASSERT_EQUAL_UINT8(5, ev[0].len);
    TEST_ASSERT_EQUAL_UINT8(0, ev[0].payload[0]);
    TEST_ASSERT_EQUAL_UINT8(0, ev[0].payload[3]); /* no threshold yet */
    TEST_ASSERT_EQUAL_UINT8(0, ev[0].payload[4]);

    echo.distance_mm = 1200;
    avr_bus_idle(&bus, 2u * SAMPLE_NS);
    check_distance_state(0, 1200, 8, 0);
    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_status_capture(&fx.master, 0, 0));
    TEST_ASSERT_EQUAL_UINT(1, collect_events(ev, 2)); /* STATUS_CAPTURE */
    check_distance(ev[0].payload, 0, 1200, 8, 0);
    TEST_ASSERT_EQUAL(HUBUS_ERR_NO_RESPONSE, hubus_master_request_data(&fx.master, DIST, &f));
    report("distance", d);
}

/* An object closer than the threshold is announced once it has been seen twice, and its leaving once it has been gone twice;
 * a distance within the hysteresis above the threshold still counts as near. */
static void test_distance_announces_an_object_below_the_threshold(void)
{
    event_t ev[4];

    add_echo(500);
    boot();
    avr_bus_idle(&bus, SAMPLE_NS);
    (void)collect_events(ev, 4); /* the power-up */
    set_threshold(300);
    check_distance_state(0, 500, 8, 300); /* the threshold shows at once */
    avr_bus_idle(&bus, 3u * SAMPLE_NS);
    TEST_ASSERT_EQUAL_UINT(0, collect_events(ev, 4));

    echo.distance_mm = 250;
    avr_bus_idle(&bus, 4u * SAMPLE_NS);
    TEST_ASSERT_EQUAL_UINT(1, collect_events(ev, 4));
    check_distance(ev[0].payload, 1, 250, 8, 300);

    echo.distance_mm = 315; /* above the threshold, within the 20 mm hysteresis */
    avr_bus_idle(&bus, 4u * SAMPLE_NS);
    TEST_ASSERT_EQUAL_UINT(0, collect_events(ev, 4));
    check_distance_state(1, 315, 8, 300);

    echo.distance_mm = 340;
    avr_bus_idle(&bus, 4u * SAMPLE_NS);
    TEST_ASSERT_EQUAL_UINT(1, collect_events(ev, 4));
    check_distance(ev[0].payload, 0, 340, 8, 300);

    /* a single wrong echo makes no event */
    echo.glitch = 1;
    avr_bus_idle(&bus, 4u * SAMPLE_NS);
    TEST_ASSERT_EQUAL_UINT(0, collect_events(ev, 4));
}

/* A threshold of 0 switches the events off, a value above 4000 mm is ignored, and a new threshold decides at once. */
static void test_distance_threshold_commands(void)
{
    event_t ev[4];

    add_echo(500);
    boot();
    avr_bus_idle(&bus, SAMPLE_NS);
    (void)collect_events(ev, 4);

    set_threshold(4001);
    check_distance_state(0, 500, 8, 0); /* ignored */
    set_threshold(0);
    echo.distance_mm = 100;
    avr_bus_idle(&bus, 4u * SAMPLE_NS);
    TEST_ASSERT_EQUAL_UINT(0, collect_events(ev, 4));
    check_distance_state(0, 100, 8, 0);

    set_threshold(300); /* the object is already nearer: the event follows */
    avr_bus_idle(&bus, 4u * SAMPLE_NS);
    TEST_ASSERT_EQUAL_UINT(1, collect_events(ev, 4));
    check_distance(ev[0].payload, 1, 100, 8, 300);

    set_threshold(50); /* now it is farther than the new threshold */
    avr_bus_idle(&bus, 4u * SAMPLE_NS);
    TEST_ASSERT_EQUAL_UINT(1, collect_events(ev, 4));
    check_distance(ev[0].payload, 0, 100, 8, 50);
}

/* Without an echo the distance is 0 and the object is gone: no echo at all, or one that never ends (nothing in range). */
static void test_distance_without_an_echo(void)
{
    event_t ev[4];

    add_echo(200);
    boot();
    set_threshold(300);
    avr_bus_idle(&bus, 4u * SAMPLE_NS);
    (void)collect_events(ev, 4);
    check_distance_state(1, 200, 8, 300);

    echo.mode = 1;
    avr_bus_idle(&bus, 4u * SAMPLE_NS);
    TEST_ASSERT_EQUAL_UINT(1, collect_events(ev, 4));
    check_distance(ev[0].payload, 0, 0, 0, 300);

    echo.mode = 0;
    avr_bus_idle(&bus, 4u * SAMPLE_NS);
    TEST_ASSERT_EQUAL_UINT(1, collect_events(ev, 4));
    check_distance(ev[0].payload, 1, 200, 8, 300);

    echo.mode = 2;
    avr_bus_idle(&bus, 4u * SAMPLE_NS);
    TEST_ASSERT_EQUAL_UINT(1, collect_events(ev, 4));
    check_distance(ev[0].payload, 0, 0, 0, 300);
    TEST_ASSERT_TRUE(echo.triggers >= 4u);
}

/* The bus traffic of the master delays the ISR, and with it the end of the echo that the main loop waits for: the result
 * stays within 2.5 cm of the true distance while the master keeps the bus busy. */
static void test_distance_under_bus_traffic(void)
{
    hubus_slave_frame_t f;
    avr_slave_t *d = add_echo(1000);
    unsigned i, worst = 0, n = 0;

    boot();
    for (i = 0; i < 400u; i++) {
        unsigned v;

        read_state(DIST, &f, 5);
        check_info(DIST, 0x87, 0x01);
        v = (unsigned)(f.payload[1] << 8 | f.payload[2]);
        if (v != 0u) { /* the first sample is not there yet */
            unsigned err = v > 1000u ? v - 1000u : 1000u - v;

            worst = err > worst ? err : worst;
            n++;
        }
    }
    TEST_ASSERT_TRUE(n > 100u);
    TEST_ASSERT_TRUE_MESSAGE(worst <= 25u, "distance off by more than 2.5 cm under traffic");
    report("distance under traffic", d);
}

/* ---- together ---------------------------------------------------------------------------------------------------- */

static hubus_slave_frame_t states[4];
static unsigned n_states;

static void on_state(void *ctx, const hubus_slave_frame_t *f)
{
    (void)ctx;
    if (n_states < 4u) {
        states[n_states++] = *f;
    }
}

/* Four examples on one bus: a STATUS_CAPTURE of all is answered in descending MCID order by those with an event buffer,
 * and the states of all are read with one group request each. */
static void test_examples_share_the_bus(void)
{
    event_t ev[8];
    avr_slave_t *relay = add(FW("ex_relay"));
    avr_slave_t *sw = add(FW("ex_switch"));
    avr_slave_t *sensor = add(FW("ex_sensor"));
    avr_slave_t *passive = add(FW("ex_sensor_passive"));
    uint16_t count = 0;

    n_states = 0;
    temperature(sensor, 300);
    temperature(passive, 600);
    boot();
    set_relay(RELAY, 1);
    press(sw, 1);
    avr_bus_idle(&bus, DEBOUNCE_NS + 3u * MS);
    TEST_ASSERT_EQUAL_UINT(1, collect_events(ev, 8)); /* the press */
    TEST_ASSERT_EQUAL_UINT8(SWITCH, ev[0].mcid);

    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_status_capture(&fx.master, 0, 0));
    TEST_ASSERT_EQUAL_UINT(3, collect_events(ev, 8));
    TEST_ASSERT_EQUAL_UINT8(SENSOR, ev[0].mcid);
    TEST_ASSERT_EQUAL_HEX16(adc(300), (uint16_t)(ev[0].payload[0] << 8 | ev[0].payload[1]));
    TEST_ASSERT_EQUAL_UINT8(SWITCH, ev[1].mcid);
    TEST_ASSERT_EQUAL_UINT8(1, ev[1].payload[0]);
    TEST_ASSERT_EQUAL_UINT8(RELAY, ev[2].mcid);
    TEST_ASSERT_EQUAL_UINT8(1, ev[2].payload[0]);

    TEST_ASSERT_EQUAL(HUBUS_OK, hubus_master_read_states(&fx.master, on_state, NULL, &count));
    TEST_ASSERT_EQUAL_UINT(3, count);
    TEST_ASSERT_EQUAL_UINT8(SENSOR_PASSIVE, states[0].mcid);
    TEST_ASSERT_EQUAL_HEX16(adc(600), (uint16_t)(states[0].payload[0] << 8 | states[0].payload[1]));
    TEST_ASSERT_EQUAL_UINT8(SWITCH, states[1].mcid);
    TEST_ASSERT_EQUAL_UINT8(1, states[1].payload[0]);
    TEST_ASSERT_EQUAL_UINT8(RELAY, states[2].mcid);
    TEST_ASSERT_EQUAL_UINT8(1, states[2].payload[0]);
    TEST_ASSERT_EQUAL_UINT8(1, output(relay, RELAY_PIN));
}

/* ---- MCID -------------------------------------------------------------------------------------------------------- */

/* One image for all devices, one EEPROM byte per device as the programmer writes it (tools/flash-avr.sh): two relays
 * from the same image under their own MCIDs. A byte without an MCID (0, above 63, erased) leaves the default. */
static void test_examples_take_their_mcid_from_the_eeprom(void)
{
    avr_slave_t *a = add(FW("ex_relay"));
    avr_slave_t *b = add(FW("ex_relay"));
    avr_slave_t *sensor = add(FW("ex_sensor"));
    avr_slave_t *sw = add(FW("ex_switch"));

    avr_slave_eeprom_write(a, -1, 30);
    avr_slave_eeprom_write(b, -1, 31);
    avr_slave_eeprom_write(sensor, -1, 0);
    avr_slave_eeprom_write(sw, -1, 64);
    boot();
    check_info(30, 0x81, 0x01);
    check_info(31, 0x81, 0x01);
    check_info(SENSOR, 0x83, 0x01);
    check_info(SWITCH, 0x82, 0x01);
    TEST_ASSERT_EQUAL(HUBUS_ERR_NO_RESPONSE, hubus_master_request_info(&fx.master, RELAY, &(hubus_slave_frame_t){0}));

    set_relay(31, 1);
    TEST_ASSERT_EQUAL_UINT8(0, output(a, RELAY_PIN));
    TEST_ASSERT_EQUAL_UINT8(1, output(b, RELAY_PIN));
}

/* The source of the MCID is up to the application: example_relay_simple_pins reads two jumpers (PB1, PB4 to ground)
 * and ignores the EEPROM byte. */
#define JUMPERS ((1u << 1) | (1u << 4))

static void test_relay_with_its_mcid_from_jumpers(void)
{
    avr_slave_t *a = add(FW("ex_relay_simple_pins"));
    avr_slave_t *b = add(FW("ex_relay_simple_pins"));

    drive(a, JUMPERS, 1u << 4); /* PB1 closed: + 1 */
    drive(b, JUMPERS, 1u << 1); /* PB4 closed: + 2 */
    avr_slave_eeprom_write(a, -1, 9);
    boot();
    check_info(0x21, 0x81, 0x02);
    check_info(0x22, 0x81, 0x02);
    TEST_ASSERT_EQUAL(HUBUS_ERR_NO_RESPONSE, hubus_master_request_info(&fx.master, 9, &(hubus_slave_frame_t){0}));

    set_relay(0x22, 1);
    TEST_ASSERT_EQUAL_UINT8(0, output(a, RELAY_PIN));
    TEST_ASSERT_EQUAL_UINT8(1, output(b, RELAY_PIN));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_relay_switches_and_reads_back);
    RUN_TEST(test_relay_without_feedback);
    RUN_TEST(test_switch_debounces_and_announces_the_button);
    RUN_TEST(test_switch_event_while_the_bus_is_busy);
    RUN_TEST(test_sensor_reports_its_measurement);
    RUN_TEST(test_passive_sensor_state);
    RUN_TEST(test_motion_sensor_announces_changes_after_the_warm_up);
    RUN_TEST(test_motion_sensor_high_after_the_warm_up);
    RUN_TEST(test_dht11_state_follows_the_sensor);
    RUN_TEST(test_dht11_faults);
    RUN_TEST(test_dht11_does_not_disturb_the_bus);
    RUN_TEST(test_distance_is_read_and_the_power_up_announced);
    RUN_TEST(test_distance_announces_an_object_below_the_threshold);
    RUN_TEST(test_distance_threshold_commands);
    RUN_TEST(test_distance_without_an_echo);
    RUN_TEST(test_distance_under_bus_traffic);
    RUN_TEST(test_examples_share_the_bus);
    RUN_TEST(test_examples_take_their_mcid_from_the_eeprom);
    RUN_TEST(test_relay_with_its_mcid_from_jumpers);
    return UNITY_END();
}

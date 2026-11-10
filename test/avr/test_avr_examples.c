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
    RUN_TEST(test_examples_share_the_bus);
    RUN_TEST(test_examples_take_their_mcid_from_the_eeprom);
    RUN_TEST(test_relay_with_its_mcid_from_jumpers);
    return UNITY_END();
}

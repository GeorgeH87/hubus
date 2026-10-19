# HuBus Slave Library (ISR-Only v0.3)

Eine konfigurierbare C-Bibliothek für ATtiny-Microcontroller als **HuBus Slave** Implementierung.

**v0.3 (ISR-Only):** Komplett interrupt-getriebene Architektur — Main-Loop ist leer.

## Merkmale

✅ **ISR-Only** — 100% interrupt-basiert, kein Polling, keine Main-Loop-Anforderungen  
✅ **Kompatibel mit großen ATtinys** — GPIO-Pins vollständig konfigurierbar für ATtiny85, ATtiny88, ATtiny1616, etc.  
✅ **Saubere API** — Nur `init()` und Daten-Management-Funktionen (`set_data()`, `signal_pending()`)  
✅ **Callback-basiert UND manuell** — Beliebig: Nutze Callbacks für Events, oder rufe manuelle Funktionen direkt auf  
✅ **Statisches Memory-Management** — Keine dynamische Speicherallokation, SRAM-Footprint vorhersagbar  
✅ **CRC8 eingebaut** — Automatische Integritätsprüfung mit konfigurierbarem Polynom  
✅ **Bit-Level Frame Parser** — Akkumuliert Bits synchron mit CLK-Kanten  
✅ **Open-Drain GPIO** — Portable Implementierung für alle AVR-Devices  

---

## Quickstart

```c
#include "lib/hubus-slave/hubus_slave.h"

void on_hubus_event(hubus_event_t event, const uint8_t *payload, uint8_t len) {
    if (event == HUBUS_EVENT_COMMAND && len > 0) {
        if (payload[0] == 0x01) {
            PORTB |= 1;  // LED ON
        } else {
            PORTB &= ~1; // LED OFF
        }
    }
}

int main(void) {
    DDRB |= 1;                           // LED als Output
    hubus_slave_init(0x05, on_hubus_event);  // MCID=5
    
    while (1) {
        // ISR läuft automatisch!
        // Main-Loop kann leer sein.
    }
}
```

**Das war's!** Alles läuft automatisch im ISR. Keine `process()` calls erforderlich.

---

## Installation

### 1. Dateistruktur

```
your_project/
├── lib/hubus-slave/       ← Diese Library
│   ├── hubus_slave_config.h
│   ├── hubus_slave.h / .c
│   ├── hubus_gpio_hal.h / .c
│   ├── hubus_crc.h / .c
│   ├── hubus_frame.h / .c
│   ├── hubus_buffer.h / .c
│   ├── hubus_bitbuffer.h / .c
│   ├── hubus_state.h / .c
│   └── README.md
└── my_app.c
```

### 2. Konfiguration (hubus_slave_config.h)

```c
// GPIO für ATtiny85:
#define HUBUS_CLK_PORT      PORTB
#define HUBUS_CLK_BIT       2
#define HUBUS_DATA_PORT     PORTB
#define HUBUS_DATA_BIT      3

// Speicher-Sparen (für ATtiny85):
#define HUBUS_CRC8_USE_BITWISE  1    // Bitwise: ~40B
                                      // Table:   256B

// ISR-Modus (immer ISR für v0.3):
#define HUBUS_USE_PCINT             1
```

### 3. Kompilieren

```bash
avr-gcc -mmcu=attiny85 -O2 \
  -I lib/hubus-slave \
  lib/hubus-slave/*.c \
  my_app.c \
  -o app.elf
```

---

## Architektur: ISR-Only

**Alte Architektur (v0.2):**
```
while (1) {
    if (hubus_gpio_clk_edge_pending()) {           // Polling
        hubus_slave_process_clk_edge();            // Kompliziert
        hubus_gpio_clk_edge_clear();
    }
    hubus_slave_process();  // Mehr Polling
}
```
❌ Main-Loop muss wissen, wie HuBus funktioniert
❌ Mehrere Funktionen zu koordinieren
❌ Komplexe Integration

**Neue Architektur (v0.3):**
```c
hubus_slave_init(0x05, callback);
while (1) {
    // ISR läuft parallel
    // Main-Loop kann leer sein!
}
```
✅ Main-Loop völlig entkoppelt
✅ Nur `init()` + Callback
✅ Saubere Integration

---

## Memory-Footprint (ATtiny85)

| Komponente | Flash | RAM |
|---|---|---|
| ISR + State Machine | ~1400 B | ~80 B |
| Bit Buffer | — | ~90 B |
| CRC8 (Bitwise) | ~40 B | — |
| GPIO HAL | ~150 B | ~10 B |
| Buffer (48B) | — | 48 B |
| **Gesamt** | **~1890 B** | **~230 B** |

**Platz:** ATtiny85 hat 8 KB Flash → ~6100 B verfügbar für App

---

## API-Referenz

### Initialisierung

```c
void hubus_slave_init(uint8_t mcid, hubus_callback_t callback);
```
- **mcid:** 6-Bit Slave-ID (0..63)
- **callback:** Event-Handler (`NULL` wenn nicht benötigt)

**Wichtig:** Aktiviert PCINT ISR automatisch!

### Daten-Management

```c
uint8_t hubus_slave_set_buffered_data(const uint8_t *data, uint8_t len);
uint8_t hubus_slave_set_status(const uint8_t *status, uint8_t len);
void hubus_slave_signal_pending(void);
```

### Debugging

```c
hubus_state_t hubus_slave_get_state(void);
const char *hubus_slave_get_state_name(void);
```

---

## Beispiel: Temperatur-Sensor

```c
#include <avr/io.h>
#include "lib/hubus-slave/hubus_slave.h"

uint8_t last_temp = 20;

void on_event(hubus_event_t event, const uint8_t *payload, uint8_t len) {
    if (event == HUBUS_EVENT_REQUEST_DATA) {
        hubus_slave_set_buffered_data(&last_temp, 1);
    }
}

int main(void) {
    hubus_slave_init(0x10, on_event);
    
    while (1) {
        // Sensor lesen (nicht blockierend!)
        _delay_ms(100);
        last_temp = read_adc();
        
        // Master holt Daten wenn nötig (im ISR)
    }
}
```

---

## Callback Events

```c
typedef enum {
    HUBUS_EVENT_REQUEST_DATA,      // Master requests data (POLL_PENDING, REQUEST_INFO, REQUEST_DATA)
    HUBUS_EVENT_COMMAND,           // Master sends command (SEND_COMMAND)
    HUBUS_EVENT_TRANSFER_COMPLETE, // Transmission successful
    // ...
} hubus_event_t;
```

**Callback läuft im ISR-Kontext!** Halte ihn kurz (< 50 µs).

---

## GPIO-Konfiguration für andere Devices

**ATtiny88 (PORTD):**
```c
#define HUBUS_CLK_PORT  PORTD
#define HUBUS_CLK_BIT   2
#define HUBUS_DATA_PORT PORTD
#define HUBUS_DATA_BIT  3
```

**ATtiny1616 (PORTC):**
```c
#define HUBUS_CLK_PORT  PORTC
#define HUBUS_CLK_BIT   4
#define HUBUS_DATA_PORT PORTC
#define HUBUS_DATA_BIT  5
```

Aktualisiere auch die DDR-Register in `hubus_gpio_hal.c`.

---

## FAQ

**Q: Läuft die Main-Loop wirklich leer?**  
A: Ja! Setzen Sie sie einfach auf `while(1) { }` oder mit App-Code, den Sie brauchen.

**Q: Kann ich andere Interrupts haben?**  
A: Ja, kein Problem. Nur CLK triggert PCINT0_vect.

**Q: Ist der Callback thread-safe?**  
A: Er läuft im ISR-Kontext. Keine Race Conditions mit Main-Loop.

**Q: Kann ich `hubus_slave_signal_pending()` vom Main-Loop aufrufen?**  
A: Ja, aber die Transition passiert auf nächster CLK-Flanke.

---

## Timing

- **CLK→ISR:** < 1 µs
- **ISR pro Bit:** ~2-5 µs
- **Callback:** ~10-50 µs (App-abhängig)
- **Frame (32 Bits):** ~200-500 µs

---

## Status

✅ **v0.3 Features:**
- ISR-only architecture
- Frame RX (Bit-Level)
- Callback system
- Static buffers
- Bitwise CRC option

⏳ **TODO (v0.4+):**
- Frame TX (Bit-Level serialization)
- Multi-slave arbitration
- Hardware validation

---

**Weitere Infos:** Siehe Hauptprojekt `/docs/PROTOCOL.md`

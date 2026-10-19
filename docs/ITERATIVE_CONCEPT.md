# Iterative Concept and Decision Log

## 1. Why Documentation-First

HUBUS starts with protocol specifications to reduce rework during firmware and hardware prototyping.

Core project goal: define a reusable communication library and protocol. Concrete device implementations are optional examples used for validation.

## 2. Confirmed Decisions

- Shared `DATA` line uses dominant-low signaling.
- Pulling `DATA` low equals bit `1`.
- Releasing `DATA` equals bit `0`.
- Higher MCID has higher arbitration priority.
- Arbitration compares MCID from MSB to LSB.
- v0 frame includes CRC8.

## 3. Open Decisions

### D1: Final MCID Width

Current default is 6 bits (0..63). Confirm whether v1 should keep 6 bits or switch to 8 bits for larger systems.

### D2: CRC8 Parameters

Freeze polynomial, init value, and final XOR after first cross-platform tests.

### D3: Fairness Strategy

Strict priority can starve low MCID nodes under sustained load. In v0 this is accepted behavior.

Future fairness extensions, if ever needed, may include:

- Priority aging.
- Deferral window for recent winners.
- Master-level scheduling policy.

## 4. Milestones

1. v0-docs: Stable core spec and examples.
2. v0-master: Raspberry Pi prototype for clock, framing, arbitration observation.
3. v0-slave: ATtiny85 prototype with arbitration and basic responses.
4. v0-multi: Multi-node stress tests and timeout tuning.
5. v1-spec: Fairness and commissioning improvements.

## 5. Deferred Topics

- Dynamic MCID assignment at runtime.
- Security/authentication on bus payloads.
- Long-distance physical layer optimization.

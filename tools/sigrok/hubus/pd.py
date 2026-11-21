##
## HUBUS protocol decoder for sigrok (PulseView, sigrok-cli).
##
## SPDX-License-Identifier: MIT
##
## Stage 1: line conditions, idle event handshake, bits, slots, frame fields,
## CRC. Burst-level analysis (pending masks) and timing checks are not part of
## it yet (docs/sigrok.md).
##

import sigrokdecode as srd

MASTER_TYPES = ('REQUEST_STATE', 'REQUEST_INFO', 'REQUEST_DATA', 'SEND_COMMAND')
SLAVE_TYPES = ('reserved 00', 'reserved 01', 'SLAVE_DATA', 'SLAVE_INFO')
SEND_COMMAND = 3
REQUEST_STATE, REQUEST_INFO, REQUEST_DATA = 0, 1, 2
SLAVE_DATA, SLAVE_INFO = 2, 3
TARGET_LEN_EXACT = 6

OPCODES = {
    0x00: 'STATUS_CAPTURE',
    0x01: 'UPDATE_ENTER',
    0x02: 'UPDATE_LOAD',
    0x03: 'UPDATE_WRITE',
    0x04: 'UPDATE_LEAVE',
    0x05: 'UPDATE_SET_MCID',
}

# Annotation classes, in the order of `annotations`.
(A_START, A_STOP, A_RESET, A_EVENT, A_BIT, A_PULSE, A_TURN, A_ARB, A_HANDOFF,
 A_FIELD, A_CRC, A_MASTER, A_SLAVE, A_SILENCE, A_WARNING) = range(15)


def crc8(data):
    '''CRC-8/AUTOSAR: polynomial 0x2F, initial value 0xFF, final XOR 0xFF.'''
    crc = 0xFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x2F) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc ^ 0xFF


def hexbytes(data):
    return ' '.join('%02X' % b for b in data)


def target_text(target_len, mcid):
    if target_len == TARGET_LEN_EXACT:
        return '%d' % mcid
    if target_len == 0:
        return 'all'
    if target_len > TARGET_LEN_EXACT:
        return 'nobody (TARGET_LEN %d)' % target_len
    bits = format(mcid, '06b')
    return 'group %s%s' % (bits[:target_len], 'x' * (6 - target_len))


class Frame:
    '''The bits of one frame as they arrive: header, payload, CRC8.'''

    def __init__(self, master):
        self.master = master
        self.bits = []  # (bit, ss, es)
        self.bytes = []
        self.len = None

    def add(self, bit, ss, es):
        self.bits.append((bit, ss, es))
        if len(self.bits) % 8 == 0:
            value = 0
            for b, _, _ in self.bits[-8:]:
                value = (value << 1) | b
            self.bytes.append(value)

    def value(self, first, count):
        v = 0
        for b, _, _ in self.bits[first:first + count]:
            v = (v << 1) | b
        return v

    def span(self, first, count):
        return self.bits[first][1], self.bits[first + count - 1][2]

    @property
    def total_bits(self):
        return None if self.len is None else 24 + 8 * self.len

    @property
    def complete(self):
        return self.total_bits is not None and len(self.bits) == self.total_bits


class Decoder(srd.Decoder):
    api_version = 3
    id = 'hubus'
    name = 'HUBUS'
    longname = 'HUBUS two-wire home bus'
    desc = 'Master-clocked bus with an open-drain data line and arbitration.'
    license = 'mit'
    inputs = ['logic']
    outputs = []
    tags = ['Embedded/industrial']
    channels = (
        {'id': 'clk', 'name': 'CLK', 'desc': 'Clock, driven by the master'},
        {'id': 'data', 'name': 'DATA', 'desc': 'Data, open drain (low = 1)'},
    )
    annotations = (
        ('start', 'START condition'),
        ('stop', 'STOP condition'),
        ('reset', 'RESET condition'),
        ('event', 'Idle event handshake'),
        ('bit', 'Bit'),
        ('pulse', 'Clock pulse outside a transaction'),
        ('turnaround', 'Turnaround slot'),
        ('arbitration', 'Arbitration slot'),
        ('handoff', 'Handoff slot'),
        ('field', 'Frame field'),
        ('crc', 'CRC8'),
        ('master-frame', 'Master frame'),
        ('slave-frame', 'Slave frame'),
        ('silence', 'No response'),
        ('warning', 'Warning'),
    )
    annotation_rows = (
        ('conditions', 'Conditions', (A_START, A_STOP, A_RESET, A_EVENT)),
        ('bits', 'Bits', (A_BIT, A_PULSE)),
        ('slots', 'Slots', (A_TURN, A_ARB, A_HANDOFF)),
        ('fields', 'Fields', (A_FIELD, A_CRC)),
        ('frames', 'Frames', (A_MASTER, A_SLAVE, A_SILENCE)),
        ('warnings', 'Warnings', (A_WARNING,)),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.rise_ss = None      # rising CLK edge of the bit in progress
        self.bit = None          # its value (1 = DATA low), None once a condition took the high phase
        self.start_ss = None     # a START in this high phase, not yet confirmed as START or RESET
        self.event_ss = None     # a slave signals an event since then
        self.event_pulses = 0
        self.state = 'IDLE'
        self.frame = None
        self.request = None      # the master frame of the transaction
        self.slot = 0
        self.n_slots = 0
        self.winner = 0

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def ann(self, ss, es, cls, texts):
        self.put(ss, es, self.out_ann, [cls, texts])

    def warn(self, ss, es, text):
        self.ann(ss, es, A_WARNING, [text])

    # ---- line events ---------------------------------------------------------------------------------------------

    def decode(self):
        clk, data = self.wait()
        if not clk and not data:
            # The capture begins while DATA is low: a slave that already signals an event, or a transaction in progress.
            self.event_ss = self.samplenum
            self.event_pulses = 0
        while True:
            pclk, pdata = clk, data
            clk, data = self.wait([{0: 'e'}, {1: 'e'}])
            if clk != pclk and data != pdata:
                # Edges in the same sample: DATA changes while CLK is low, so it goes after a falling and before a
                # rising edge.
                if pclk:
                    self.clk_fall()
                    self.data_change_low(data)
                else:
                    self.data_change_low(data)
                    self.clk_rise(data)
            elif clk != pclk:
                if clk:
                    self.clk_rise(data)
                else:
                    self.clk_fall()
            elif data != pdata:
                if clk:
                    self.condition(data)
                else:
                    self.data_change_low(data)

    def clk_rise(self, data):
        self.rise_ss = self.samplenum
        self.bit = 0 if data else 1

    def clk_fall(self):
        now = self.samplenum
        if self.start_ss is not None:
            self.ann(self.start_ss, now, A_START, ['START', 'S'])
            self.begin_transaction(self.start_ss)
            self.start_ss = None
        elif self.bit is not None and self.rise_ss is not None:
            self.on_bit(self.bit, self.rise_ss, now)
        self.bit = None

    def condition(self, data):
        '''DATA changes while CLK is high: START when it falls, STOP when it rises.'''
        now = self.samplenum
        self.bit = None
        if not data:
            self.start_ss = now
            return
        if self.start_ss is not None:
            self.end_transaction(self.start_ss, 'RESET')
            self.ann(self.start_ss, now, A_RESET, ['RESET', 'R'])
            self.start_ss = None
        else:
            self.end_transaction(now, 'STOP')
            self.ann(now, now, A_STOP, ['STOP', 'P'])

    def data_change_low(self, data):
        '''DATA changes while CLK is low: the next bit, or a slave that signals an event while the bus is idle.'''
        if self.state != 'IDLE':
            return
        now = self.samplenum
        if not data:
            self.event_ss = now
            self.event_pulses = 0
        elif self.event_ss is not None:
            self.ann(self.event_ss, now, A_EVENT,
                     ['Event signaled, released after %d pulse%s' % (self.event_pulses,
                                                                     '' if self.event_pulses == 1 else 's'),
                      'Event', 'E'])
            self.event_ss = None

    # ---- transactions --------------------------------------------------------------------------------------------

    def begin_transaction(self, ss):
        if self.state not in ('IDLE', 'END'):
            self.incomplete(ss, 'START')
        if self.event_ss is not None:
            self.warn(self.event_ss, ss, 'Event signal still held at START')
            self.event_ss = None
        self.state = 'MASTER'
        self.frame = Frame(master=True)
        self.request = None

    def end_transaction(self, ss, how):
        if self.state == 'IDLE':
            return
        if self.state not in ('END',):
            self.incomplete(ss, how)
        self.state = 'IDLE'
        self.frame = None

    def incomplete(self, ss, how):
        f = self.frame
        if f is not None and f.bits:
            total = f.total_bits
            self.warn(f.bits[0][1], ss, '%s frame incomplete at %s: %d of %s bits' %
                      ('Master' if f.master else 'Slave', how, len(f.bits), total if total else '24+'))
        elif self.state in ('TURN', 'ARB', 'FIRST'):
            self.warn(ss, ss, '%s before the response' % how)

    def on_bit(self, bit, ss, es):
        if self.state == 'IDLE':
            self.ann(ss, es, A_PULSE, ['Pulse', 'P'])
            if self.event_ss is not None:
                self.event_pulses += 1
            return
        self.ann(ss, es, A_BIT, ['%d' % bit])
        if self.state in ('MASTER', 'SLAVE'):
            self.frame_bit(bit, ss, es)
        elif self.state == 'TURN':
            self.ann(ss, es, A_TURN, ['Turnaround', 'T'])
            req = self.request
            self.n_slots = 2 * (6 - req['target_len']) if req['target_len'] < TARGET_LEN_EXACT else 0
            self.slot = 0
            self.winner = req['mcid'] >> (6 - req['target_len']) if req['target_len'] < TARGET_LEN_EXACT else 0
            self.state = 'ARB' if self.n_slots else 'FIRST'
        elif self.state == 'ARB':
            k = 5 - self.request['target_len'] - self.slot // 2
            if self.slot % 2 == 0:
                self.ann(ss, es, A_ARB, ['Arbitration bit %d: %d' % (k, bit), 'A%d=%d' % (k, bit), '%d' % bit])
                self.winner = (self.winner << 1) | bit
            else:
                self.ann(ss, es, A_HANDOFF, ['Handoff %d: lost here' % k if bit else 'Handoff %d' % k,
                                             'H%d%s' % (k, '!' if bit else ''), 'H'])
            self.slot += 1
            if self.slot == self.n_slots:
                self.state = 'FIRST'
        elif self.state == 'FIRST':
            if not bit:
                self.ann(ss, es, A_SILENCE, ['No response', 'None', '-'])
                self.state = 'END'
            else:
                self.state = 'SLAVE'
                self.frame = Frame(master=False)
                self.frame_bit(bit, ss, es)
        else:
            self.warn(ss, es, 'Clock pulse after the end of the frame')

    # ---- frames --------------------------------------------------------------------------------------------------

    def field(self, first, count, texts):
        ss, es = self.frame.span(first, count)
        self.ann(ss, es, A_FIELD, texts)

    def frame_bit(self, bit, ss, es):
        f = self.frame
        f.add(bit, ss, es)
        n = len(f.bits)
        if n == 16:
            self.header()
        elif n > 16 and n % 8 == 0 and not f.complete:
            i = n // 8 - 3
            b = f.bytes[-1]
            if f.master and self.frame_type == SEND_COMMAND and i == 0:
                name = OPCODES.get(b, 'reserved' if b < 0x10 else 'application')
                self.field(n - 8, 8, ['Opcode 0x%02X (%s)' % (b, name), 'Op %02X' % b, '%02X' % b])
            else:
                self.field(n - 8, 8, ['Payload %d: 0x%02X' % (i, b), '%02X' % b])
        if f.complete:
            self.frame_end()

    def header(self):
        f = self.frame
        t = f.value(0, 2)
        self.frame_type = t
        f.len = f.value(11, 5)
        if f.master:
            tl = f.value(2, 3)
            mcid = f.value(5, 6)
            self.field(0, 2, ['Type: %s' % MASTER_TYPES[t], MASTER_TYPES[t], 'T'])
            self.field(2, 3, ['TARGET_LEN: %d' % tl, 'TL %d' % tl, '%d' % tl])
            self.field(5, 6, ['Target: %s' % target_text(tl, mcid), target_text(tl, mcid)])
            self.request = {'type': t, 'target_len': tl, 'mcid': mcid}
        else:
            more = f.value(2, 1)
            gen = f.value(3, 2)
            mcid = f.value(5, 6)
            self.field(0, 2, ['Type: %s' % SLAVE_TYPES[t], SLAVE_TYPES[t], 'T'])
            self.field(2, 1, ['MORE: %d' % more, 'M%d' % more])
            self.field(3, 2, ['GEN: %d' % gen, 'G%d' % gen])
            self.field(5, 6, ['Source: %d' % mcid, '%d' % mcid])
        self.field(11, 5, ['LEN: %d' % f.len, 'L%d' % f.len, '%d' % f.len])

    def frame_end(self):
        f = self.frame
        covered = f.bytes[:-1]
        crc = f.bytes[-1]
        expected = crc8(covered)
        ok = crc == expected
        n = len(f.bits)
        crc_ss, crc_es = f.span(n - 8, 8)
        if ok:
            self.ann(crc_ss, crc_es, A_CRC, ['CRC 0x%02X ok' % crc, 'CRC ok', 'C'])
        else:
            self.ann(crc_ss, crc_es, A_CRC, ['CRC 0x%02X wrong' % crc, 'CRC!', 'C!'])
            self.warn(crc_ss, crc_es, 'CRC wrong: 0x%02X, expected 0x%02X' % (crc, expected))
        ss, es = f.bits[0][1], f.bits[-1][2]
        payload = f.bytes[2:-1]
        suffix = '' if ok else ' (CRC wrong)'
        if f.master:
            self.master_frame_end(ss, es, payload, suffix)
        else:
            self.slave_frame_end(ss, es, payload, suffix)

    def master_frame_end(self, ss, es, payload, suffix):
        req = self.request
        t = req['type']
        target = target_text(req['target_len'], req['mcid'])
        if t == SEND_COMMAND:
            if payload:
                name = OPCODES.get(payload[0])
                op = name if name else '0x%02X' % payload[0]
                args = (': ' + hexbytes(payload[1:])) if len(payload) > 1 else ''
                text = 'SEND_COMMAND %s to %s%s' % (op, target, args)
            else:
                text = 'SEND_COMMAND to %s without opcode (ignored)' % target
            self.state = 'END'
        else:
            text = '%s to %s' % (MASTER_TYPES[t], target)
            if payload:
                self.warn(ss, es, 'Request with LEN %d (reserved, ignored by slaves)' % len(payload))
            self.state = 'TURN'
        self.ann(ss, es, A_MASTER, [text + suffix, MASTER_TYPES[t]])

    def slave_frame_end(self, ss, es, payload, suffix):
        f = self.frame
        req = self.request
        t = self.frame_type
        mcid = f.value(5, 6)
        more = f.value(2, 1)
        gen = f.value(3, 2)
        if t == SLAVE_INFO:
            text = 'SLAVE_INFO from %d' % mcid
            if payload:
                text += ': protocol %d, speed class %d' % (payload[0] >> 4, payload[0] & 0x0F)
                if len(payload) > 1:
                    text += ', HWID ' + hexbytes(payload[1:])
        elif t == SLAVE_DATA:
            kind = 'state' if req['type'] == REQUEST_STATE else 'event'
            text = 'SLAVE_DATA (%s) from %d' % (kind, mcid)
            if kind == 'event':
                text += ', GEN %d%s' % (gen, ', MORE' if more else '')
            if payload:
                text += ': ' + hexbytes(payload)
        else:
            text = 'Slave frame of reserved type %d from %d' % (t, mcid)
            self.warn(ss, es, 'Reserved slave type %d' % t)
        expected_type = SLAVE_INFO if req['type'] == REQUEST_INFO else SLAVE_DATA
        if t in (SLAVE_DATA, SLAVE_INFO) and t != expected_type:
            self.warn(ss, es, '%s answers %s' % (SLAVE_TYPES[t], MASTER_TYPES[req['type']]))
        if req['target_len'] == TARGET_LEN_EXACT:
            expected_mcid = req['mcid']
        else:
            expected_mcid = self.winner if req['target_len'] < TARGET_LEN_EXACT else None
        if expected_mcid is not None and mcid != expected_mcid:
            self.warn(ss, es, 'Source %d, expected %d' % (mcid, expected_mcid))
        self.ann(ss, es, A_SLAVE, [text + suffix, SLAVE_TYPES[t]])
        self.state = 'END'

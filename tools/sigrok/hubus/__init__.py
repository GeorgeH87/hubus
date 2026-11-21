##
## HUBUS protocol decoder for sigrok (PulseView, sigrok-cli).
##
## SPDX-License-Identifier: MIT
##

'''
HUBUS is a two-wire bus: a master-driven clock (CLK) and an open-drain data
line (DATA) that every node pulls low or releases. This decoder shows the line
conditions (START, STOP, RESET), the idle event handshake, the bits, the
turnaround and arbitration slots, the fields of master and slave frames, and
checks the CRC-8/AUTOSAR of every frame.

The specification is in docs/spec/ of the HUBUS repository; the decoder is
described in docs/sigrok.md.
'''

from .pd import Decoder

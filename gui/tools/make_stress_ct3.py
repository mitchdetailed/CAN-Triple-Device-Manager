"""Generate a maximum-capacity stress configuration: 500 transmit messages,
no receive, every calculation table full except conditions.

WHY 500 TRANSMIT AND ONE CHANNEL EACH. The device has 500 message slots and
1000 signal slots, and a transmit row costs a signal slot of its own on top of
the channel it carries. The budget is therefore:

    500 transmit rows        500 signals
    424 calculation outputs  424 signals   (100 math + 100 conditions +
                                            50 counters + 50 timers +
                                            100 constants + 8 + 8 tables +
                                            8 integrators)
     36 device channels       36 signals   (mapped on every Send since 2.4)
                             ---
                             960 of 1000

which is why each message carries exactly ONE channel. Two would need 1000
signals for the rows alone and the configuration would not map. The frames are
still 8 bytes: messageLength is what sets the DLC, and a 32-bit channel in an
8-byte message leaves the rest zero -- which is the point, since the goal is to
prove 500 EIGHT-BYTE messages go out on time.

WHY N_COND IS 100 AND NOT THE 250 THE DEVICE NOW HOLDS. Store v10 raised the
condition table to 250, and the signal table did NOT grow with it -- 1000 is
still 1000, and 64 bytes a slot is what stopped it growing. So "every table
full at once" stopped being representable: 250 conditions would put the budget
above at 1110 of 1000 and the configuration would simply fail to map.

That is not a defect in this script, it is the shape of the device now, and it
is worth stating rather than quietly picking a smaller number: the SIGNAL table
is the binding constraint on the calculation side, and any table can be filled
to its own capacity only by leaving room somewhere else. 100 conditions is the
share that keeps the 500-message transmit load -- the thing this file exists to
measure -- intact.

WHY THE MESSAGES CARRY CALCULATED CHANNELS rather than channels of their own.
A channel nothing generates is a channel the device transmits as zero for ever,
and validation calls it out. Sourcing every message from a calculation means the
whole chain runs under load: constants feed math, math feeds conditions and
tables, conditions gate counters, timers and integrators, and all of it is read
back out by the transmit composer. Nothing in the document is decorative.

The chain is rooted in CONSTANTS deliberately: with no receive messages there is
nothing else that generates a value from outside, so a stress test built on
received channels would be one where every input reads zero and every branch
takes the same path for ever.

    python make_stress_ct3.py [out.ct3]
"""

import json
import sys

N_TX = 500
N_MATH = 100
N_COND = 100
N_CNT = 50
N_TMR = 50
N_CONST = 100
N_T2 = 8
N_T8 = 8
N_INTG = 8
BASE_ID = 0x100
RATE_HZ = 10
MSG_BYTES = 8

K = [f"K_Const_{i:03d}" for i in range(N_CONST)]
M = [f"M_Math_{i:03d}" for i in range(N_MATH)]
C = [f"C_Cond_{i:03d}" for i in range(N_COND)]
N = [f"N_Cnt_{i:03d}" for i in range(N_CNT)]
R = [f"R_Tmr_{i:03d}" for i in range(N_TMR)]
T2 = [f"T2_Tbl_{i}" for i in range(N_T2)]
T8 = [f"T8_Tbl_{i}" for i in range(N_T8)]
I = [f"I_Intg_{i}" for i in range(N_INTG)]


def channel(name, category, dtype, decimals, lo, hi, quantity="Unitless", unit=""):
    return {
        "baseResolution": 1,
        "category": category,
        "dataType": dtype,
        "decimalPlaces": decimals,
        "maxValue": hi,
        "minValue": lo,
        "name": name,
        "quantity": quantity,
        "unit": unit,
    }


def build():
    user_channels = []
    user_channels += [channel(n, "Constants", "u16", 0, 0, 65535) for n in K]
    user_channels += [channel(n, "Math", "float", 3, -1000000, 1000000) for n in M]
    user_channels += [channel(n, "Conditions", "boolean", 0, 0, 1) for n in C]
    user_channels += [channel(n, "Counters", "u16", 0, 0, 65535) for n in N]
    user_channels += [channel(n, "Timers", "float", 2, 0, 65535, "Time", "s") for n in R]
    user_channels += [channel(n, "Tables", "float", 2, -1000000, 1000000) for n in T2 + T8]
    user_channels += [channel(n, "Integrators", "float", 3, 0, 1000000) for n in I]

    # Constants: the roots. Nothing upstream of them, which is what makes the
    # rest of the chain evaluate to something other than zero.
    constants = [{"active": True, "dataType": "u16", "decimals": 0,
                  "name": K[i], "value": (i * 37) % 65536} for i in range(N_CONST)]

    # Math: each reads two constants, so every row has live inputs. Ops are
    # cycled across the first six so the evaluator is not running one opcode
    # five hundred times.
    math = [{"aChannel": K[i % N_CONST], "aConst": 0, "aIsChannel": True,
             "active": True,
             "bChannel": K[(i + 1) % N_CONST], "bConst": 0, "bIsChannel": True,
             "cChannel": "", "cConst": 0, "cIsChannel": False,
             "dest": M[i], "op": i % 6} for i in range(N_MATH)]

    # Conditions: compare a math output against a threshold. op cycles over the
    # comparison set for the same reason.
    conditions = [{"active": True, "joiners": [],
                   "outputChannel": C[i],
                   "terms": [{"aChannel": M[i % N_MATH], "bChannel": "",
                              "bConst": (i * 13) % 500, "bIsChannel": False,
                              "op": i % 6}]} for i in range(N_COND)]

    counters = [{"active": True,
                 "down": C[(3 * i + 1) % N_COND], "enable": C[(3 * i + 2) % N_COND],
                 "follow": "", "max": 65535, "min": 0, "mode": 0,
                 # The device retains at most 20 preserved values across a power
                 # cycle, so preserve is on for 12 counters and all 8
                 # integrators -- exactly the budget, deliberately spent rather
                 # than left unused.
                 "output": N[i], "preserveValue": i < 12, "rateCountDown": True,
                 "rateHz": 1, "reset": C[(3 * i + 3) % N_COND], "resetValue": 0,
                 "rollAtLimits": True, "step": 1,
                 "up": C[(3 * i) % N_COND]} for i in range(N_CNT)]

    # A down-counting timer has to START somewhere above its floor or it is
    # already finished; same for the integrators below. Half of each count down
    # so both directions are exercised.
    timers = [{"active": True, "countDown": bool(i % 2), "limit": 60,
               "output": R[i], "rollover": True, "setOnStart": False,
               "setOnStop": True, "start": C[(2 * i) % N_COND],
               "startValue": 60 if i % 2 else 0,
               "stop": C[(2 * i + 1) % N_COND],
               "stopValue": 0} for i in range(N_TMR)]

    tables2x16 = [{"active": True, "dataType": "float", "decimals": 2,
                   "output": T2[i],
                   "outputs": [round(j * 1.5 + i, 2) for j in range(16)],
                   "xChannel": M[(i * 7) % N_MATH], "xInterp": True,
                   "xSites": [j * 500 for j in range(16)]} for i in range(N_T2)]

    tables8x8 = [{"active": True, "dataType": "float", "decimals": 2,
                  "output": T8[i],
                  "outputs": [round(j * 0.5 + i, 2) for j in range(64)],
                  "xChannel": M[(i * 11) % N_MATH], "xInterp": True,
                  "xSites": [j * 1000 for j in range(8)],
                  "yChannel": M[(i * 13 + 1) % N_MATH], "yInterp": True,
                  "ySites": [j * 10 for j in range(8)]} for i in range(N_T8)]

    integrators = [{"active": True, "countDown": bool(i % 2),
                    "enable": C[(2 * i) % N_COND],
                    "inputChannel": M[(i * 5) % N_MATH], "inputIsChannel": True,
                    "inputValue": 0, "max": 1000000, "min": 0,
                    "output": I[i], "preserveValue": True, "rateHz": 1,
                    "reset": C[(2 * i + 1) % N_COND], "resetValue": 0,
                    "startValue": 1000000 if i % 2 else 0}
                   for i in range(N_INTG)]

    # Every calculated channel, in the order the transmit messages will cycle
    # through them, so the 500 messages spread evenly over the 424 sources
    # rather than hammering the first few.
    sources = M + C + N + R + T2 + T8 + I + K
    sections = []
    for i in range(N_TX):
        sections.append({
            "alignment": "wordSwap",
            "baseAddress": format(BASE_ID + i, "X"),
            "channels": [{
                "bitLength": 32,
                "channel": sources[i % len(sources)],
                "dbcFactor": 1,
                "dbcOffset": 0,
                "dbcType": 2,
                "defaultValue": 0,
                "startBit": 0,
            }],
            "compound": False,
            "compoundTxMode": "batch",
            "cyclic": True,
            "defaultValueOnTimeout": True,
            "device": "transmit",
            "diagnosticChannel": "",
            "extended": False,
            "fd": False,
            "identifiers": [],
            "messageLength": MSG_BYTES,
            "name": f"TX_{i:03d}",
            "receiveTimeoutMs": 2200,
            "relayBitmask": "0",
            "relayInvert": False,
            "routeBusMask": 0,
            "routeEnable": False,
            "transmitPeriodMs": 0,
            "transmitRateHz": RATE_HZ,
        })

    return {
        "accessVerifiers": {},
        "buses": [
            {"dataRateKbps": 0, "enabled": True, "rateKbps": 1000,
             "termination": True, "sections": sections},
            {"dataRateKbps": 0, "enabled": False, "rateKbps": 1000,
             "termination": False, "sections": []},
            {"dataRateKbps": 0, "enabled": False, "rateKbps": 1000,
             "termination": False, "sections": []},
        ],
        "comments": (
            f"Maximum-capacity transmit stress test.\n\n"
            f"{N_TX} cyclic transmit messages at {RATE_HZ} Hz on CAN1, "
            f"{MSG_BYTES} bytes each, no receive messages. Every calculation "
            f"table is full and every transmit message is sourced from one of "
            f"them, so the whole evaluation chain runs at load rather than the "
            f"transmit composer running over static values.\n\n"
            f"Offered bus load is about 65% of 1 Mbit/s. Generated by "
            f"tools/make_stress_ct3.py."),
        "conditions": conditions,
        "configTitle": "Max Transmit Stress - 500 messages",
        "constants": constants,
        "counters": counters,
        "fileType": "CANTripleConfig",
        "fileVersion": 16,
        "fleetIdentity": {"configVersion": 0, "flags": 0, "modelId": "",
                          "serialNumber": 0, "vendorId": ""},
        "integrators": integrators,
        "math": math,
        "tables2x16": tables2x16,
        "tables8x8": tables8x8,
        "timers": timers,
        "uploadPolicy": {"allowedSerials": [], "requireFleetKey": True,
                         "warnOnOlderVersion": True},
        "userChannels": user_channels,
    }


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "stress_500_transmit.ct3"
    doc = build()
    with open(out, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=4, sort_keys=True)
        f.write("\n")
    calc = N_MATH + N_COND + N_CNT + N_TMR + N_CONST + N_T2 + N_T8 + N_INTG
    print(f"wrote {out}")
    print(f"  {N_TX} transmit messages, 0 receive")
    print(f"  {calc} calculated channels -> {N_TX} + {calc} + 36 device "
          f"= {N_TX + calc + 36} of 1000 signals")

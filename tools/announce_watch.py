#!/usr/bin/env python3
"""Watch device_announce cadence and shout when a light is in a reset loop.

This is the abort gate for an OTA test. It is READ-ONLY: it subscribes to
zigbee2mqtt/bridge/event and never publishes anything.

Why it exists
-------------
On 2026-08-14 a converted light booted, joined and interviewed perfectly, and
was recorded as a complete success. It was in fact resetting every ~11 seconds
and could never be updated again. The only signal that distinguished it from a
healthy light was the *rate* of device_announce - 7 in 4 minutes.

A healthy mains-powered router announces once when it joins and then goes
quiet, essentially forever. Any repeat is a reboot.

Thresholds (see OTA_TEST_PLAN.md)
---------------------------------
  >= 2 announces within the first 2 minutes  -> ABORT, it is looping
  >  1 announce  within 15 minutes           -> ABORT, it is looping
  exactly 1 announce, then silence           -> healthy so far, keep soaking

Usage
-----
  # watch one light for 15 minutes (the soak gate)
  ./announce_watch.py --ieee 0xa4c138…c03e --minutes 15

  # watch everything (useful while a rollout is running)
  ./announce_watch.py --minutes 30

  # if mosquitto runs in a container on the z2m host
  ./announce_watch.py --mqtt-cmd 'docker exec mosquitto mosquitto_sub -h localhost'

Exit status: 0 = healthy for the whole window, 2 = abort criteria met,
1 = could not watch (no broker, no messages at all).
"""

import argparse
import json
import shlex
import subprocess
import sys
import time

ABORT_EARLY_WINDOW_S = 120     # "within the first two minutes"
ABORT_EARLY_COUNT = 2          # this many announces in that window = looping
ABORT_SOAK_COUNT = 2           # more than one in the whole window = looping


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--ieee", help="only report this device (0x...); default: all")
    ap.add_argument("--fresh-join", action="store_true",
                    help="conversion watch: also count device_joined and "
                         "successful device_interview events as liveness")
    ap.add_argument("--minutes", type=float, default=15.0,
                    help="how long to watch (default 15)")
    ap.add_argument("--mqtt-cmd", default="mosquitto_sub -h localhost",
                    help="command that subscribes; the topic and -v are appended")
    ap.add_argument("--topic", default="zigbee2mqtt/bridge/event")
    args = ap.parse_args()

    cmd = shlex.split(args.mqtt_cmd) + ["-t", args.topic]
    started = time.time()
    deadline = started + args.minutes * 60
    counts = {}
    liveness = {}
    saw_anything = False
    verdict = 0

    print(f"watching {args.topic} for {args.minutes:g} min"
          + (f", device {args.ieee}" if args.ieee else ", all devices"))
    print(f"abort if: >={ABORT_EARLY_COUNT} announces in the first "
          f"{ABORT_EARLY_WINDOW_S}s, or >{ABORT_SOAK_COUNT - 1} in the whole window\n")

    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True, bufsize=1)
    except FileNotFoundError:
        print(f"cannot run: {' '.join(cmd)}", file=sys.stderr)
        return 1

    try:
        while time.time() < deadline:
            remaining = deadline - time.time()
            proc.stdout.flush() if proc.stdout else None
            line = proc.stdout.readline() if proc.stdout else ""
            if not line:
                if proc.poll() is not None:
                    print("subscriber exited early", file=sys.stderr)
                    break
                continue

            saw_anything = True
            try:
                evt = json.loads(line)
            except ValueError:
                continue
            evt_type = evt.get("type")
            data = evt.get("data") or {}
            if evt_type == "device_announce":
                is_announce = True
            elif args.fresh_join and evt_type == "device_joined":
                is_announce = False
            elif (args.fresh_join and evt_type == "device_interview"
                  and data.get("status") == "successful"):
                is_announce = False
            else:
                continue

            ieee = data.get("ieee_address", "?")
            name = data.get("friendly_name", ieee)
            if args.ieee and ieee.lower() != args.ieee.lower():
                continue

            elapsed = time.time() - started
            if args.fresh_join:
                liveness[ieee] = liveness.get(ieee, 0) + 1

            if is_announce:
                counts[ieee] = counts.get(ieee, 0) + 1
                n = counts[ieee]
                print(f"  [{elapsed:6.1f}s] device_announce #{n}  {name} ({ieee})")

                if elapsed <= ABORT_EARLY_WINDOW_S and n >= ABORT_EARLY_COUNT:
                    print(f"\n*** ABORT: {n} announces in the first {elapsed:.0f}s. "
                          f"{name} is in a reset loop.")
                    print("*** Cut power to that fixture now. Do not touch another light.")
                    verdict = 2
                    break
                if n >= ABORT_SOAK_COUNT:
                    print(f"\n*** ABORT: {n} announces in {elapsed / 60:.1f} min. "
                          f"{name} is rebooting.")
                    verdict = 2
                    break
            else:
                label = (evt_type if evt_type == "device_joined"
                         else "device_interview(successful)")
                n = liveness[ieee]
                print(f"  [{elapsed:6.1f}s] {label} #{n}  {name} ({ieee})")
            _ = remaining
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    if verdict == 2:
        return 2

    if not saw_anything:
        print("\nno messages at all - is the broker reachable and is z2m publishing?")
        return 1

    print()
    if args.fresh_join:
        if args.ieee and not any(k.lower() == args.ieee.lower() for k in liveness):
            print(f"no liveness signal (device_joined, successful device_interview, "
                  f"or device_announce) for {args.ieee} in {args.minutes:g} min.")
            print("*** ABORT: the light did not come back after the conversion OTA.")
            return 2

        for ieee, n in sorted(liveness.items()):
            print(f"{ieee}: {n} liveness event(s) in {args.minutes:g} min")
        for ieee, n in sorted(counts.items()):
            print(f"{ieee}: {n} announce(s) in {args.minutes:g} min")
        print("\nFresh-join conversion: z2m seeing the device join and interview "
              "successfully is the healthy signal. PASS.")
        return 0

    if not counts:
        print(f"no device_announce in {args.minutes:g} min.")
        print("If the light was expected to (re)join in this window, that is its own "
              "problem - it did not come back. If it was already joined and settled, "
              "this is the healthy result.")
    else:
        for ieee, n in sorted(counts.items()):
            print(f"{ieee}: {n} announce(s) in {args.minutes:g} min")
        print("\nOne announce then silence is what a healthy light looks like. PASS.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

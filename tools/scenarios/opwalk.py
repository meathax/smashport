#!/usr/bin/env python3
"""Operator-menu session (PLAN P5 scripted scenarios): a legal input schedule
through the test menu, built from explicit phases whose screens were checked
on native snapshots (cursor positions are deterministic).

  python tools/scenarios/opwalk.py [--checkpoints] > tests/scenarios/operator_walk.inputs

Menu rules (TEST.ASM GETSTICK): the stick moves the cursor (clamped, no wrap);
start activates; about 14 stick hits in one menu session count as a stuck
stick. Tests are left with P1+P2 START together (switch test) or any button.
--checkpoints prints "frame label" lines (for --snap) instead of inputs.

IN0: P1 up 0x1, down 0x2; IN1: test switch 0x10, start1 0x4, start2 0x20.
"""
import sys

ev, marks = [], []
t = 3000


def pulse(tag, mask, on=6, gap=24):
    global t
    ev.append((t, tag, mask, 1))
    ev.append((t + on, tag, mask, 0))
    t += on + gap


def wait(n):
    global t
    t += n


def mark(label):
    marks.append((t - 10, label))


def down(n=1):
    wait(60)                                  # switches held on entry are latched: let them settle
    for _ in range(n):
        pulse(":IN0", 0x2)


def activate():
    wait(60)                                  # a press right after stick moves is not taken
    pulse(":IN1", 0x4, on=20, gap=30)


def leave():
    """P1 START and P2 START together."""
    global t
    for mask in (0x4, 0x20):
        ev.append((t, ":IN1", mask, 1))
        ev.append((t + 30, ":IN1", mask, 0))
    t += 60


pulse(":IN1", 0x10, on=10, gap=300)
mark("main menu")
# diagnostics: switch, CPU board, sound board tests, monitor patterns
activate(); wait(240); mark("diagnostics")
activate(); wait(300); mark("switch test")
leave(); wait(240); mark("back in diagnostics")
# (the CPU board test ends by restarting the game, so it comes last)
down(2); activate(); wait(2400); mark("sound board test")
leave(); wait(240); mark("after sound test")
down(); activate(); wait(240); mark("monitor patterns")
for k in range(5):
    if k:
        down()
    activate(); wait(120); mark(f"pattern {k}")
    leave(); wait(120)
mark("patterns menu again")
down(); activate(); wait(240); mark("back to diagnostics")
down(2); activate(); wait(240); mark("main menu from diagnostics")
# coin bookkeeping and audits pages
down(); activate(); wait(240); mark("bookkeeping")
activate(); wait(240); mark("main after bookkeeping")
down(); activate(); wait(240); mark("audits")
activate(); wait(240); mark("audits after one activate")
activate(); wait(240); mark("audits after two")

for f, tag, mask, v in sorted(ev):
    if "--checkpoints" not in sys.argv:
        print(f"{f} {tag} {mask:x} {v}")
if "--checkpoints" in sys.argv:
    for f, label in marks:
        print(f, label)

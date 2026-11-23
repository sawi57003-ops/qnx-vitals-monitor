# QNX Real-Time Patient Monitor

A multi-rate periodic task set for **QNX 8.0** that simulates a bedside
patient monitor — and proves, with measured numbers, that its
safety-critical threads keep their deadlines even when the CPU is
saturated by low-priority work.

Built for the **QNX Global Hackathon 2026** (Robotics & Medical).

## What it does

Six `SCHED_FIFO` threads with explicit priorities:

| Thread | Period | Prio | Job |
|--------|--------|------|-----|
| alarm | 100 ms | 22 | Checks HR / SpO2 / NIBP against limits, raises alarms |
| ecg | 10 ms | 20 | Samples a synthetic ECG waveform (100 Hz), derives HR from R-R intervals |
| spo2 | 20 ms | 18 | Samples a synthetic SpO2 signal (50 Hz) |
| nibp | 15 s | 12 | Intermittent blood-pressure reading |
| display | 1 s | 10 | Prints the live dashboard on the serial console |
| load | busy | 5 | CPU hog, switched on mid-demo to prove priority isolation |

Every periodic thread sleeps with `clock_nanosleep(CLOCK_MONOTONIC,
TIMER_ABSTIME)` and measures its own wake-up jitter with `ClockCycles()`.
A deadline is missed when a thread wakes up more than one full period
late. The summary printed at the end is the proof.

## Demo timeline (90 s)

- **0–20 s** — clean run, no load
- **20–60 s** — CPU load ON (the prio-5 hog saturates the CPU)
- **45–60 s** — tachycardia event: HR jumps to ~132 bpm, the alarm must fire
- **60–90 s** — load OFF, vitals recover

## Result (QEMU x86_64, no KVM)

Measured on QNX 8.0 in QEMU (TCG emulation, 2 vCPUs, no KVM — the host
has no `/dev/kvm`):

```
thread   period   activations   avg jitter   max jitter   misses
alarm     100 ms         902      1127 us      4460 us        0
ecg        10 ms        9006       452 us     33003 us       22
spo2       20 ms        4504       288 us     36275 us        6
nibp      15000 ms           8       852 us      1183 us        0
display  1000 ms          92      1102 us     25143 us        0
```

The safety-critical alarm thread (100 ms, prio 22) missed **0 deadlines**
in 902 activations while the CPU was saturated. The high-rate sampler
threads showed occasional late wake-ups under full load (max ~33 ms) —
expected in TCG emulation without KVM, where the emulator itself, not
QNX scheduling, is the bottleneck — and exactly why the alarm path
outranks them.

Full serial log: [`demo-90s.log`](demo-90s.log)

## Build

Requires QNX SDP 8.0 (`qcc`).

```sh
source ~/qnx/sdp-8.0/qnxsdp-env.sh
qcc -Vgcc_ntox86_64 -o vitals-x86_64 vitals.c -lm
```

## Run in QEMU

The easiest way is [qnxrun](https://github.com/sawi57003-ops/qnxrun)
(one-command QNX 8.0 image builder + QEMU runner):

```sh
qnxrun build --binary ./vitals-x86_64 --project-dir ./qnx-image
qnxrun run --project-dir ./qnx-image --timeout 150
```

Or by hand with `mkqnximage` (see `qnxrun`'s README for the gotchas:
the stock `runimage` needs brctl + a setuid bridge helper).

Run the binary directly with a custom demo length in seconds:

```sh
vitals-x86_64 60
```

## Why QNX

A patient monitor is the textbook case for a hard real-time OS: the
alarm path must fire on time no matter what else the system is doing.
QNX gives that guarantee structurally — fixed-priority preemptive
scheduling with priority inheritance — and this demo measures it instead
of asserting it: jitter and deadline misses are counted per thread and
printed, before / during / after a CPU-saturation phase.

## License

MIT — see [LICENSE](LICENSE).

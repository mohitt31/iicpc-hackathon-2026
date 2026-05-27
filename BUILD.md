# BUILD.md — Dependencies and Build Instructions

If a judge runs `cmake .. && make -j` on a fresh checkout without the right dependencies installed, the build fails before showing what we built. This file is the install guide.

---

## Requirements

| Component | Version | Why |
|---|---|---|
| C++ compiler | C++20-capable (g++ 10+ or clang 12+) | Designated initializers, `<concepts>`, `<bit>` |
| CMake | 3.16+ | Project build system |
| `hdr_histogram_c` | 0.11+ | HDR latency histograms with CO correction |
| Python 3.8+ | (any) | Demo input generator, integration test wrapper |
| GNU make | (any) | CMake's default backend |

Optional but recommended:

| Component | Why |
|---|---|
| `valgrind` (Linux) | Soak test's leak check uses it if available |
| `leaks` (macOS) | Bundled with Xcode CLT — soak test's leak check uses it |

---

## Install instructions per platform

### Ubuntu / Debian

```bash
sudo apt update
sudo apt install -y \
    build-essential cmake g++ git python3 \
    libhdrhistogram-dev valgrind

# If libhdrhistogram-dev isn't in your repo (some Ubuntu versions):
sudo apt install -y autoconf libtool
git clone https://github.com/HdrHistogram/HdrHistogram_c.git /tmp/hdr_c
cd /tmp/hdr_c
mkdir build && cd build
cmake .. && sudo make install
sudo ldconfig
```

### Fedora / RHEL / Rocky

```bash
sudo dnf install -y \
    gcc-c++ cmake git python3 valgrind \
    HdrHistogram_c-devel

# If the dnf package isn't available, build from source as above.
```

### Arch Linux

```bash
sudo pacman -S --needed base-devel cmake git python valgrind

# hdr_histogram_c via AUR or source build (see Ubuntu fallback above)
```

### macOS

```bash
# Xcode Command Line Tools (gives clang + leaks)
xcode-select --install

# Homebrew dependencies
brew install cmake hdrhistogram python3
```

Notes for macOS:
- The bot compiles and runs on macOS, but **CPU pinning is a no-op** (Linux-only API). Demo runs with scheduler jitter visible.
- `SO_BUSY_POLL` and `SO_TIMESTAMPING` are Linux-only and skipped via `#ifdef`. No build-time impact.

### Windows

Not supported. The bot uses POSIX sockets, `pthread_setaffinity_np`, and `SO_BUSY_POLL`. Use WSL2 with the Ubuntu instructions above.

---

## Building from a fresh checkout

```bash
git clone https://github.com/repo/iicpc-hackathon-2026.git
cd iicpc-hackathon-2026/bot-engine
mkdir -p build
cd build
cmake ..
make -j
cd ..
```

After `make -j` the following binaries should be in `build/`:

| Binary | What it does |
|---|---|
| `bot` | The load generator (main deliverable) |
| `null_responder` | Multi-connection TCP server that acks everything |
| `refengine` | Reference matching engine + `diff` subcommand |
| `buggy_engine` | Reference engine + planted price-time priority bug (demo) |
| `hdr_merge` | Per-bot CSV → fleet unified + ranking |

---

## Smoke test the build

```bash
# From bot-engine/
./build/null_responder &
sleep 1
./build/bot --bots 1 --interval-us 100 --duration-sec 5
kill %1
```

Expected output:
- `[Main] TSC calibrated.`
- `[Gate] PASSED: bot self-test p99 = XXXXX ns`
- `[Main] All bots finished. Connected: 1/1`
- `[Main] INTEGRITY: PASSED`
- An aggregate latency table with Naive vs CO-corrected percentiles
- `Naive sample count: ~50000`

Exit code `0` on success.

---

## Common build failures and fixes

### `fatal error: hdr/hdr_histogram.h: No such file or directory`

`hdr_histogram_c` is not installed or not on the include path. Install it via the platform instructions above. If you built from source and installed to `/usr/local`, you may need:

```bash
export CMAKE_PREFIX_PATH=/usr/local
cd build && rm -rf * && cmake ..
```

### `error: 'pthread_setaffinity_np' was not declared`

You're on macOS or another non-Linux system and didn't define `_GNU_SOURCE` consistently. The bot guards this with `#if defined(__linux__)` — if you see this error, it means you're trying to compile that branch directly. Check that CMakeLists.txt is being used (not a manual `g++` invocation).

### `_Static_assert is a C11 extension` warnings (6 of them)

Cosmetic. These come from `contracts/interface_contract_v1.h` using C11 static asserts in a C++ TU. They confirm the wire-format struct sizes match the contract. Safe to ignore.

### `cannot find -lhdr_histogram_static`

Some `hdr_histogram_c` distributions name the static lib differently. Check what's installed:

```bash
ldconfig -p | grep hdr        # Linux
find /usr/local -name '*hdr*' # macOS
```

Then update `bot-engine/CMakeLists.txt` `target_link_libraries(...)` accordingly. Common alternatives: `hdr_histogram`, `hdr_histogram_static`.

### `python3: command not found`

The demo and integration test scripts need Python 3.8+. Install per the platform instructions above. The main `bot` and `refengine` binaries do not require Python.

---

## Reproducing the headline numbers

```bash
# Soak suite (~9 min — produces SUMMARY.md with the 3M / 7.2M numbers)
./soak/soak_test.sh

# Coordinated Omission proof (30-second run, requires stall mode)
./build/null_responder --stall-mode --stall-every 20000 --stall-ms 5 &
./build/bot --bots 1 --interval-us 100 --duration-sec 30 --no-gate
kill %1
```

The CO proof should show:
- Naive p99 in the tens-of-µs range (~28 000 to ~60 000 ns)
- CO-corrected p99 in the millisecond range (~4 to ~5 ms)
- A ratio of roughly 70–150× — the gap that proves CO correction is working

---

## OS-level tuning (production-style)

The bot is calibrated for an isolated, jitter-controlled Linux environment. None of these are required to run the demo, but the bot reaches its best numbers when these are applied on the production target:

```bash
# Disable CPU frequency scaling
sudo cpupower frequency-set -g performance

# Disable IRQ balancing on the bot's cores (so IRQs don't preempt them)
sudo systemctl stop irqbalance

# Boot-time: isolate cores 1–4 from the scheduler
# In /etc/default/grub: GRUB_CMDLINE_LINUX="... isolcpus=1-4 nohz_full=1-4 rcu_nocbs=1-4"
sudo update-grub && sudo reboot

# Then pin bots to the isolated cores
./build/bot --start-core 1 ...
```

Without these tunings on Linux you'll see scheduler jitter at the tail; with them the p99 should drop into the low-µs range on real hardware.

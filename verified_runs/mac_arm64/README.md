Runs captured 2026-07-29 on an Apple M4 MacBook Air (macOS 26.5.2, Darwin
25.5.0 arm64) after fixing bot-engine/CMakeLists.txt to build on non-x86_64
targets (see git history). This machine was concurrently running an active
Claude Code session throughout — it is NOT a clean, isolated host, unlike the
Arch Linux and i7-13620H runs elsewhere in verified_runs/. Included
deliberately as a noisy, real-world fourth data point (and as honest evidence
of run-to-run variance under identical hardware/fault — see
docs/COORDINATED_OMISSION_WRITEUP.md), not as a rigor-matched addition to the
shared-host/isolcpus comparison.

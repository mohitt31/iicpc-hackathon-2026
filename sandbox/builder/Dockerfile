# Hermetic Builder — Sandbox Build Stage (§7 stage 2)
#
# This container compiles contestant source code into a single
# statically-linked ELF binary. It runs with NO NETWORK ACCESS
# (docker run --network none) and a pinned toolchain.
#
# Usage:
#   docker build -t sandbox-builder sandbox/builder/
#   docker run --network none \
#     -v /path/to/src:/src:ro \
#     -v /path/to/output:/output \
#     sandbox-builder
#
# The builder:
#   1. Copies source from /src to a build directory
#   2. Compiles with a pinned GCC version and strict flags
#   3. Static-links the output binary
#   4. Records compiler version + flags into /output/run_metadata.json
#   5. Outputs the ELF to /output/contestant.elf

FROM gcc:14.2 AS builder

# Install minimal build tools only — no network access at runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    make \
    cmake \
    file \
    jq \
    && rm -rf /var/lib/apt/lists/*

# Set up build environment
WORKDIR /build

# The build script that runs inside the container
COPY build_contestant.sh /build/build_contestant.sh
RUN chmod +x /build/build_contestant.sh

# Volumes:
#   /src    — contestant source (read-only mount)
#   /output — where the ELF + metadata go
VOLUME ["/src", "/output"]

ENTRYPOINT ["/build/build_contestant.sh"]

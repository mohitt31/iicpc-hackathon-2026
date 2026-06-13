# intake-api — the platform's HTTP front door.
#
# Build context is the REPO ROOT (it needs both platform/intake-api/ and the
# sandbox/ scripts the worker shells out to):
#   docker build -f platform/intake-api/Dockerfile -t iicpc-intake-api .
#
# ---- build stage ----------------------------------------------------------
FROM golang:1.23-bookworm AS build
WORKDIR /src
COPY platform/intake-api/go.mod platform/intake-api/go.sum ./
RUN go mod download
COPY platform/intake-api/ ./
RUN CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags="-s -w" -o /out/intake-api .

# ---- runtime stage --------------------------------------------------------
# Needs bash + a C++ toolchain + coreutils: the worker runs sandbox/pipeline.sh,
# which (in --skip-docker mode) compiles the contestant source with g++ and
# hashes the ELF with sha256sum.
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        bash g++ coreutils tar findutils ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /out/intake-api /usr/local/bin/intake-api
COPY sandbox/ /app/sandbox/
RUN find /app/sandbox -name '*.sh' -exec chmod +x {} + && mkdir -p /data

ENV PORT=9090 \
    SANDBOX_DIR=/app/sandbox \
    WORK_DIR=/data \
    PIPELINE_SKIP_DOCKER=true \
    PIPELINE_SKIP_PACK=true
EXPOSE 9090

HEALTHCHECK --interval=5s --timeout=3s --retries=10 \
    CMD bash -c 'exec 3<>/dev/tcp/127.0.0.1/9090 && printf "GET /health HTTP/1.0\r\n\r\n" >&3 && grep -q ok <&3'

ENTRYPOINT ["/usr/local/bin/intake-api"]

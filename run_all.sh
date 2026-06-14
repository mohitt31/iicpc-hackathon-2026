#!/bin/bash
set -e

echo "======================================"
echo " BUILDING IICPC BOT ENGINE COMPONENTS "
echo "======================================"
cd bot-engine
mkdir -p build && cd build
cmake ..
make -j ws_bot rest_bot fix_bot bot refserver
cd ../..

echo "======================================"
echo " SETTING UP TELEMETRY DATA DIRECTORY  "
echo "======================================"
SNAPSHOT_DIR="/tmp/iicpc_telemetry"
mkdir -p "$SNAPSHOT_DIR"
rm -f "$SNAPSHOT_DIR"/*.csv

# Kill any existing processes from previous runs
pkill -f refserver || true
pkill -f ws_adapter.js || true
pkill -f rest_adapter.js || true
pkill -f fix_adapter.js || true
pkill -f ws_bot || true
pkill -f rest_bot || true
pkill -f fix_bot || true
pkill -f bot || true
pkill -f telemetry_server.js || true
sleep 1

echo "======================================"
echo " STARTING REFERENCE ENGINE            "
echo "======================================"
./bot-engine/build/refserver --port 9000 --journal /tmp/ref.jrn &
RS_PID=$!
sleep 1

echo "======================================"
echo " STARTING PROTOCOL ADAPTERS           "
echo "======================================"
node platform/wsrest-adapters/ws_adapter.js --port 9001 --engine-host 127.0.0.1 --engine-port 9000 &
WS_AD_PID=$!
node platform/wsrest-adapters/rest_adapter.js --port 9002 --engine-host 127.0.0.1 --engine-port 9000 &
REST_AD_PID=$!
node platform/wsrest-adapters/fix_adapter.js --port 9003 --engine-host 127.0.0.1 --engine-port 9000 &
FIX_AD_PID=$!
sleep 1

echo "======================================"
echo " STARTING BOTS (ALL 4 PROTOCOLS)      "
echo "======================================"
# Binary TCP (direct to engine)
./bot-engine/build/bot --ip 127.0.0.1 --port 9000 --interval-us 100 --snapshot-dir "$SNAPSHOT_DIR" &
BIN_BOT_PID=$!

# WebSocket (via adapter)
./bot-engine/build/ws_bot --ip 127.0.0.1 --port 9001 --path /orders --interval-us 200 --snapshot-dir "$SNAPSHOT_DIR" &
WS_BOT_PID=$!

# REST (via adapter)
./bot-engine/build/rest_bot --ip 127.0.0.1 --port 9002 --interval-us 400 --snapshot-dir "$SNAPSHOT_DIR" &
REST_BOT_PID=$!

# FIX (via adapter)
./bot-engine/build/fix_bot --ip 127.0.0.1 --port 9003 --interval-us 400 --snapshot-dir "$SNAPSHOT_DIR" &
FIX_BOT_PID=$!

echo "======================================"
echo " STARTING TELEMETRY SERVER            "
echo "======================================"
node tools/telemetry_server.js --snapshot-dir "$SNAPSHOT_DIR" --port 8080 &
TEL_PID=$!

echo ""
echo "🚀 ALL SYSTEMS GO! FULLY INTEGRATED PIPELINE RUNNING 🚀"
echo ""
echo "Dashboard Link: file://$(pwd)/frontend/index.html"
echo "Showcase Link:  file://$(pwd)/frontend/landing.html"
echo ""
echo "Press Ctrl+C to stop everything."

cleanup() {
    echo ""
    echo "Stopping all processes..."
    kill $BIN_BOT_PID $WS_BOT_PID $REST_BOT_PID $FIX_BOT_PID $WS_AD_PID $REST_AD_PID $FIX_AD_PID $RS_PID $TEL_PID 2>/dev/null || true
    exit 0
}

trap cleanup SIGINT SIGTERM

wait

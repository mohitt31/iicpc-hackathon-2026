# Frontend — IICPC HFT Benchmarking Platform

Two static pages, zero build step:

| File | What it is |
|---|---|
| `index.html` | 3D landing page — "Simulating Peak Volatility". Procedural Three.js network-cluster + particle grid, scroll-linked zoom into the leaderboard section. |
| `leaderboard.html` | Full real-time leaderboard (deep telemetry: percentile ladders, PTP sync, integrity gate). |

Both pages auto-connect to the telemetry gateway at
`ws://localhost:8080/leaderboard/deltas` (see `tools/telemetry_server.js`)
and fall back to a contract-faithful synthetic feed when no gateway is
running — so the demo always works.

The 3D scene is 100% procedural (no image/model assets, one CDN import:
`three@0.160.0`). `prefers-reduced-motion` is respected.

## Run locally

```bash
cd frontend
python3 -m http.server 4173
# → http://localhost:4173

# optional live feed:
cd ../tools && npm install ws && node telemetry_server.js --port 8080
```

## Deploy to Vercel (one minute)

```bash
npm i -g vercel       # once
cd frontend
vercel                # first deploy: accept defaults (no build command, output = ./)
vercel --prod         # promote to the production domain
```

Or without the CLI: drag the `frontend/` folder onto https://vercel.com/new.

Note: a Vercel deployment serves the static pages; the WebSocket live feed
needs the gateway reachable from the browser (run it locally or point
`LIVE_WS_URL` at a hosted gateway). The synthetic feed keeps the deployed
site fully alive either way.

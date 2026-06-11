# Frontend — IICPC HFT Benchmarking Platform

Two static pages, zero build step:

| File | What it is |
|---|---|
| `index.html` | **Real-time leaderboard** (the main page) — data-dense board with synthetic demo data visible instantly: percentile ladders, protocol tracks, PTP sync, integrity gate. |
| `landing.html` | Optional 3D showcase — "Simulating Peak Volatility" hero with a bloom-lit WebGL engine core, order-flow particle streams, and a GSAP scroll-jacked telemetry HUD. Linked from the leaderboard's "3D INTRO" pill. |

Both pages auto-connect to the telemetry gateway at
`ws://localhost:8080/leaderboard/deltas` (see `tools/telemetry_server.js`)
and fall back to a contract-faithful synthetic feed when no gateway is
running — so the demo always works, instantly, with zero backend.

The 3D scene is 100% procedural (no image/model assets; CDN imports:
`three@0.160.0` + `gsap@3.12.5`) and fully optional — if WebGL or the CDN
is unavailable, `landing.html` degrades to a clean static page (3-second
watchdog guarantees content is never hidden). `prefers-reduced-motion`
is respected on both pages.

## Run locally

```bash
cd frontend
python3 -m http.server 4173
# → http://localhost:4173

# optional live feed:
cd ../tools && npm install ws && node telemetry_server.js --port 8080
```

## Deploy

Auto-deployed to GitHub Pages on every push to main that touches
`frontend/` (see `.github/workflows/pages.yml`):

**https://mohitt31.github.io/iicpc-hackathon-2026/**

Or to Vercel: `npm i -g vercel && cd frontend && vercel --prod`.

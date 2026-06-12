const WebSocket = require('ws');
const ws = new WebSocket('ws://localhost:8080/leaderboard/deltas');
ws.on('open', () => console.log('Connected!'));
ws.on('message', (data) => { console.log(data.toString()); process.exit(0); });
ws.on('error', (err) => { console.error(err); process.exit(1); });

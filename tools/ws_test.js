const WebSocket = require('ws');
const ws = new WebSocket('ws://localhost:8080/leaderboard/deltas');
ws.on('message', (data) => {
  console.log(data.toString());
  process.exit(0);
});

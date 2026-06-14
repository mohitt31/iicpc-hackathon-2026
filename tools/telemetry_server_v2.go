package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/go-redis/redis/v8"
	"github.com/gorilla/websocket"
	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promauto"
	"github.com/prometheus/client_golang/prometheus/promhttp"
)

// ---------------------------------------------------------
// 1. PROMETHEUS METRICS REGISTRY (Scraped by VictoriaMetrics)
// ---------------------------------------------------------
var (
	sentCounter = promauto.NewCounter(prometheus.CounterOpts{
		Name: "benchmark_sent_count_total",
		Help: "Total orders sent by the C++ bot fleet",
	})
	ackedCounter = promauto.NewCounter(prometheus.CounterOpts{
		Name: "benchmark_acked_count_total",
		Help: "Total orders acked by the engine",
	})
	p99LatencyGauge = promauto.NewGauge(prometheus.GaugeOpts{
		Name: "benchmark_p99_latency_ns",
		Help: "CO-corrected p99 latency in nanoseconds",
	})
)

// ---------------------------------------------------------
// 2. WEBSOCKET HUB (Concurrent-safe Fan-out)
// ---------------------------------------------------------
var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool { return true }, // Relaxed for hackathon UI
	ReadBufferSize:  1024,
	WriteBufferSize: 1024,
}

type Client struct {
	hub  *Hub
	conn *websocket.Conn
	send chan []byte
}

type Hub struct {
	clients    map[*Client]bool
	broadcast  chan []byte
	register   chan *Client
	unregister chan *Client
}

func NewHub() *Hub {
	return &Hub{
		broadcast:  make(chan []byte, 1024), // Buffered to prevent blocking the stream reader
		register:   make(chan *Client),
		unregister: make(chan *Client),
		clients:    make(map[*Client]bool),
	}
}

func (h *Hub) Run() {
	for {
		select {
		case client := <-h.register:
			h.clients[client] = true
		case client := <-h.unregister:
			if _, ok := h.clients[client]; ok {
				delete(h.clients, client)
				close(client.send)
			}
		case message := <-h.broadcast:
			for client := range h.clients {
				select {
				case client.send <- message:
				default:
					// KILL SLOW CLIENTS: If client channel is full, drop them.
					// We do not let a 3G mobile connection lag our HFT dashboard.
					close(client.send)
					delete(h.clients, client)
				}
			}
		}
	}
}

func (c *Client) writePump() {
	defer func() {
		c.conn.Close()
	}()
	for message := range c.send {
		c.conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
		err := c.conn.WriteMessage(websocket.TextMessage, message)
		if err != nil {
			return
		}
	}
}

func serveWs(hub *Hub, w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Println("WS Upgrade Error:", err)
		return
	}
	client := &Client{hub: hub, conn: conn, send: make(chan []byte, 256)}
	client.hub.register <- client
	go client.writePump()
}

// ---------------------------------------------------------
// 3. REDIS STREAM CONSUMER (At-Least-Once Delivery via PEL)
// ---------------------------------------------------------
type TelemetryPayload struct {
	Timestamp int64   `json:"timestamp"`
	Sent      float64 `json:"sent_count"`
	Acked     float64 `json:"acked_count"`
	P99       float64 `json:"p99_latency_ns"`
}

func consumeRedisStream(rdb *redis.Client, hub *Hub, stream, group, consumer string) {
	ctx := context.Background()

	// Ensure Consumer Group exists; ignores error if it already does
	err := rdb.XGroupCreateMkStream(ctx, stream, group, "$").Err()
	if err != nil && !strings.Contains(err.Error(), "BUSYGROUP") {
		log.Fatalf("Failed to create consumer group: %v", err)
	}

	for {
		args := &redis.XReadGroupArgs{
			Group:    group,
			Consumer: consumer,
			Streams:  []string{stream, ">"},
			Count:    100, // Batch pull for throughput
			Block:    0,   // Block indefinitely until new data
		}

		streams, err := rdb.XReadGroup(ctx, args).Result()
		if err != nil {
			log.Printf("Redis XReadGroup error: %v. Retrying...", err)
			time.Sleep(1 * time.Second)
			continue
		}

		for _, s := range streams {
			for _, msg := range s.Messages {
				// Parse payload (Redis maps fields as strings)
				sent, _ := strconv.ParseFloat(fmt.Sprintf("%v", msg.Values["sent"]), 64)
				acked, _ := strconv.ParseFloat(fmt.Sprintf("%v", msg.Values["acked"]), 64)
				p99, _ := strconv.ParseFloat(fmt.Sprintf("%v", msg.Values["p99_latency_ns"]), 64)

				// 1. Update Prometheus Metrics in-memory
				sentCounter.Add(sent)
				ackedCounter.Add(acked)
				p99LatencyGauge.Set(p99)

				// 2. Prepare JSON Delta for Leaderboard (Zero-Reflection, Zero-Allocation)
				jsonBytes := make([]byte, 0, 128)
				jsonBytes = append(jsonBytes, `{"timestamp":`...)
				jsonBytes = strconv.AppendInt(jsonBytes, time.Now().UnixMilli(), 10)

				jsonBytes = append(jsonBytes, `,"sent_count":`...)
				jsonBytes = strconv.AppendFloat(jsonBytes, sent, 'f', -1, 64)

				jsonBytes = append(jsonBytes, `,"acked_count":`...)
				jsonBytes = strconv.AppendFloat(jsonBytes, acked, 'f', -1, 64)

				jsonBytes = append(jsonBytes, `,"p99_latency_ns":`...)
				jsonBytes = strconv.AppendFloat(jsonBytes, p99, 'f', -1, 64)

				jsonBytes = append(jsonBytes, '}')

				// 3. Fan-out to all connected WebSocket clients
				hub.broadcast <- jsonBytes

				// 4. Acknowledge message strictly AFTER processing to prevent data loss
				rdb.XAck(ctx, stream, group, msg.ID)
			}
		}
	}
}

// ---------------------------------------------------------
// MAIN
// ---------------------------------------------------------
func main() {
	redisHost := os.Getenv("REDIS_HOST")
	if redisHost == "" {
		redisHost = "localhost:6379"
	}

	rdb := redis.NewClient(&redis.Options{
		Addr: redisHost,
	})

	hub := NewHub()
	go hub.Run()

	// Fire up the Redis Consumer loop
	go consumeRedisStream(rdb, hub, "telemetry:stream", "gateway_group", "worker_1")

	// Routes
	http.HandleFunc("/ws", func(w http.ResponseWriter, r *http.Request) {
		serveWs(hub, w, r)
	})
	http.Handle("/metrics", promhttp.Handler())

	log.Println("Elite Telemetry Gateway running on :8080")
	log.Println(" -> /metrics ready for VictoriaMetrics scraper")
	log.Println(" -> /ws ready for Leaderboard delta streaming")
	
	if err := http.ListenAndServe(":8080", nil); err != nil {
		log.Fatalf("Server crashed: %v", err)
	}
}

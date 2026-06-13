// Command intake-api is the platform's front door: the HTTP service that accepts
// contestant engine submissions and drives them through the sandbox pipeline.
//
// It is the missing control plane between a contestant's `curl` and the sandbox
// scripts under sandbox/ (intake → build → attest → pack → run → score).
//
//	POST /api/v1/submissions   (multipart tarball, field "engine")
//	    → size-checked, sha256'd into an immutable submission_id
//	    → state RECEIVED, LPUSH'd onto the Redis queue  submissions:pending
//	    → 202 { "submission_id": "...", "state": "RECEIVED" }
//
//	GET  /api/v1/submissions/:id
//	    → the live state machine:
//	        RECEIVED → BUILDING → ATTESTED → RUNNING → SCORED
//	                                      ↘  REJECTED  (any stage can reject)
//
// Redis is deliberately the real control-plane queue (not just a name-drop): a
// worker goroutine BRPOPs submissions:pending, runs sandbox/pipeline.sh
// (intake+build+attest), advances the state, and hands ATTESTED submissions off
// to submissions:scoring for the microVM/orchestrator stage.
package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"mime/multipart"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/redis/go-redis/v9"
)

// ---- state machine ---------------------------------------------------------

type State string

const (
	StateReceived State = "RECEIVED"
	StateBuilding State = "BUILDING"
	StateAttested State = "ATTESTED"
	StateRunning  State = "RUNNING"
	StateScored   State = "SCORED"
	StateRejected State = "REJECTED"
)

// allowedTransitions guards the externally-driven advances (the orchestrator
// posting RUNNING/SCORED). The worker advances RECEIVED→BUILDING→ATTESTED itself.
var allowedTransitions = map[State][]State{
	StateReceived: {StateBuilding, StateRejected},
	StateBuilding: {StateAttested, StateRejected},
	StateAttested: {StateRunning, StateRejected},
	StateRunning:  {StateScored, StateRejected},
}

func canTransition(from, to State) bool {
	for _, s := range allowedTransitions[from] {
		if s == to {
			return true
		}
	}
	return false
}

// ---- config ----------------------------------------------------------------

type config struct {
	port       string
	redisAddr  string
	sandboxDir string
	workDir    string
	sizeCapMB  int64
	skipDocker bool
	skipPack   bool
}

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func envBool(key string, def bool) bool {
	v := os.Getenv(key)
	if v == "" {
		return def
	}
	b, err := strconv.ParseBool(v)
	if err != nil {
		return def
	}
	return b
}

func loadConfig() config {
	capMB, _ := strconv.ParseInt(envOr("SIZE_CAP_MB", "50"), 10, 64)
	if capMB <= 0 {
		capMB = 50
	}
	return config{
		port:       envOr("PORT", "9090"),
		redisAddr:  envOr("REDIS_ADDR", "localhost:6379"),
		sandboxDir: envOr("SANDBOX_DIR", "/app/sandbox"),
		workDir:    envOr("WORK_DIR", "/data"),
		sizeCapMB:  capMB,
		// Default to the no-Docker, no-pack path: hermetic Docker-in-Docker and
		// the ext4 pack need privileges the API container does not have. The real
		// hermetic build + microVM run happen on the orchestrator nodes.
		skipDocker: envBool("PIPELINE_SKIP_DOCKER", true),
		skipPack:   envBool("PIPELINE_SKIP_PACK", true),
	}
}

// ---- server ----------------------------------------------------------------

const (
	queuePending = "submissions:pending"
	queueScoring = "submissions:scoring"
	indexKey     = "submissions:index"
)

type server struct {
	cfg config
	rdb *redis.Client
}

func subKey(id string) string { return "submission:" + id }

// short returns a log-friendly id prefix without panicking on short ids
// (real ids are 64-char sha256, but the /state endpoint accepts arbitrary ids).
func short(id string) string {
	if len(id) > 12 {
		return id[:12]
	}
	return id
}

// setState writes the state (and optional extra fields) and bumps updated_at.
func (s *server) setState(ctx context.Context, id string, st State, extra map[string]any) error {
	fields := map[string]any{"state": string(st), "updated_at": time.Now().UTC().Format(time.RFC3339)}
	for k, v := range extra {
		fields[k] = v
	}
	return s.rdb.HSet(ctx, subKey(id), fields).Err()
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

// POST /api/v1/submissions — accept a tarball, validate size, queue it.
func (s *server) handleCreate(w http.ResponseWriter, r *http.Request) {
	ctx := r.Context()
	capBytes := s.cfg.sizeCapMB * 1024 * 1024

	// Cap the request body so an oversized upload can't exhaust memory/disk.
	// +1MB slack for multipart framing/headers around the file part.
	r.Body = http.MaxBytesReader(w, r.Body, capBytes+(1<<20))
	if err := r.ParseMultipartForm(8 << 20); err != nil {
		var maxErr *http.MaxBytesError
		if errors.As(err, &maxErr) {
			writeJSON(w, http.StatusRequestEntityTooLarge,
				map[string]string{"error": fmt.Sprintf("upload exceeds %d MB cap", s.cfg.sizeCapMB)})
			return
		}
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "could not parse multipart form: " + err.Error()})
		return
	}

	file, hdr, err := s.pickFile(r)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
		return
	}
	defer file.Close()

	// Stream to a temp file while hashing and counting bytes.
	if err := os.MkdirAll(s.cfg.workDir, 0o755); err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "workdir: " + err.Error()})
		return
	}
	tmp, err := os.CreateTemp(s.cfg.workDir, "upload-*.tar.gz")
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "tempfile: " + err.Error()})
		return
	}
	tmpName := tmp.Name()

	h := sha256.New()
	n, copyErr := io.Copy(io.MultiWriter(tmp, h), file)
	tmp.Close()
	if copyErr != nil {
		os.Remove(tmpName)
		// MaxBytesReader trips here when the cap is exceeded.
		writeJSON(w, http.StatusRequestEntityTooLarge,
			map[string]string{"error": fmt.Sprintf("upload exceeds %d MB cap", s.cfg.sizeCapMB)})
		return
	}
	if n == 0 {
		os.Remove(tmpName)
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "empty upload"})
		return
	}

	id := hex.EncodeToString(h.Sum(nil))
	finalPath := filepath.Join(s.cfg.workDir, id+".tar.gz")
	if err := os.Rename(tmpName, finalPath); err != nil {
		os.Remove(tmpName)
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "persist: " + err.Error()})
		return
	}

	now := time.Now().UTC().Format(time.RFC3339)
	if err := s.rdb.HSet(ctx, subKey(id), map[string]any{
		"submission_id": id,
		"state":         string(StateReceived),
		"original_file": hdr.Filename,
		"file_size":     n,
		"tarball_path":  finalPath,
		"created_at":    now,
		"updated_at":    now,
	}).Err(); err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "redis: " + err.Error()})
		return
	}
	// Index for listing (newest first), and enqueue for the worker.
	s.rdb.ZAdd(ctx, indexKey, redis.Z{Score: float64(time.Now().UnixNano()), Member: id})
	if err := s.rdb.LPush(ctx, queuePending, id).Err(); err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "enqueue: " + err.Error()})
		return
	}

	log.Printf("RECEIVED %s (%s, %d bytes) → queued", short(id), hdr.Filename, n)
	writeJSON(w, http.StatusAccepted, map[string]any{
		"submission_id": id,
		"state":         StateReceived,
		"status_url":    "/api/v1/submissions/" + id,
	})
}

// pickFile returns the "engine" file part, or the first file part if absent.
func (s *server) pickFile(r *http.Request) (multipart.File, *multipart.FileHeader, error) {
	if r.MultipartForm == nil || r.MultipartForm.File == nil {
		return nil, nil, errors.New("no file in multipart form (expected -F engine=@<tarball>)")
	}
	if fhs := r.MultipartForm.File["engine"]; len(fhs) > 0 {
		f, err := fhs[0].Open()
		return f, fhs[0], err
	}
	for _, fhs := range r.MultipartForm.File {
		if len(fhs) > 0 {
			f, err := fhs[0].Open()
			return f, fhs[0], err
		}
	}
	return nil, nil, errors.New("no file in multipart form (expected -F engine=@<tarball>)")
}

// GET /api/v1/submissions/:id — current state machine snapshot.
func (s *server) handleGet(w http.ResponseWriter, id string) {
	ctx := context.Background()
	m, err := s.rdb.HGetAll(ctx, subKey(id)).Result()
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": err.Error()})
		return
	}
	if len(m) == 0 {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "no such submission: " + id})
		return
	}
	// tarball_path is an internal detail — don't leak the host path.
	delete(m, "tarball_path")
	writeJSON(w, http.StatusOK, m)
}

// POST /api/v1/submissions/:id/state — orchestrator advances RUNNING/SCORED.
func (s *server) handleAdvance(w http.ResponseWriter, r *http.Request, id string) {
	ctx := r.Context()
	var body struct {
		State State   `json:"state"`
		Score float64 `json:"score"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 1<<16)).Decode(&body); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad json: " + err.Error()})
		return
	}
	cur, err := s.rdb.HGet(ctx, subKey(id), "state").Result()
	if errors.Is(err, redis.Nil) {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "no such submission"})
		return
	} else if err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": err.Error()})
		return
	}
	if !canTransition(State(cur), body.State) {
		writeJSON(w, http.StatusConflict,
			map[string]string{"error": fmt.Sprintf("illegal transition %s → %s", cur, body.State)})
		return
	}
	extra := map[string]any{}
	if body.State == StateScored {
		extra["score"] = body.Score
	}
	if err := s.setState(ctx, id, body.State, extra); err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": err.Error()})
		return
	}
	log.Printf("%s %s → %s", short(id), cur, body.State)
	writeJSON(w, http.StatusOK, map[string]any{"submission_id": id, "state": body.State})
}

// router keeps things dependency-free: no mux library, just path matching.
func (s *server) routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) {
		if err := s.rdb.Ping(r.Context()).Err(); err != nil {
			http.Error(w, "redis down: "+err.Error(), http.StatusServiceUnavailable)
			return
		}
		w.Write([]byte("ok"))
	})
	mux.HandleFunc("/api/v1/submissions", func(w http.ResponseWriter, r *http.Request) {
		switch r.Method {
		case http.MethodPost:
			s.handleCreate(w, r)
		case http.MethodGet:
			s.handleList(w, r)
		default:
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		}
	})
	mux.HandleFunc("/api/v1/submissions/", func(w http.ResponseWriter, r *http.Request) {
		rest := strings.TrimPrefix(r.URL.Path, "/api/v1/submissions/")
		id, sub, _ := strings.Cut(rest, "/")
		if id == "" {
			http.Error(w, "missing submission id", http.StatusBadRequest)
			return
		}
		switch {
		case sub == "" && r.Method == http.MethodGet:
			s.handleGet(w, id)
		case sub == "state" && r.Method == http.MethodPost:
			s.handleAdvance(w, r, id)
		default:
			http.Error(w, "not found", http.StatusNotFound)
		}
	})
	return mux
}

// GET /api/v1/submissions — newest-first list (id + state).
func (s *server) handleList(w http.ResponseWriter, r *http.Request) {
	ctx := r.Context()
	ids, err := s.rdb.ZRevRange(ctx, indexKey, 0, 49).Result()
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": err.Error()})
		return
	}
	out := make([]map[string]string, 0, len(ids))
	for _, id := range ids {
		st, _ := s.rdb.HGet(ctx, subKey(id), "state").Result()
		out = append(out, map[string]string{"submission_id": id, "state": st})
	}
	writeJSON(w, http.StatusOK, map[string]any{"submissions": out})
}

// ---- worker ----------------------------------------------------------------

// worker drains submissions:pending and runs the sandbox pipeline for each.
func (s *server) worker(ctx context.Context) {
	pipeline := filepath.Join(s.cfg.sandboxDir, "pipeline.sh")
	for {
		select {
		case <-ctx.Done():
			return
		default:
		}
		// Block up to 5s for the next id; loop lets us honour ctx cancellation.
		res, err := s.rdb.BRPop(ctx, 5*time.Second, queuePending).Result()
		if err != nil {
			if errors.Is(err, redis.Nil) || ctx.Err() != nil {
				continue
			}
			log.Printf("worker BRPOP error: %v", err)
			time.Sleep(time.Second)
			continue
		}
		id := res[1]
		s.process(ctx, pipeline, id)
	}
}

func (s *server) process(ctx context.Context, pipeline, id string) {
	tarball, err := s.rdb.HGet(ctx, subKey(id), "tarball_path").Result()
	if err != nil || tarball == "" {
		log.Printf("process %s: no tarball_path: %v", short(id), err)
		_ = s.setState(ctx, id, StateRejected, map[string]any{"error": "lost tarball reference"})
		return
	}

	_ = s.setState(ctx, id, StateBuilding, nil)
	log.Printf("BUILDING %s", short(id))

	args := []string{pipeline, tarball, "--output-dir", filepath.Join(s.cfg.workDir, "submissions")}
	if s.cfg.skipDocker {
		args = append(args, "--skip-docker")
	}
	if s.cfg.skipPack {
		args = append(args, "--skip-pack")
	}

	runCtx, cancel := context.WithTimeout(ctx, 5*time.Minute)
	defer cancel()
	cmd := exec.CommandContext(runCtx, "bash", args...)
	out, runErr := cmd.CombinedOutput()
	if runErr != nil {
		msg := lastLines(string(out), 4)
		log.Printf("REJECTED %s: pipeline failed: %v\n%s", short(id), runErr, msg)
		_ = s.setState(ctx, id, StateRejected, map[string]any{"error": "build/attest failed: " + msg})
		return
	}

	_ = s.setState(ctx, id, StateAttested, nil)
	// Hand off to the run/score stage (microVM orchestrator consumes this).
	s.rdb.LPush(ctx, queueScoring, id)
	log.Printf("ATTESTED %s → queued for scoring", short(id))
}

func lastLines(s string, n int) string {
	s = strings.TrimRight(s, "\n")
	lines := strings.Split(s, "\n")
	if len(lines) > n {
		lines = lines[len(lines)-n:]
	}
	return strings.Join(lines, " | ")
}

// ---- main ------------------------------------------------------------------

func main() {
	cfg := loadConfig()
	rdb := redis.NewClient(&redis.Options{Addr: cfg.redisAddr})

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	// Wait briefly for Redis (compose ordering races).
	for i := 0; i < 30; i++ {
		if err := rdb.Ping(ctx).Err(); err == nil {
			break
		}
		if ctx.Err() != nil {
			return
		}
		log.Printf("waiting for redis at %s ...", cfg.redisAddr)
		time.Sleep(time.Second)
	}

	s := &server{cfg: cfg, rdb: rdb}
	go s.worker(ctx)

	srv := &http.Server{Addr: ":" + cfg.port, Handler: s.routes()}
	go func() {
		<-ctx.Done()
		shutCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = srv.Shutdown(shutCtx)
	}()

	log.Printf("intake-api listening on :%s · redis=%s · sandbox=%s · skipDocker=%v skipPack=%v",
		cfg.port, cfg.redisAddr, cfg.sandboxDir, cfg.skipDocker, cfg.skipPack)
	if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
		log.Fatalf("server error: %v", err)
	}
}

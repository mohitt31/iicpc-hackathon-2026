package main

import (
	"bytes"
	"context"
	"encoding/json"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"github.com/alicebob/miniredis/v2"
	"github.com/redis/go-redis/v9"
)

// newTestServer wires the server against an in-process Redis (miniredis) and a
// temp work dir, with SANDBOX_DIR pointing at a dir we can drop a fake
// pipeline.sh into. No external services, no Docker — pure unit test.
func newTestServer(t *testing.T) (*server, *miniredis.Miniredis) {
	t.Helper()
	mr, err := miniredis.Run()
	if err != nil {
		t.Fatalf("miniredis: %v", err)
	}
	t.Cleanup(mr.Close)
	rdb := redis.NewClient(&redis.Options{Addr: mr.Addr()})
	s := &server{
		cfg: config{
			port:       "0",
			sandboxDir: t.TempDir(),
			workDir:    t.TempDir(),
			sizeCapMB:  50,
			skipDocker: true,
			skipPack:   true,
		},
		rdb: rdb,
	}
	return s, mr
}

// writeFakePipeline drops a pipeline.sh into SANDBOX_DIR that exits with the
// given code, emitting `out` so we can assert error capture.
func writeFakePipeline(t *testing.T, s *server, exitCode int, out string) {
	t.Helper()
	body := "#!/bin/bash\n" + "echo '" + out + "'\n" + "exit " + strconv.Itoa(exitCode) + "\n"
	p := filepath.Join(s.cfg.sandboxDir, "pipeline.sh")
	if err := os.WriteFile(p, []byte(body), 0o755); err != nil {
		t.Fatalf("write fake pipeline: %v", err)
	}
}

// multipartUpload builds a multipart POST body with one file part.
func multipartUpload(t *testing.T, field, filename string, content []byte) (*bytes.Buffer, string) {
	t.Helper()
	var buf bytes.Buffer
	w := multipart.NewWriter(&buf)
	fw, err := w.CreateFormFile(field, filename)
	if err != nil {
		t.Fatal(err)
	}
	fw.Write(content)
	w.Close()
	return &buf, w.FormDataContentType()
}

func TestCanTransition(t *testing.T) {
	cases := []struct {
		from, to State
		ok       bool
	}{
		{StateReceived, StateBuilding, true},
		{StateBuilding, StateAttested, true},
		{StateAttested, StateRunning, true},
		{StateRunning, StateScored, true},
		{StateAttested, StateRejected, true},
		{StateReceived, StateScored, false}, // can't skip
		{StateScored, StateBuilding, false}, // terminal
		{StateBuilding, StateScored, false}, // skip
	}
	for _, c := range cases {
		if got := canTransition(c.from, c.to); got != c.ok {
			t.Errorf("canTransition(%s,%s)=%v want %v", c.from, c.to, got, c.ok)
		}
	}
}

func TestCreateAndGet(t *testing.T) {
	s, _ := newTestServer(t)
	body, ctype := multipartUpload(t, "engine", "e.tar.gz", []byte("dummy tarball bytes"))
	req := httptest.NewRequest(http.MethodPost, "/api/v1/submissions", body)
	req.Header.Set("Content-Type", ctype)
	rec := httptest.NewRecorder()
	s.handleCreate(rec, req)

	if rec.Code != http.StatusAccepted {
		t.Fatalf("POST status=%d body=%s", rec.Code, rec.Body.String())
	}
	var resp struct {
		SubmissionID string `json:"submission_id"`
		State        string `json:"state"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &resp); err != nil {
		t.Fatal(err)
	}
	if resp.State != "RECEIVED" || len(resp.SubmissionID) != 64 {
		t.Fatalf("unexpected resp: %+v", resp)
	}

	// queued?
	if n, _ := s.rdb.LLen(context.Background(), queuePending).Result(); n != 1 {
		t.Fatalf("expected 1 queued, got %d", n)
	}
	// tarball persisted?
	if _, err := os.Stat(filepath.Join(s.cfg.workDir, resp.SubmissionID+".tar.gz")); err != nil {
		t.Fatalf("tarball not persisted: %v", err)
	}

	// GET returns the state, and hides tarball_path
	greq := httptest.NewRecorder()
	s.handleGet(greq, resp.SubmissionID)
	if greq.Code != http.StatusOK {
		t.Fatalf("GET status=%d", greq.Code)
	}
	if strings.Contains(greq.Body.String(), "tarball_path") {
		t.Errorf("GET leaked tarball_path: %s", greq.Body.String())
	}

	// unknown id → 404
	nf := httptest.NewRecorder()
	s.handleGet(nf, "deadbeef")
	if nf.Code != http.StatusNotFound {
		t.Errorf("unknown id status=%d want 404", nf.Code)
	}
}

func TestCreateEmpty(t *testing.T) {
	s, _ := newTestServer(t)
	body, ctype := multipartUpload(t, "engine", "e.tar.gz", []byte{})
	req := httptest.NewRequest(http.MethodPost, "/api/v1/submissions", body)
	req.Header.Set("Content-Type", ctype)
	rec := httptest.NewRecorder()
	s.handleCreate(rec, req)
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("empty upload status=%d want 400", rec.Code)
	}
}

func TestCreateOversize(t *testing.T) {
	s, _ := newTestServer(t)
	s.cfg.sizeCapMB = 1 // 1 MB cap
	big := bytes.Repeat([]byte("x"), 2*1024*1024)
	body, ctype := multipartUpload(t, "engine", "e.tar.gz", big)
	req := httptest.NewRequest(http.MethodPost, "/api/v1/submissions", body)
	req.Header.Set("Content-Type", ctype)
	rec := httptest.NewRecorder()
	s.handleCreate(rec, req)
	if rec.Code != http.StatusRequestEntityTooLarge {
		t.Fatalf("oversize status=%d want 413", rec.Code)
	}
}

func TestAdvanceLegalAndIllegal(t *testing.T) {
	s, _ := newTestServer(t)
	ctx := context.Background()
	id := "abc123"
	s.setState(ctx, id, StateAttested, nil)

	// legal: ATTESTED → RUNNING
	legal := httptest.NewRecorder()
	s.handleAdvance(legal, httptest.NewRequest(http.MethodPost, "/", strings.NewReader(`{"state":"RUNNING"}`)), id)
	if legal.Code != http.StatusOK {
		t.Fatalf("legal advance status=%d body=%s", legal.Code, legal.Body.String())
	}

	// legal: RUNNING → SCORED with score
	scored := httptest.NewRecorder()
	s.handleAdvance(scored, httptest.NewRequest(http.MethodPost, "/", strings.NewReader(`{"state":"SCORED","score":91.2}`)), id)
	if scored.Code != http.StatusOK {
		t.Fatalf("scored status=%d", scored.Code)
	}
	if got, _ := s.rdb.HGet(ctx, subKey(id), "score").Result(); got != "91.2" {
		t.Errorf("score=%q want 91.2", got)
	}

	// illegal: SCORED → BUILDING
	illegal := httptest.NewRecorder()
	s.handleAdvance(illegal, httptest.NewRequest(http.MethodPost, "/", strings.NewReader(`{"state":"BUILDING"}`)), id)
	if illegal.Code != http.StatusConflict {
		t.Fatalf("illegal advance status=%d want 409", illegal.Code)
	}

	// advance unknown id → 404
	nf := httptest.NewRecorder()
	s.handleAdvance(nf, httptest.NewRequest(http.MethodPost, "/", strings.NewReader(`{"state":"RUNNING"}`)), "nope")
	if nf.Code != http.StatusNotFound {
		t.Errorf("advance unknown status=%d want 404", nf.Code)
	}
}

func TestWorkerHappyPath(t *testing.T) {
	s, _ := newTestServer(t)
	ctx := context.Background()
	writeFakePipeline(t, s, 0, "build ok")

	id := "happy123"
	tarball := filepath.Join(s.cfg.workDir, id+".tar.gz")
	os.WriteFile(tarball, []byte("x"), 0o644)
	s.rdb.HSet(ctx, subKey(id), map[string]any{"state": string(StateReceived), "tarball_path": tarball})

	s.process(ctx, filepath.Join(s.cfg.sandboxDir, "pipeline.sh"), id)

	if st, _ := s.rdb.HGet(ctx, subKey(id), "state").Result(); st != string(StateAttested) {
		t.Fatalf("state=%s want ATTESTED", st)
	}
	if n, _ := s.rdb.LLen(ctx, queueScoring).Result(); n != 1 {
		t.Fatalf("expected handoff to scoring queue, len=%d", n)
	}
}

func TestWorkerReject(t *testing.T) {
	s, _ := newTestServer(t)
	ctx := context.Background()
	writeFakePipeline(t, s, 1, "REJECTED: banned syscall fork")

	id := "bad123"
	tarball := filepath.Join(s.cfg.workDir, id+".tar.gz")
	os.WriteFile(tarball, []byte("x"), 0o644)
	s.rdb.HSet(ctx, subKey(id), map[string]any{"state": string(StateReceived), "tarball_path": tarball})

	s.process(ctx, filepath.Join(s.cfg.sandboxDir, "pipeline.sh"), id)

	st, _ := s.rdb.HGet(ctx, subKey(id), "state").Result()
	if st != string(StateRejected) {
		t.Fatalf("state=%s want REJECTED", st)
	}
	errMsg, _ := s.rdb.HGet(ctx, subKey(id), "error").Result()
	if !strings.Contains(errMsg, "banned syscall") {
		t.Errorf("error did not capture pipeline output: %q", errMsg)
	}
}

func TestList(t *testing.T) {
	s, _ := newTestServer(t)
	for _, content := range [][]byte{[]byte("aaa"), []byte("bbb")} {
		body, ctype := multipartUpload(t, "engine", "e.tar.gz", content)
		req := httptest.NewRequest(http.MethodPost, "/api/v1/submissions", body)
		req.Header.Set("Content-Type", ctype)
		s.handleCreate(httptest.NewRecorder(), req)
	}
	rec := httptest.NewRecorder()
	s.handleList(rec, httptest.NewRequest(http.MethodGet, "/api/v1/submissions", nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("list status=%d", rec.Code)
	}
	var out struct {
		Submissions []map[string]string `json:"submissions"`
	}
	json.Unmarshal(rec.Body.Bytes(), &out)
	if len(out.Submissions) != 2 {
		t.Fatalf("expected 2 submissions, got %d", len(out.Submissions))
	}
}

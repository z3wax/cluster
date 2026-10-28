package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

func testAPI(t *testing.T) (*api, *http.ServeMux) {
	t.Helper()
	root := t.TempDir()
	db, err := OpenDB(filepath.Join(root, "db.json"))
	if err != nil {
		t.Fatal(err)
	}
	a := &api{db: db, data: root}
	mux := http.NewServeMux()
	a.routes(mux)
	return a, mux
}

func TestOpenDBRejectsReadFailureAndRestoresNullMaps(t *testing.T) {
	root := t.TempDir()
	if _, err := OpenDB(root); err == nil {
		t.Fatal("directory read failure was silently replaced with an empty database")
	}
	path := filepath.Join(root, "db.json")
	if err := os.WriteFile(path, []byte(`{"nodes":null,"files":null,"tasks":null,"payouts":null}`), 0600); err != nil {
		t.Fatal(err)
	}
	db, err := OpenDB(path)
	if err != nil {
		t.Fatal(err)
	}
	if db.Nodes == nil || db.Files == nil || db.Tasks == nil || db.Payouts == nil {
		t.Fatal("null collections would panic on first assignment")
	}
}

func TestConcurrentSaveDoesNotCorruptDatabase(t *testing.T) {
	db, err := OpenDB(filepath.Join(t.TempDir(), "db.json"))
	if err != nil {
		t.Fatal(err)
	}
	// Large enough snapshots overlap at the actual filesystem writes too.
	for index := range 32 {
		id := fmt.Sprintf("existing-%d", index)
		db.Nodes[id] = &Node{ID: id, Name: strings.Repeat("x", 8192)}
	}
	const workers = 24
	start := make(chan struct{})
	results := make(chan error, workers)
	var wg sync.WaitGroup
	for index := range workers {
		wg.Add(1)
		go func(index int) {
			defer wg.Done()
			<-start
			db.mu.Lock()
			id := fmt.Sprintf("pc-%d", index)
			db.Nodes[id] = &Node{ID: id}
			db.mu.Unlock()
			results <- db.Save()
		}(index)
	}
	close(start)
	wg.Wait()
	close(results)
	for err := range results {
		if err != nil {
			t.Errorf("concurrent save failed: %v", err)
		}
	}
	disk, err := OpenDB(db.path)
	if err != nil {
		t.Fatal(err)
	}
	if len(disk.Nodes) != workers+32 {
		t.Fatalf("latest snapshot lost nodes: %d/%d", len(disk.Nodes), workers+32)
	}
}

type failingBody struct{ sent bool }

func (reader *failingBody) Read(buffer []byte) (int, error) {
	if reader.sent {
		return 0, errors.New("connection lost")
	}
	reader.sent = true
	return copy(buffer, "ab"), errors.New("connection lost")
}
func (*failingBody) Close() error { return nil }

func TestUploadRejectsMismatchedBodiesWithoutPublishingOrLeakingChunks(t *testing.T) {
	for _, tc := range []struct {
		name, size string
		body       io.Reader
	}{
		{"negative", "-1", strings.NewReader("abc")},
		{"short", "4", strings.NewReader("abc")},
		{"long", "2", strings.NewReader("abc")},
		{"zero-with-data", "0", strings.NewReader("abc")},
		{"read-error", "4", &failingBody{}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			a, mux := testAPI(t)
			response := httptest.NewRecorder()
			mux.ServeHTTP(response, httptest.NewRequest("POST", "/api/files/upload?name=test.bin&size="+tc.size, tc.body))
			if response.Code != 400 {
				t.Fatalf("status %d, body %s", response.Code, response.Body)
			}
			if len(a.db.Files) != 0 || len(a.db.Tasks) != 0 {
				t.Fatal("invalid upload was published")
			}
			entries, err := os.ReadDir(filepath.Join(a.data, "chunks"))
			if err != nil && !os.IsNotExist(err) {
				t.Fatal(err)
			}
			if len(entries) != 0 {
				t.Fatal("invalid upload left chunks on disk")
			}
		})
	}
}

func TestUploadAndDownloadPreserveExactBytesAndMetadata(t *testing.T) {
	a, mux := testAPI(t)
	data := bytes.Repeat([]byte{'a', 0, 'z'}, (ChunkSize/3)+17)
	response := httptest.NewRecorder()
	mux.ServeHTTP(response, httptest.NewRequest("POST", fmt.Sprintf("/api/files/upload?name=payload.bin&size=%d", len(data)), bytes.NewReader(data)))
	if response.Code != 200 {
		t.Fatalf("upload %d: %s", response.Code, response.Body)
	}
	var result struct {
		FileID string `json:"fileId"`
		Size   int64  `json:"size"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	stored := a.db.Files[result.FileID]
	if stored.Size != int64(len(data)) || result.Size != stored.Size || len(stored.Chunks) != 2 {
		t.Fatalf("incorrect upload metadata: %+v", stored)
	}
	response = httptest.NewRecorder()
	mux.ServeHTTP(response, httptest.NewRequest("GET", "/api/files/download?id="+result.FileID, nil))
	if response.Code != 200 || !bytes.Equal(response.Body.Bytes(), data) {
		t.Fatal("download does not match uploaded bytes")
	}
	if got := response.Header().Get("Content-Length"); got != fmt.Sprint(len(data)) {
		t.Fatalf("wrong download length %s", got)
	}
}

func TestReadResponsesAreSnapshotsUnderConcurrentMutation(t *testing.T) {
	a, mux := testAPI(t)
	a.db.Nodes["pc"] = &Node{ID: "pc", Online: true, Status: "active"}
	a.db.Tasks["task"] = &Task{ID: "task", NodeID: "pc", Status: "pending"}
	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		for index := range 1000 {
			a.db.mu.Lock()
			a.db.Nodes["pc"].Name = fmt.Sprint(index)
			a.db.Nodes["pc"].EarningsCents = float64(index)
			a.db.Nodes["pc"].VerifiedUptimeSec = int64(index)
			a.db.Tasks["task"].Status = fmt.Sprint(index)
			a.db.mu.Unlock()
		}
	}()
	for range 100 {
		for _, endpoint := range []string{"/api/dashboard", "/api/nodes/pc", "/api/nodes/pc/tasks"} {
			response := httptest.NewRecorder()
			mux.ServeHTTP(response, httptest.NewRequest("GET", endpoint, nil))
			if response.Code != 200 {
				t.Fatalf("%s: %d", endpoint, response.Code)
			}
		}
	}
	wg.Wait()
}

func TestChunkDownloadRejectsEncodedTraversalAndUnknownMetadata(t *testing.T) {
	a, mux := testAPI(t)
	if err := os.MkdirAll(filepath.Join(a.data, "chunks"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(a.data, "0.bin"), []byte("outside chunks"), 0600); err != nil {
		t.Fatal(err)
	}
	response := httptest.NewRecorder()
	mux.ServeHTTP(response, httptest.NewRequest("GET", "/api/files/..%2f/chunks/0", nil))
	if response.Code != 404 {
		t.Fatalf("escaped fileId exposed data: %d %s", response.Code, response.Body)
	}
	if err := os.MkdirAll(filepath.Join(a.data, "chunks", "f_test"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(a.data, "chunks", "f_test", "0.bin"), []byte("unregistered"), 0600); err != nil {
		t.Fatal(err)
	}
	response = httptest.NewRecorder()
	mux.ServeHTTP(response, httptest.NewRequest("GET", "/api/files/f_test/chunks/0", nil))
	if response.Code != 404 {
		t.Fatalf("unregistered file served: %d", response.Code)
	}
	a.db.Files["f_test"] = &File{ID: "f_test", Chunks: []Chunk{{Index: 0, Size: 12}}}
	response = httptest.NewRecorder()
	mux.ServeHTTP(response, httptest.NewRequest("GET", "/api/files/f_test/chunks/0", nil))
	if response.Code != 200 || response.Body.String() != "unregistered" {
		t.Fatalf("known chunk download failed: %d %s", response.Code, response.Body)
	}
}

func TestMutationDoesNotAcknowledgeFailedPersistence(t *testing.T) {
	for _, endpoint := range []string{"register", "upload"} {
		t.Run(endpoint, func(t *testing.T) {
			a, mux := testAPI(t)
			blockedParent := filepath.Join(a.data, "not-a-directory")
			if err := os.WriteFile(blockedParent, []byte("file"), 0600); err != nil {
				t.Fatal(err)
			}
			a.db.path = filepath.Join(blockedParent, "db.json")
			var request *http.Request
			if endpoint == "register" {
				request = httptest.NewRequest("POST", "/api/nodes/register", strings.NewReader(`{"id":"pc","agreementAccepted":true}`))
			} else {
				request = httptest.NewRequest("POST", "/api/files/upload?name=payload.bin&size=3", strings.NewReader("abc"))
			}
			response := httptest.NewRecorder()
			mux.ServeHTTP(response, request)
			if response.Code != http.StatusInternalServerError {
				t.Fatalf("unsaved mutation acknowledged: %d %s", response.Code, response.Body)
			}
			var result map[string]any
			if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil {
				t.Fatal(err)
			}
			if result["error"] == nil || result["ok"] == true {
				t.Fatalf("not an explicit save failure: %v", result)
			}
		})
	}
}

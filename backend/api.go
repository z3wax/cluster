package main

import (
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
)

type api struct {
	db   *DB
	data string // chunks papkasi (data/chunks)
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(v)
}

func writeErr(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, map[string]any{"error": msg})
}

// save reports durability failures instead of acknowledging an unsaved mutation.
// The in-memory mutation remains available for a retry; this is not a rollback.
func (a *api) save(w http.ResponseWriter) bool {
	if err := a.db.Save(); err != nil {
		log.Printf("DB saqlashda xato: %v", err)
		writeErr(w, http.StatusInternalServerError, "ma'lumotlarni saqlashda xato")
		return false
	}
	return true
}

// ---------------- Node register ----------------

func (a *api) register(w http.ResponseWriter, r *http.Request) {
	var req struct {
		ID                string `json:"id"`
		Name              string `json:"name"`
		OS                string `json:"os"`
		Platform          string `json:"platform"`
		SpaceTotal        int64  `json:"spaceTotal"`
		AgreementVersion  string `json:"agreementVersion"`
		AgreementAccepted bool   `json:"agreementAccepted"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeErr(w, 400, "noto'g'ri so'rov")
		return
	}
	a.db.mu.Lock()
	n, exists := a.db.Nodes[req.ID]
	if !exists {
		n = &Node{ID: req.ID, JoinedAt: now()}
		a.db.Nodes[req.ID] = n
	}
	n.Name = req.Name
	n.OS = req.OS
	n.Platform = req.Platform
	n.SpaceTotal = req.SpaceTotal
	n.AgreementVersion = req.AgreementVersion
	n.AgreementAccepted = req.AgreementAccepted
	n.Status = "active"
	n.Online = true
	n.LastSeen = now()
	a.db.mu.Unlock()
	if !a.save(w) {
		return
	}

	writeJSON(w, 200, map[string]any{
		"ok":                  true,
		"nodeId":              n.ID,
		"heartbeatIntervalMs": 30000,
		"replicaRatio":        ReplicaRatio,
		"rateCentsPerGBHour":  RateCentsPerGBH,
		"serverTime":          now(),
	})
}

// ---------------- Heartbeat ----------------

func (a *api) heartbeat(w http.ResponseWriter, r *http.Request) {
	var req struct {
		ID        string `json:"id"`
		SpaceUsed int64  `json:"spaceUsed"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeErr(w, 400, "noto'g'ri so'rov")
		return
	}
	a.db.mu.Lock()
	n, ok := a.db.Nodes[req.ID]
	if !ok {
		a.db.mu.Unlock()
		writeErr(w, 404, "node topilmadi — avval register qiling")
		return
	}
	ts := now()
	elapsed := ts - n.LastSeen
	if n.LastSeen == 0 || !n.Online || elapsed > int64(HeartbeatTimeout.Seconds()) {
		elapsed = 0 // yangi, oflayn yoki uzilish bo'lgan tugunlar uchun vaqt hisoblanmaydi
	}
	if elapsed > 0 {
		a.db.addEarnings(n, elapsed)
		n.VerifiedUptimeSec += elapsed
	}
	n.LastSeen = ts
	n.Online = true
	n.SpaceUsed = req.SpaceUsed

	tasksDue := 0
	for _, t := range a.db.Tasks {
		if t.NodeID == req.ID && t.Status == "pending" {
			tasksDue++
		}
	}
	earnings, uptime := n.EarningsCents, n.VerifiedUptimeSec
	a.db.mu.Unlock()
	if !a.save(w) {
		return
	}

	writeJSON(w, 200, map[string]any{
		"ok":             true,
		"tasksDue":       tasksDue,
		"earningsCents":  earnings,
		"verifiedUptime": uptime,
		"serverTime":     ts,
	})
}

// ---------------- Upload (server PC yoki saytdan) ----------------

func (a *api) upload(w http.ResponseWriter, r *http.Request) {
	name := r.URL.Query().Get("name")
	sizeStr := r.URL.Query().Get("size")
	if name == "" || sizeStr == "" {
		writeErr(w, 400, "name va size kerak")
		return
	}
	size, err := strconv.ParseInt(sizeStr, 10, 64)
	if err != nil || size < 0 {
		writeErr(w, 400, "size noto'g'ri")
		return
	}
	defer r.Body.Close()

	fileID := randomID("f")
	dir := filepath.Join(a.data, "chunks", fileID)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		writeErr(w, 500, err.Error())
		return
	}

	committed := false
	defer func() {
		if !committed {
			os.RemoveAll(dir)
		}
	}()
	// Bound reads to the declared size plus one byte, so oversized bodies are
	// rejected without filling disk. A mismatch must never become a valid file.
	body := io.LimitReader(r.Body, size)
	buf := make([]byte, ChunkSize)
	var chunks []Chunk
	idx := 0
	var total int64
	for {
		n, err := io.ReadFull(body, buf)
		if n > 0 {
			cp := filepath.Join(dir, fmt.Sprintf("%d.bin", idx))
			if werr := os.WriteFile(cp, buf[:n], 0o644); werr != nil {
				writeErr(w, 500, werr.Error())
				return
			}
			chunks = append(chunks, Chunk{Index: idx, Size: int64(n)})
			total += int64(n)
			idx++
		}
		if err == io.ErrUnexpectedEOF || err == io.EOF {
			break
		}
		if err != nil {
			writeErr(w, 400, "faylni o'qishda xato")
			return
		}
	}
	if total != size {
		writeErr(w, 400, "fayl hajmi size bilan mos emas")
		return
	}
	var extra [1]byte
	if count, err := io.ReadFull(r.Body, extra[:]); count != 0 || err != io.EOF {
		writeErr(w, 400, "fayl hajmi size bilan mos emas")
		return
	}

	a.db.mu.Lock()
	nodes := a.db.onlineNodes()
	sort.Slice(nodes, func(i, j int) bool { return nodes[i].LastSeen < nodes[j].LastSeen })
	picked := replicaPick(nodes)
	f := &File{
		ID:            fileID,
		Name:          name,
		Size:          size,
		ChunkSize:     ChunkSize,
		Chunks:        chunks,
		UploadedAt:    now(),
		ReplicaTarget: len(picked),
		ReplicasDone:  0,
		Status:        "active",
	}
	a.db.Files[fileID] = f
	committed = true
	for _, nd := range picked {
		t := &Task{
			ID:         randomID("t"),
			NodeID:     nd.ID,
			FileID:     fileID,
			Chunks:     chunks,
			Status:     "pending",
			AssignedAt: now(),
			Kind:       "replica",
		}
		a.db.Tasks[t.ID] = t
	}
	a.db.mu.Unlock()
	if !a.save(w) {
		return
	}

	writeJSON(w, 200, map[string]any{
		"ok":            true,
		"fileId":        fileID,
		"size":          total,
		"chunks":        len(chunks),
		"replicaTarget": f.ReplicaTarget,
		"tasksCreated":  f.ReplicaTarget,
	})
}

// ---------------- Vazifalar (tasks) ----------------

func (a *api) nodeTasks(w http.ResponseWriter, r *http.Request) {
	nodeID := r.PathValue("id")
	a.db.mu.RLock()
	var out []Task
	for _, t := range a.db.Tasks {
		if t.NodeID == nodeID && t.Status == "pending" {
			out = append(out, *t)
		}
	}
	a.db.mu.RUnlock()
	writeJSON(w, 200, map[string]any{"tasks": out})
}

func (a *api) chunkDownload(w http.ResponseWriter, r *http.Request) {
	fileID := r.PathValue("fileId")
	idxStr := r.PathValue("idx")
	idx, err := strconv.Atoi(idxStr)
	if err != nil || idx < 0 {
		writeErr(w, 400, "idx noto'g'ri")
		return
	}
	// PathValue decodes escaped slashes: never use an unverified fileId to
	// construct a filesystem path (e.g. an encoded ../ outside chunks/).
	a.db.mu.RLock()
	file, exists := a.db.Files[fileID]
	validChunk := false
	if exists {
		for _, chunk := range file.Chunks {
			if chunk.Index == idx {
				validChunk = true
				break
			}
		}
	}
	a.db.mu.RUnlock()
	if !validChunk {
		writeErr(w, 404, "chunk topilmadi")
		return
	}
	path := filepath.Join(a.data, "chunks", fileID, fmt.Sprintf("%d.bin", idx))
	if _, err := os.Stat(path); err != nil {
		writeErr(w, 404, "chunk topilmadi")
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	http.ServeFile(w, r, path)
}

func (a *api) downloadFile(w http.ResponseWriter, r *http.Request) {
	fileID := r.URL.Query().Get("id")
	if fileID == "" {
		fileID = r.PathValue("id")
	}
	if fileID == "" {
		writeErr(w, 400, "fayl ID kerak")
		return
	}
	a.db.mu.RLock()
	f, ok := a.db.Files[fileID]
	a.db.mu.RUnlock()
	if !ok {
		writeErr(w, 404, "fayl topilmadi")
		return
	}

	w.Header().Set("Content-Disposition", fmt.Sprintf("attachment; filename=\"%s\"", f.Name))
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Length", strconv.FormatInt(f.Size, 10))

	for i := range f.Chunks {
		chunkPath := filepath.Join(a.data, "chunks", f.ID, fmt.Sprintf("%d.bin", i))
		b, err := os.ReadFile(chunkPath)
		if err != nil {
			// fallback if local server chunk missing
			writeErr(w, 500, fmt.Sprintf("chunk %d ni o'qishda xato", i))
			return
		}
		w.Write(b)
	}
}

func (a *api) taskDone(w http.ResponseWriter, r *http.Request) {
	nodeID := r.PathValue("id")
	taskID := r.PathValue("taskId")
	a.db.mu.Lock()
	t, ok := a.db.Tasks[taskID]
	if !ok || t.NodeID != nodeID {
		a.db.mu.Unlock()
		writeErr(w, 404, "task topilmadi")
		return
	}
	if t.Status != "done" {
		t.Status = "done"
		t.DoneAt = now()
		if f, ok := a.db.Files[t.FileID]; ok {
			if t.Kind == "replica" {
				f.ReplicasDone++
				if f.ReplicasDone >= f.ReplicaTarget {
					f.Status = "active"
				}
			}
			// fayl tugunga biriktiriladi (verified)
			n := a.db.Nodes[nodeID]
			if n != nil && !contains(n.StoredFiles, f.ID) {
				n.StoredFiles = append(n.StoredFiles, f.ID)
			}
		}
	}
	a.db.mu.Unlock()
	if !a.save(w) {
		return
	}
	writeJSON(w, 200, map[string]any{"ok": true})
}

// repair — foydalanuvchi faylni o'chirgan bo'lsa, qayta yuklab olish vazifasi
func (a *api) repair(w http.ResponseWriter, r *http.Request) {
	nodeID := r.PathValue("id")
	var req struct {
		FileID string `json:"fileId"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.FileID == "" {
		writeErr(w, 400, "fileId kerak")
		return
	}
	a.db.mu.Lock()
	f, ok := a.db.Files[req.FileID]
	if !ok {
		a.db.mu.Unlock()
		writeErr(w, 404, "fayl topilmadi")
		return
	}
	// Fayl o'chirilgan — qayta yuklangungacha StoredFiles dan olib tashlaymiz (to'lov to'xtaydi - 3.3 band)
	if n, ok := a.db.Nodes[nodeID]; ok {
		var filtered []string
		for _, sf := range n.StoredFiles {
			if sf != req.FileID {
				filtered = append(filtered, sf)
			}
		}
		n.StoredFiles = filtered
	}
	t := &Task{
		ID:         randomID("t"),
		NodeID:     nodeID,
		FileID:     f.ID,
		Chunks:     f.Chunks,
		Status:     "pending",
		AssignedAt: now(),
		Kind:       "repair",
	}
	a.db.Tasks[t.ID] = t
	a.db.mu.Unlock()
	if !a.save(w) {
		return
	}
	writeJSON(w, 200, map[string]any{"ok": true, "taskId": t.ID})
}

// ---------------- Node info / uninstall / payout ----------------

func (a *api) nodeInfo(w http.ResponseWriter, r *http.Request) {
	nodeID := r.PathValue("id")
	a.db.mu.RLock()
	n, ok := a.db.Nodes[nodeID]
	if !ok {
		a.db.mu.RUnlock()
		writeErr(w, 404, "node topilmadi")
		return
	}
	stored := a.db.nodeStoredBytes(n)
	var pending int
	for _, t := range a.db.Tasks {
		if t.NodeID == nodeID && t.Status == "pending" {
			pending++
		}
	}
	node := *n
	node.StoredFiles = append([]string(nil), n.StoredFiles...)
	a.db.mu.RUnlock()
	writeJSON(w, 200, map[string]any{
		"node":               node,
		"storedBytes":        stored,
		"pendingTasks":       pending,
		"rateCentsPerGBHour": RateCentsPerGBH,
	})
}

func (a *api) uninstall(w http.ResponseWriter, r *http.Request) {
	nodeID := r.PathValue("id")
	a.db.mu.Lock()
	a.db.uninstallNode(nodeID)
	a.db.mu.Unlock()
	if !a.save(w) {
		return
	}
	writeJSON(w, 200, map[string]any{"ok": true, "note": "payout olinmagan oy yo'qotiladi (shartnoma 6.2-band)"})
}

func (a *api) claim(w http.ResponseWriter, r *http.Request) {
	var req struct {
		NodeID string `json:"nodeId"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.NodeID == "" {
		writeErr(w, 400, "nodeId kerak")
		return
	}
	a.db.mu.Lock()
	p, err := a.db.claimPayout(req.NodeID, currentPeriod(time.Now()))
	a.db.mu.Unlock()
	if err != nil {
		writeErr(w, 404, err.Error())
		return
	}
	if !a.save(w) {
		return
	}
	writeJSON(w, 200, map[string]any{
		"ok":     true,
		"payout": p,
		"note":   "Oy oxiridagi payout rasmiylashtirildi",
	})
}

// ---------------- Dashboard / cluster stats ----------------

func (a *api) info(w http.ResponseWriter, r *http.Request) {
	a.db.mu.RLock()
	defer a.db.mu.RUnlock()
	totalNodes := len(a.db.Nodes)
	online := 0
	var totalSpace, usedSpace, storedBytes int64
	var totalFiles, doneReplicas, pendingTasks int
	var paidCents float64
	for _, n := range a.db.Nodes {
		if n.Online {
			online++
		}
		if n.Status == "active" {
			totalSpace += n.SpaceTotal
			usedSpace += n.SpaceUsed
			storedBytes += a.db.nodeStoredBytes(n)
		}
	}
	totalFiles = len(a.db.Files)
	for _, t := range a.db.Tasks {
		if t.Status == "pending" {
			pendingTasks++
		}
	}
	for _, f := range a.db.Files {
		doneReplicas += f.ReplicasDone
	}
	for _, p := range a.db.Payouts {
		if p.Status == "paid" {
			paidCents += p.AmountCents
		}
	}
	writeJSON(w, 200, map[string]any{
		"totalNodes":   totalNodes,
		"onlineNodes":  online,
		"totalSpace":   totalSpace,
		"usedSpace":    usedSpace,
		"storedBytes":  storedBytes,
		"totalFiles":   totalFiles,
		"doneReplicas": doneReplicas,
		"pendingTasks": pendingTasks,
		"paidCents":    paidCents,
		"replicaRatio": ReplicaRatio,
		"rate":         RateCentsPerGBH,
		"serverTime":   now(),
	})
}

func (a *api) dashboard(w http.ResponseWriter, r *http.Request) {
	a.db.mu.RLock()
	nodes := make([]Node, 0, len(a.db.Nodes))
	for _, n := range a.db.Nodes {
		nodes = append(nodes, *n)
	}
	files := make([]File, 0, len(a.db.Files))
	for _, f := range a.db.Files {
		files = append(files, *f)
	}
	tasks := make([]Task, 0, len(a.db.Tasks))
	for _, t := range a.db.Tasks {
		tasks = append(tasks, *t)
	}
	payouts := make([]Payout, 0, len(a.db.Payouts))
	for _, p := range a.db.Payouts {
		payouts = append(payouts, *p)
	}
	a.db.mu.RUnlock()
	sort.Slice(nodes, func(i, j int) bool { return nodes[i].JoinedAt > nodes[j].JoinedAt })
	sort.Slice(files, func(i, j int) bool { return files[i].UploadedAt > files[j].UploadedAt })
	sort.Slice(tasks, func(i, j int) bool { return tasks[i].AssignedAt > tasks[j].AssignedAt })
	writeJSON(w, 200, map[string]any{
		"nodes":   nodes,
		"files":   files,
		"tasks":   tasks,
		"payouts": payouts,
	})
}

// ---------------- Agreement ----------------

func (a *api) agreement(w http.ResponseWriter, r *http.Request) {
	root, _ := r.Context().Value("root").(string)
	paths := []string{
		filepath.Join(root, "SHARTNOMA.md"),
		filepath.Join(root, "..", "SHARTNOMA.md"),
		"SHARTNOMA.md",
		"../SHARTNOMA.md",
	}
	var b []byte
	var err error
	for _, p := range paths {
		if b, err = os.ReadFile(p); err == nil && len(b) > 0 {
			break
		}
	}
	if err != nil || len(b) == 0 {
		writeErr(w, 500, "shartnoma topilmadi")
		return
	}
	writeJSON(w, 200, map[string]any{"version": "1.0", "text": string(b)})
}

// ---------------- Router ----------------

func (a *api) routes(mux *http.ServeMux) {
	mux.HandleFunc("GET /api/info", a.info)
	mux.HandleFunc("GET /api/dashboard", a.dashboard)
	mux.HandleFunc("GET /api/agreement", a.agreement)
	mux.HandleFunc("POST /api/nodes/register", a.register)
	mux.HandleFunc("POST /api/nodes/heartbeat", a.heartbeat)
	mux.HandleFunc("POST /api/files/upload", a.upload)
	mux.HandleFunc("GET /api/files/download", a.downloadFile)
	mux.HandleFunc("GET /api/files/{id}/download", a.downloadFile)
	mux.HandleFunc("GET /api/nodes/{id}", a.nodeInfo)
	mux.HandleFunc("GET /api/nodes/{id}/tasks", a.nodeTasks)
	mux.HandleFunc("POST /api/nodes/{id}/tasks/{taskId}/done", a.taskDone)
	mux.HandleFunc("POST /api/nodes/{id}/repair", a.repair)
	mux.HandleFunc("POST /api/nodes/{id}/uninstall", a.uninstall)
	mux.HandleFunc("POST /api/payouts/claim", a.claim)
	mux.HandleFunc("GET /api/files/{fileId}/chunks/{idx}", a.chunkDownload)
}

// go:embed
var _ = strings.TrimSpace

package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"mime"
	"net/http"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"unicode"
	"unicode/utf8"
)

var nodeIDRe = regexp.MustCompile(`^[A-Za-z0-9_.-]{4,80}$`)

func clip(s string, n int) string {
	s = strings.TrimSpace(s)
	for len(s) > n {
		_, size := utf8.DecodeLastRuneInString(s)
		s = s[:len(s)-size]
	}
	return s
}

// sanitizeName — fayl nomidan papka qismlari va boshqaruv belgilarini olib tashlaydi
func sanitizeName(name string) string {
	name = strings.ReplaceAll(name, "\\", "/")
	name = path.Base(name)
	name = strings.Map(func(r rune) rune {
		if unicode.IsControl(r) {
			return -1
		}
		return r
	}, name)
	name = clip(name, 200)
	if name == "." || name == "/" || name == ".." {
		return ""
	}
	return name
}

// ---------------- Ochiq API ----------------

func (s *Server) handleInfo(w http.ResponseWriter, r *http.Request) {
	s.db.mu.Lock()
	defer s.db.mu.Unlock()
	var totalNodes, online, active int
	var totalSpace, usedSpace, storedBytes, fileBytes int64
	for _, n := range s.db.Nodes {
		if n.Status != NodeActive {
			continue
		}
		totalNodes++
		active++
		if n.Online {
			online++
		}
		totalSpace += n.SpaceTotal
		usedSpace += n.SpaceUsed
		storedBytes += s.db.nodeStoredBytes(n)
	}
	healthy := 0
	for _, f := range s.db.Files {
		fileBytes += f.Size
		if f.Status == FileHealthy {
			healthy++
		}
	}
	pending := 0
	for _, t := range s.db.Tasks {
		if t.Status == TaskPending {
			pending++
		}
	}
	var paid, forfeited float64
	for _, p := range s.db.Payouts {
		if p.Status == "paid" {
			paid += p.AmountCents
		} else {
			forfeited += p.AmountCents
		}
	}
	open, opens, closes := s.payoutWindow()
	writeJSON(w, http.StatusOK, map[string]any{
		"totalNodes":          totalNodes,
		"onlineNodes":         online,
		"totalSpace":          totalSpace,
		"usedSpace":           usedSpace,
		"storedBytes":         storedBytes,
		"totalFiles":          len(s.db.Files),
		"healthyFiles":        healthy,
		"fileBytes":           fileBytes,
		"pendingTasks":        pending,
		"paidCents":           paid,
		"forfeitedCents":      forfeited,
		"replicaRatio":        s.cfg.ReplicaRatio,
		"replicaTarget":       s.desiredReplicasLocked(),
		"rate":                s.cfg.RateCentsPerGBHour,
		"chunkSize":           s.cfg.ChunkSize,
		"heartbeatIntervalMs": s.cfg.HeartbeatInterval.Milliseconds(),
		"agreementVersion":    s.agreementVersion,
		"payoutWindow":        map[string]any{"open": open, "opensAt": opens.Unix(), "closesAt": closes.Unix()},
		"serverTime":          s.now(),
	})
}

func (s *Server) handleAgreement(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{"version": s.agreementVersion, "text": s.agreementText})
}

// ---------------- Tugun: ro'yxatdan o'tish ----------------

func (s *Server) handleRegister(w http.ResponseWriter, r *http.Request) {
	var req struct {
		ID                string `json:"id"`
		Name              string `json:"name"`
		OS                string `json:"os"`
		Platform          string `json:"platform"`
		Version           string `json:"version"`
		CPUCores          int    `json:"cpuCores"`
		SpaceTotal        int64  `json:"spaceTotal"`
		AgreementVersion  string `json:"agreementVersion"`
		AgreementAccepted bool   `json:"agreementAccepted"`
	}
	if !readJSON(w, r, &req) {
		return
	}
	if !nodeIDRe.MatchString(req.ID) {
		writeErr(w, http.StatusBadRequest, "tugun ID noto'g'ri (4-80 belgi: harf, raqam, _ . -)")
		return
	}
	if req.SpaceTotal <= 0 {
		writeErr(w, http.StatusBadRequest, "spaceTotal (ajratilgan joy) 0 dan katta bo'lishi kerak")
		return
	}
	if !req.AgreementAccepted {
		// Shartnoma 9.3: rozilik bo'lmasa dastur ma'lumot saqlamaydi
		writeErr(w, http.StatusPreconditionFailed, "shartnoma qabul qilinmagan — tugun klasterga qo'shilmaydi (9.3-band)")
		return
	}
	if req.AgreementVersion != s.agreementVersion {
		writeJSON(w, http.StatusPreconditionFailed, map[string]any{
			"ok":               false,
			"error":            "shartnomaning yangi versiyasi chiqdi — qayta o'qib, rozilik bildiring",
			"agreementVersion": s.agreementVersion,
		})
		return
	}

	s.db.mu.Lock()
	ts := s.now()
	n, exists := s.db.Nodes[req.ID]
	token := ""
	if exists {
		if n.Status != NodeActive {
			s.db.mu.Unlock()
			writeErr(w, http.StatusGone, "bu tugun o'chirilgan (uninstall) — qayta o'rnatishda yangi ID ishlatiladi")
			return
		}
		if hash, has := s.db.Tokens[req.ID]; has {
			if !tokenEqual(sha256Hex([]byte(bearer(r))), hash) {
				s.db.mu.Unlock()
				writeErr(w, http.StatusConflict, "bu tugun ID boshqa kompyuterga tegishli")
				return
			}
		} else {
			token = randomHex(32) // eski (tokensiz) versiyadan o'tish
		}
	} else {
		n = &Node{ID: req.ID, JoinedAt: ts, Status: NodeActive}
		s.db.Nodes[req.ID] = n
		token = randomHex(32)
	}
	if token != "" {
		s.db.Tokens[req.ID] = sha256Hex([]byte(token))
	}
	n.Name = clip(req.Name, 64)
	n.OS = clip(req.OS, 64)
	n.Platform = clip(req.Platform, 32)
	n.Version = clip(req.Version, 32)
	n.CPUCores = req.CPUCores
	n.SpaceTotal = req.SpaceTotal
	n.AgreementVersion = req.AgreementVersion
	n.AgreementAccepted = true
	n.Online = true
	n.LastSeen = ts
	s.db.markDirty()
	s.db.mu.Unlock()
	s.persist()
	s.triggerReconcile()

	resp := map[string]any{
		"ok":                  true,
		"nodeId":              req.ID,
		"heartbeatIntervalMs": s.cfg.HeartbeatInterval.Milliseconds(),
		"offlineTimeoutMs":    s.cfg.OfflineTimeout.Milliseconds(),
		"replicaRatio":        s.cfg.ReplicaRatio,
		"rateCentsPerGBHour":  s.cfg.RateCentsPerGBHour,
		"chunkSize":           s.cfg.ChunkSize,
		"agreementVersion":    s.agreementVersion,
		"serverTime":          ts,
	}
	if token != "" {
		resp["token"] = token
	}
	writeJSON(w, http.StatusOK, resp)
}

// ---------------- Tugun: heartbeat ----------------

func (s *Server) handleHeartbeat(w http.ResponseWriter, r *http.Request) {
	var req struct {
		SpaceUsed  int64 `json:"spaceUsed"`
		UserActive bool  `json:"userActive"`
	}
	if !readJSON(w, r, &req) {
		return
	}
	s.db.mu.Lock()
	n := s.authNodeLocked(w, r)
	if n == nil {
		s.db.mu.Unlock()
		return
	}
	ts := s.now()
	wasOnline := n.Online
	elapsed := ts - n.LastSeen
	if !n.Online || n.LastSeen == 0 || elapsed < 0 || elapsed > int64(s.cfg.OfflineTimeout.Seconds()) {
		elapsed = 0 // yangi, oflayn yoki uzilish bo'lgan vaqt hisoblanmaydi (shartnoma 5.1, 5.2)
	}
	if elapsed > 0 {
		s.addEarningsLocked(n, elapsed)
		n.VerifiedUptimeSec += elapsed
	}
	n.LastSeen = ts
	n.Online = true
	if req.SpaceUsed >= 0 {
		n.SpaceUsed = req.SpaceUsed
	}
	n.UserActive = req.UserActive
	s.db.markDirty()

	open, opens, _ := s.payoutWindow()
	resp := map[string]any{
		"ok":                  true,
		"tasksDue":            len(s.db.pendingTasksOf(n.ID)),
		"audits":              s.issueAuditsLocked(n, ts),
		"earningsCents":       n.EarningsCents,
		"verifiedUptime":      n.VerifiedUptimeSec,
		"storedBytes":         s.db.nodeStoredBytes(n),
		"storedFiles":         len(n.StoredFiles),
		"heartbeatIntervalMs": s.cfg.HeartbeatInterval.Milliseconds(),
		"payoutOpen":          open,
		"payoutOpensAt":       opens.Unix(),
		"serverTime":          ts,
	}
	s.db.mu.Unlock()
	if !wasOnline {
		s.triggerReconcile() // tugun qaytdi — vazifalar qayta taqsimlanadi
	}
	writeJSON(w, http.StatusOK, resp)
}

// ---------------- Tugun: ma'lumot ----------------

func (s *Server) handleNodeInfo(w http.ResponseWriter, r *http.Request) {
	isAdm, _ := s.isAdmin(r)
	s.db.mu.Lock()
	defer s.db.mu.Unlock()
	var n *Node
	if isAdm {
		n = s.db.Nodes[r.PathValue("id")]
		if n == nil {
			writeErr(w, http.StatusNotFound, "tugun topilmadi")
			return
		}
	} else if n = s.authNodeLocked(w, r); n == nil {
		return
	}
	open, opens, _ := s.payoutWindow()
	writeJSON(w, http.StatusOK, map[string]any{
		"node":               n,
		"storedBytes":        s.db.nodeStoredBytes(n),
		"pendingTasks":       len(s.db.pendingTasksOf(n.ID)),
		"rateCentsPerGBHour": s.cfg.RateCentsPerGBHour,
		"payoutOpen":         open,
		"payoutOpensAt":      opens.Unix(),
	})
}

// ---------------- Tugun: vazifalar ----------------

func (s *Server) handleNodeTasks(w http.ResponseWriter, r *http.Request) {
	s.db.mu.Lock()
	defer s.db.mu.Unlock()
	n := s.authNodeLocked(w, r)
	if n == nil {
		return
	}
	type taskView struct {
		ID     string  `json:"id"`
		Kind   string  `json:"kind"`
		FileID string  `json:"fileId"`
		Chunks []Chunk `json:"chunks"`
	}
	out := []taskView{}
	for _, t := range s.db.pendingTasksOf(n.ID) {
		chunks := t.Chunks
		if chunks == nil {
			chunks = []Chunk{}
		}
		out = append(out, taskView{ID: t.ID, Kind: t.Kind, FileID: t.FileID, Chunks: chunks})
	}
	writeJSON(w, http.StatusOK, map[string]any{"tasks": out})
}

func findChunk(f *File, idx int) (Chunk, bool) {
	for _, c := range f.Chunks {
		if c.Index == idx {
			return c, true
		}
	}
	return Chunk{}, false
}

// handleNodeChunk — tugun vazifasi uchun chunk yuklab olish (faqat vazifa bor bo'lsa)
func (s *Server) handleNodeChunk(w http.ResponseWriter, r *http.Request) {
	idx, err := strconv.Atoi(r.PathValue("idx"))
	if err != nil {
		writeErr(w, http.StatusBadRequest, "chunk indeksi noto'g'ri")
		return
	}
	s.db.mu.Lock()
	n := s.authNodeLocked(w, r)
	if n == nil {
		s.db.mu.Unlock()
		return
	}
	f, ok := s.db.Files[r.PathValue("fileId")]
	if !ok {
		s.db.mu.Unlock()
		writeErr(w, http.StatusGone, "fayl o'chirilgan")
		return
	}
	c, ok := findChunk(f, idx)
	if !ok {
		s.db.mu.Unlock()
		writeErr(w, http.StatusNotFound, "chunk topilmadi")
		return
	}
	if !s.db.hasPendingTask(n.ID, f.ID, KindReplica, KindRepair) {
		s.db.mu.Unlock()
		writeErr(w, http.StatusForbidden, "bu fayl uchun vazifa yo'q")
		return
	}
	missing := containsInt(f.Missing, idx)
	fileID := f.ID
	s.db.mu.Unlock()

	if missing {
		writeErr(w, http.StatusServiceUnavailable, "server nusxasi tiklanmoqda")
		return
	}
	fh, err := os.Open(s.chunkPath(fileID, idx))
	if err != nil {
		s.markMissing(fileID, idx)
		writeErr(w, http.StatusServiceUnavailable, "server nusxasi yo'q — tiklanmoqda")
		return
	}
	defer fh.Close()
	fi, err := fh.Stat()
	if err != nil || fi.Size() != c.Size {
		s.markMissing(fileID, idx)
		writeErr(w, http.StatusServiceUnavailable, "server nusxasi buzilgan — tiklanmoqda")
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("X-Chunk-SHA256", c.SHA256)
	http.ServeContent(w, r, "", fi.ModTime(), fh)
}

// handleRestoreChunk — tugun o'zidagi nusxadan server chunk'ini tiklaydi (restore vazifasi)
func (s *Server) handleRestoreChunk(w http.ResponseWriter, r *http.Request) {
	idx, err := strconv.Atoi(r.PathValue("idx"))
	if err != nil {
		writeErr(w, http.StatusBadRequest, "chunk indeksi noto'g'ri")
		return
	}
	s.db.mu.Lock()
	n := s.authNodeLocked(w, r)
	if n == nil {
		s.db.mu.Unlock()
		return
	}
	t, ok := s.db.Tasks[r.PathValue("taskId")]
	if !ok || t.NodeID != n.ID || t.Kind != KindRestore || t.Status != TaskPending {
		s.db.mu.Unlock()
		writeErr(w, http.StatusNotFound, "restore vazifasi topilmadi")
		return
	}
	f, ok := s.db.Files[t.FileID]
	if !ok {
		s.db.mu.Unlock()
		writeErr(w, http.StatusGone, "fayl o'chirilgan")
		return
	}
	c, ok := findChunk(f, idx)
	inTask := false
	for _, tc := range t.Chunks {
		if tc.Index == idx {
			inTask = true
		}
	}
	nodeID, fileID := n.ID, f.ID
	s.db.mu.Unlock()
	if !ok || !inTask {
		writeErr(w, http.StatusBadRequest, "bu chunk vazifada yo'q")
		return
	}

	data, err := io.ReadAll(http.MaxBytesReader(w, r.Body, c.Size+1))
	if err != nil || int64(len(data)) != c.Size {
		writeErr(w, http.StatusBadRequest, fmt.Sprintf("chunk hajmi noto'g'ri (kutilgan %d bayt)", c.Size))
		return
	}
	if sha256Hex(data) != c.SHA256 {
		// tugundagi nusxa buzilgan — saqlash isboti o'tmadi, tiklash boshqa tugundan qilinadi
		s.db.mu.Lock()
		if t, ok := s.db.Tasks[r.PathValue("taskId")]; ok && t.Status == TaskPending {
			finishTask(t, TaskFailed, "sha256 mos emas", s.now())
		}
		s.auditFailedLocked(nodeID, fileID, "restore: sha256 mos emas")
		s.db.mu.Unlock()
		s.triggerReconcile()
		writeErr(w, http.StatusUnprocessableEntity, "sha256 mos emas — nusxa buzilgan")
		return
	}
	if err := writeFileAtomic(s.chunkPath(fileID, idx), data); err != nil {
		writeErr(w, http.StatusInternalServerError, "diskka yozib bo'lmadi: "+err.Error())
		return
	}
	s.db.mu.Lock()
	if f, ok := s.db.Files[fileID]; ok {
		var rest []int
		for _, m := range f.Missing {
			if m != idx {
				rest = append(rest, m)
			}
		}
		f.Missing = rest
		s.db.markDirty()
	}
	s.db.mu.Unlock()
	writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

func (s *Server) handleTaskDone(w http.ResponseWriter, r *http.Request) {
	s.db.mu.Lock()
	n := s.authNodeLocked(w, r)
	if n == nil {
		s.db.mu.Unlock()
		return
	}
	t, ok := s.db.Tasks[r.PathValue("taskId")]
	if !ok || t.NodeID != n.ID {
		s.db.mu.Unlock()
		writeErr(w, http.StatusNotFound, "vazifa topilmadi")
		return
	}
	switch t.Status {
	case TaskDone:
		s.db.mu.Unlock()
		writeJSON(w, http.StatusOK, map[string]any{"ok": true})
		return
	case TaskCancelled:
		s.db.mu.Unlock()
		writeJSON(w, http.StatusGone, map[string]any{"ok": false, "cancelled": true, "error": "vazifa bekor qilingan — lokal nusxani o'chiring"})
		return
	}
	f := s.db.Files[t.FileID]
	switch t.Kind {
	case KindReplica, KindRepair:
		if f == nil {
			t.Status = TaskCancelled
			s.db.markDirty()
			s.db.mu.Unlock()
			writeJSON(w, http.StatusGone, map[string]any{"ok": false, "cancelled": true, "error": "fayl o'chirilgan"})
			return
		}
		if !contains(n.StoredFiles, f.ID) {
			n.StoredFiles = append(n.StoredFiles, f.ID)
		}
		// yangi saqlangan nusxa birinchi navbatda tekshiriladi
		s.priorityAudit[n.ID] = append(s.priorityAudit[n.ID], f.ID)
	case KindRestore:
		if f != nil {
			for _, c := range t.Chunks {
				if containsInt(f.Missing, c.Index) {
					s.db.mu.Unlock()
					writeErr(w, http.StatusConflict, fmt.Sprintf("chunk %d hali yuklanmagan", c.Index))
					return
				}
			}
		}
	}
	t.Status = TaskDone
	t.DoneAt = s.now()
	t.Error = ""
	s.db.markDirty()
	s.db.mu.Unlock()
	s.triggerReconcile()
	writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

func (s *Server) handleTaskFail(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Error string `json:"error"`
	}
	if !readJSON(w, r, &req) {
		return
	}
	s.db.mu.Lock()
	n := s.authNodeLocked(w, r)
	if n == nil {
		s.db.mu.Unlock()
		return
	}
	t, ok := s.db.Tasks[r.PathValue("taskId")]
	if !ok || t.NodeID != n.ID {
		s.db.mu.Unlock()
		writeErr(w, http.StatusNotFound, "vazifa topilmadi")
		return
	}
	scrub := ""
	if t.Status == TaskPending {
		t.Status = TaskFailed
		t.Error = clip(req.Error, 300)
		t.DoneAt = s.now()
		s.db.markDirty()
		if (t.Kind == KindReplica || t.Kind == KindRepair) && strings.Contains(strings.ToLower(req.Error), "sha256") {
			scrub = t.FileID // tugun server chunk'i buzuq deb xabar berdi — tekshiramiz
		}
	}
	s.db.mu.Unlock()
	if scrub != "" {
		go s.scrubFile(scrub, true)
	}
	s.triggerReconcile()
	writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

// handleRepair — foydalanuvchi faylni o'chirgan bo'lsa, qayta yuklab olish vazifasi (shartnoma 3.2, 3.3)
func (s *Server) handleRepair(w http.ResponseWriter, r *http.Request) {
	var req struct {
		FileID  string   `json:"fileId"`
		FileIDs []string `json:"fileIds"`
	}
	if !readJSON(w, r, &req) {
		return
	}
	ids := req.FileIDs
	if req.FileID != "" {
		ids = append(ids, req.FileID)
	}
	if len(ids) == 0 {
		writeErr(w, http.StatusBadRequest, "fileId kerak")
		return
	}
	s.db.mu.Lock()
	n := s.authNodeLocked(w, r)
	if n == nil {
		s.db.mu.Unlock()
		return
	}
	ts := s.now()
	created := 0
	unknown := []string{}
	for _, fid := range ids {
		// qayta yuklanguncha to'lov to'xtaydi (3.3-band)
		n.StoredFiles = removeStr(n.StoredFiles, fid)
		f, ok := s.db.Files[fid]
		if !ok {
			unknown = append(unknown, fid)
			continue
		}
		if len(f.Missing) == 0 && !s.db.hasPendingTask(n.ID, fid, KindRepair, KindReplica) {
			s.db.newTask(n.ID, f, KindRepair, f.Chunks, ts)
			created++
		}
	}
	s.db.markDirty()
	s.db.mu.Unlock()
	s.triggerReconcile()
	s.sim.wakeAll()
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "tasksCreated": created, "unknown": unknown})
}

// ---------------- Admin: dashboard ----------------

func (s *Server) handleDashboard(w http.ResponseWriter, r *http.Request) {
	s.db.mu.Lock()
	nodes := make([]*Node, 0, len(s.db.Nodes))
	for _, n := range s.db.Nodes {
		nodes = append(nodes, n)
	}
	files := make([]*File, 0, len(s.db.Files))
	for _, f := range s.db.Files {
		files = append(files, f)
	}
	tasks := make([]*Task, 0, len(s.db.Tasks))
	counts := map[string]int{}
	for _, t := range s.db.Tasks {
		tasks = append(tasks, t)
		counts[t.Status]++
	}
	payouts := make([]*Payout, 0, len(s.db.Payouts))
	for _, p := range s.db.Payouts {
		payouts = append(payouts, p)
	}
	sort.Slice(nodes, func(i, j int) bool {
		if nodes[i].JoinedAt != nodes[j].JoinedAt {
			return nodes[i].JoinedAt > nodes[j].JoinedAt
		}
		return nodes[i].ID < nodes[j].ID
	})
	sort.Slice(files, func(i, j int) bool { return files[i].UploadedAt > files[j].UploadedAt })
	sort.Slice(tasks, func(i, j int) bool {
		if tasks[i].AssignedAt != tasks[j].AssignedAt {
			return tasks[i].AssignedAt > tasks[j].AssignedAt
		}
		return tasks[i].ID > tasks[j].ID
	})
	if len(tasks) > 300 {
		tasks = tasks[:300]
	}
	sort.Slice(payouts, func(i, j int) bool { return payouts[i].CreatedAt > payouts[j].CreatedAt })
	// mu ostida serializatsiya — maydonlar parallel o'zgarmasligi uchun
	b, err := json.Marshal(map[string]any{
		"nodes":      nodes,
		"files":      files,
		"tasks":      tasks,
		"taskCounts": counts,
		"payouts":    payouts,
		"serverTime": s.now(),
	})
	s.db.mu.Unlock()
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.Write(b)
}

func (s *Server) handleReconcileNow(w http.ResponseWriter, r *http.Request) {
	s.reconcile()
	writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

// ---------------- Admin: fayl yuklash ----------------

func (s *Server) handleUpload(w http.ResponseWriter, r *http.Request) {
	name := sanitizeName(r.URL.Query().Get("name"))
	if name == "" {
		writeErr(w, http.StatusBadRequest, "name (fayl nomi) kerak")
		return
	}
	expected := int64(-1)
	if v := r.URL.Query().Get("size"); v != "" {
		n, err := strconv.ParseInt(v, 10, 64)
		if err != nil || n < 0 {
			writeErr(w, http.StatusBadRequest, "size noto'g'ri")
			return
		}
		if n > s.cfg.MaxUploadBytes {
			writeErr(w, http.StatusRequestEntityTooLarge, "fayl juda katta")
			return
		}
		expected = n
	}
	body := http.MaxBytesReader(w, r.Body, s.cfg.MaxUploadBytes)
	defer body.Close()

	fileID := randomID("f")
	dir := filepath.Join(s.chunksDir, fileID)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	fail := func(status int, msg string) {
		os.RemoveAll(dir)
		writeErr(w, status, msg)
	}

	whole := newSHA256()
	buf := make([]byte, s.cfg.ChunkSize)
	var chunks []Chunk
	var total int64
	for idx := 0; ; idx++ {
		n, err := io.ReadFull(body, buf)
		if n > 0 {
			if werr := writeFileAtomic(s.chunkPath(fileID, idx), buf[:n]); werr != nil {
				fail(http.StatusInternalServerError, "chunk yozishda xato: "+werr.Error())
				return
			}
			whole.Write(buf[:n])
			chunks = append(chunks, Chunk{Index: idx, Size: int64(n), SHA256: sha256Hex(buf[:n])})
			total += int64(n)
		}
		if err == io.EOF || err == io.ErrUnexpectedEOF {
			break
		}
		if err != nil {
			var mbe *http.MaxBytesError
			if errors.As(err, &mbe) {
				fail(http.StatusRequestEntityTooLarge, "fayl juda katta")
			} else {
				fail(http.StatusBadRequest, "yuklash uzildi: "+err.Error())
			}
			return
		}
	}
	if total == 0 {
		fail(http.StatusBadRequest, "bo'sh fayl yuklab bo'lmaydi")
		return
	}
	if expected >= 0 && expected != total {
		fail(http.StatusBadRequest, fmt.Sprintf("hajm mos emas: e'lon qilingan %d, kelgan %d", expected, total))
		return
	}

	s.db.mu.Lock()
	f := &File{
		ID:         fileID,
		Name:       name,
		Size:       total,
		SHA256:     whole.hex(),
		ChunkSize:  s.cfg.ChunkSize,
		Chunks:     chunks,
		UploadedAt: s.now(),
		Status:     FileReplicating,
	}
	s.db.Files[fileID] = f
	s.db.markDirty()
	s.reconcileLocked() // replikatsiya vazifalari darhol yaratiladi
	tasks := 0
	for _, t := range s.db.Tasks {
		if t.FileID == fileID && t.Status == TaskPending {
			tasks++
		}
	}
	resp := map[string]any{
		"ok":            true,
		"fileId":        fileID,
		"name":          name,
		"size":          total,
		"sha256":        f.SHA256,
		"chunks":        len(chunks),
		"replicaTarget": f.ReplicaTarget,
		"tasksCreated":  tasks,
		"status":        f.Status,
	}
	s.db.mu.Unlock()
	s.persist()
	s.sim.wakeAll()
	log.Printf("fayl yuklandi: %s (%s, %d bayt, %d chunk, %d nusxa vazifasi)", fileID, name, total, len(chunks), tasks)
	writeJSON(w, http.StatusOK, resp)
}

// ---------------- Admin: fayl yuklab olish ----------------

func (s *Server) handleDownload(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	s.db.mu.Lock()
	f, ok := s.db.Files[id]
	var cp File
	if ok {
		cp = *f
		cp.Chunks = append([]Chunk(nil), f.Chunks...)
		cp.Missing = append([]int(nil), f.Missing...)
	}
	s.db.mu.Unlock()
	if !ok {
		writeErr(w, http.StatusNotFound, "fayl topilmadi")
		return
	}
	restoring := func() {
		s.triggerReconcile()
		writeJSON(w, http.StatusServiceUnavailable, map[string]any{
			"ok": false, "restoring": true,
			"error": "fayl serverda to'liq emas — tugunlardagi nusxadan tiklanmoqda, birozdan so'ng qayta urinib ko'ring",
		})
	}
	if len(cp.Missing) > 0 {
		restoring()
		return
	}
	for _, c := range cp.Chunks {
		fi, err := os.Stat(s.chunkPath(cp.ID, c.Index))
		if err != nil || fi.Size() != c.Size {
			s.markMissing(cp.ID, c.Index)
			restoring()
			return
		}
	}

	w.Header().Set("Content-Disposition", mime.FormatMediaType("attachment", map[string]string{"filename": cp.Name}))
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Length", strconv.FormatInt(cp.Size, 10))
	w.Header().Set("X-Content-SHA256", cp.SHA256)
	if r.Method == http.MethodHead {
		return
	}
	for _, c := range cp.Chunks {
		b, err := os.ReadFile(s.chunkPath(cp.ID, c.Index))
		if err != nil || sha256Hex(b) != c.SHA256 {
			// yuborish boshlangan — ulanishni uzamiz, chunk tiklashga qo'yiladi
			s.markMissing(cp.ID, c.Index)
			s.triggerReconcile()
			panic(http.ErrAbortHandler)
		}
		if _, err := w.Write(b); err != nil {
			return
		}
	}
}

// ---------------- Admin: fayl o'chirish ----------------

func (s *Server) handleDeleteFile(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	s.db.mu.Lock()
	f, ok := s.db.Files[id]
	if !ok {
		s.db.mu.Unlock()
		writeErr(w, http.StatusNotFound, "fayl topilmadi")
		return
	}
	ts := s.now()
	targets := map[string]bool{}
	for _, t := range s.db.Tasks {
		if t.FileID == id && t.Status == TaskPending {
			if t.Kind == KindReplica || t.Kind == KindRepair {
				targets[t.NodeID] = true // yuklab olayotgan bo'lishi mumkin
			}
			if t.Kind != KindDelete {
				t.Status = TaskCancelled
				t.DoneAt = ts
			}
		}
	}
	for _, n := range s.db.Nodes {
		if contains(n.StoredFiles, id) {
			n.StoredFiles = removeStr(n.StoredFiles, id)
			targets[n.ID] = true
		}
	}
	deletes := 0
	for nid := range targets {
		if n := s.db.Nodes[nid]; n != nil && n.Status == NodeActive && !s.db.hasPendingTask(nid, id, KindDelete) {
			s.db.newTask(nid, f, KindDelete, nil, ts)
			deletes++
		}
	}
	delete(s.db.Files, id)
	s.db.markDirty()
	s.db.mu.Unlock()
	os.RemoveAll(filepath.Join(s.chunksDir, id))
	s.persist()
	s.sim.wakeAll()
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "deleteTasks": deletes})
}

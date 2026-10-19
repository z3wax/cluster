package main

import (
	"context"
	"encoding/hex"
	"log"
	"math"
	mrand "math/rand/v2"
	"net/http"
	"os"
	"sort"
	"time"
)

// ================= Replikatsiya menejeri =================
//
// Har siklda (va har bir muhim hodisadan keyin) klaster holati kerakli holatga keltiriladi:
//   - heartbeat yubormayotgan tugunlar oflayn deb belgilanadi (internet/elektr uzilishi);
//   - oflayn tugunlardagi vazifalar boshqa tugunlarga beriladi;
//   - har bir fayl faol tugunlarning 30% iga nusxalanadi (shartnoma 2.3);
//   - oflayn qolgan tugundagi nusxa RepairDelay dan keyin boshqa tugunga ko'chiriladi;
//   - ortiqcha nusxalar olib tashlanadi (tugun qaytib kelganda);
//   - serverda yo'qolgan/buzilgan chunk'lar tugunlardagi nusxadan tiklanadi.

func (s *Server) reconcileLoop(ctx context.Context) {
	t := time.NewTicker(s.cfg.ReconcileInterval)
	defer t.Stop()
	s.reconcile()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			s.reconcile()
		case <-s.kick:
			s.reconcile()
		}
	}
}

func (s *Server) reconcile() {
	s.db.mu.Lock()
	created := s.reconcileLocked()
	s.db.mu.Unlock()
	if created > 0 {
		s.sim.wakeAll()
	}
}

// desiredFor — n ta faol tugundan nechtasiga nusxa kerak (ceil(30%), kamida 1)
func desiredFor(n int, ratio float64) int {
	if n <= 0 {
		return 0
	}
	d := int(math.Ceil(float64(n)*ratio - 1e-9))
	if d < 1 {
		d = 1
	}
	if d > n {
		d = n
	}
	return d
}

// eligibleLocked — 30% hisobiga kiradigan tugunlar: faol, shartnomani qabul qilgan, yaqinda ko'ringan
func (s *Server) eligibleLocked(ts int64) []*Node {
	win := int64(s.cfg.ActiveWindow.Seconds())
	var out []*Node
	for _, n := range s.db.Nodes {
		if n.Status == NodeActive && n.AgreementAccepted && ts-n.LastSeen <= win {
			out = append(out, n)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out
}

func (s *Server) desiredReplicasLocked() int {
	return desiredFor(len(s.eligibleLocked(s.now())), s.cfg.ReplicaRatio)
}

func finishTask(t *Task, status, reason string, ts int64) {
	t.Status = status
	t.Error = reason
	t.DoneAt = ts
}

// reconcileLocked — db.mu ostida chaqiriladi; yaratilgan vazifalar sonini qaytaradi.
func (s *Server) reconcileLocked() int {
	d := s.db
	ts := s.now()
	offline := int64(s.cfg.OfflineTimeout.Seconds())
	repairDelay := int64(s.cfg.RepairDelay.Seconds())
	created := 0

	// 1. Uzilgan tugunlar — onlayn vaqt to'xtatiladi (shartnoma 5.1, 5.2)
	for _, n := range d.Nodes {
		if n.Online && ts-n.LastSeen > offline {
			n.Online = false
			n.UserActive = false
			d.markDirty()
		}
	}

	// 2. Javobsiz qolgan auditlar
	auditTimeout := int64((3*s.cfg.HeartbeatInterval + 30*time.Second).Seconds())
	for id, a := range s.audits {
		if ts-a.IssuedAt <= auditTimeout {
			continue
		}
		delete(s.audits, id)
		if n := d.Nodes[a.NodeID]; n != nil && n.Online && n.Status == NodeActive {
			s.auditFailedLocked(a.NodeID, a.FileID, "audit javobsiz qoldi")
		}
	}

	// 3. Bajarilmay qolgan vazifalar
	taskTimeout := int64(s.cfg.TaskTimeout.Seconds())
	for _, t := range d.Tasks {
		if t.Status != TaskPending {
			continue
		}
		n := d.Nodes[t.NodeID]
		if n == nil || n.Status != NodeActive {
			finishTask(t, TaskCancelled, "tugun o'chirilgan", ts)
			d.markDirty()
			continue
		}
		if t.Kind == KindDelete {
			continue // tugun qaytganda bajaradi
		}
		if _, ok := d.Files[t.FileID]; !ok {
			finishTask(t, TaskCancelled, "fayl o'chirilgan", ts)
			d.markDirty()
			continue
		}
		if !n.Online && ts-n.LastSeen > repairDelay {
			finishTask(t, TaskFailed, "tugun oflayn — boshqa tugunga berildi", ts)
			d.markDirty()
		} else if ts-t.AssignedAt > taskTimeout {
			finishTask(t, TaskFailed, "vaqt tugadi", ts)
			d.markDirty()
		}
	}

	// 4. Indekslar
	eligible := s.eligibleLocked(ts)
	desired := desiredFor(len(eligible), s.cfg.ReplicaRatio)
	used := map[string]int64{} // tugun: saqlangan + kutilayotgan bayt
	for _, n := range d.Nodes {
		if n.Status == NodeActive {
			used[n.ID] = d.nodeStoredBytes(n)
		}
	}
	type fileTasks struct {
		copies  map[string]bool
		deletes map[string]bool
		restore bool
	}
	ft := map[string]*fileTasks{}
	get := func(fid string) *fileTasks {
		x := ft[fid]
		if x == nil {
			x = &fileTasks{copies: map[string]bool{}, deletes: map[string]bool{}}
			ft[fid] = x
		}
		return x
	}
	recentFail := map[string]bool{}
	for _, t := range d.Tasks {
		switch t.Status {
		case TaskPending:
			x := get(t.FileID)
			switch t.Kind {
			case KindReplica, KindRepair:
				x.copies[t.NodeID] = true
				if f := d.Files[t.FileID]; f != nil {
					used[t.NodeID] += f.Size
				}
			case KindRestore:
				x.restore = true
			case KindDelete:
				x.deletes[t.NodeID] = true
			}
		case TaskFailed:
			if ts-t.DoneAt < 3600 {
				recentFail[t.NodeID+"|"+t.FileID] = true
			}
		}
	}
	holdersOf := map[string][]*Node{}
	for _, n := range d.Nodes {
		if n.Status != NodeActive {
			continue
		}
		for _, fid := range n.StoredFiles {
			holdersOf[fid] = append(holdersOf[fid], n)
		}
	}
	free := func(n *Node) int64 { return n.SpaceTotal - used[n.ID] }

	files := make([]*File, 0, len(d.Files))
	for _, f := range d.Files {
		files = append(files, f)
	}
	sort.Slice(files, func(i, j int) bool {
		if files[i].UploadedAt != files[j].UploadedAt {
			return files[i].UploadedAt < files[j].UploadedAt
		}
		return files[i].ID < files[j].ID
	})

	for _, f := range files {
		x := get(f.ID)
		var avail, onlineH []*Node
		for _, h := range holdersOf[f.ID] {
			if h.Online {
				onlineH = append(onlineH, h)
				avail = append(avail, h)
			} else if ts-h.LastSeen <= repairDelay {
				avail = append(avail, h) // qisqa uzilish — nusxa hali hisobga olinadi
			}
		}
		serverOK := len(f.Missing) == 0

		// 4a. Yetishmayotgan nusxalar
		newTasks := 0
		if need := desired - len(avail) - len(x.copies); need > 0 && serverOK {
			var cands []*Node
			for _, n := range eligible {
				if !n.Online || contains(n.StoredFiles, f.ID) || x.copies[n.ID] || x.deletes[n.ID] || recentFail[n.ID+"|"+f.ID] {
					continue
				}
				if free(n) < f.Size {
					continue
				}
				cands = append(cands, n)
			}
			sort.Slice(cands, func(i, j int) bool {
				if free(cands[i]) != free(cands[j]) {
					return free(cands[i]) > free(cands[j])
				}
				return cands[i].ID < cands[j].ID
			})
			for i := 0; i < need && i < len(cands); i++ {
				n := cands[i]
				d.newTask(n.ID, f, KindReplica, f.Chunks, ts)
				x.copies[n.ID] = true
				used[n.ID] += f.Size
				newTasks++
				created++
			}
		}

		// 4b. Ortiqcha nusxalar (masalan, oflayn tugun qaytib keldi)
		if serverOK && desired > 0 && len(x.copies) == 0 && newTasks == 0 && len(avail) > desired {
			excess := len(avail) - desired
			victims := append([]*Node(nil), onlineH...)
			sort.Slice(victims, func(i, j int) bool {
				if free(victims[i]) != free(victims[j]) {
					return free(victims[i]) < free(victims[j])
				}
				return victims[i].ID < victims[j].ID
			})
			trimmed := map[string]bool{}
			for i := 0; i < excess && i < len(victims); i++ {
				v := victims[i]
				v.StoredFiles = removeStr(v.StoredFiles, f.ID)
				used[v.ID] -= f.Size
				d.newTask(v.ID, f, KindDelete, nil, ts)
				x.deletes[v.ID] = true
				trimmed[v.ID] = true
				created++
			}
			if len(trimmed) > 0 {
				keep := func(list []*Node) []*Node {
					var out []*Node
					for _, n := range list {
						if !trimmed[n.ID] {
							out = append(out, n)
						}
					}
					return out
				}
				avail, onlineH = keep(avail), keep(onlineH)
			}
		}

		// 4c. Server nusxasini tugundan tiklash
		if !serverOK && !x.restore {
			var src *Node
			for _, h := range onlineH {
				if recentFail[h.ID+"|"+f.ID] {
					continue
				}
				if src == nil || h.AuditsPassed-h.AuditsFailed > src.AuditsPassed-src.AuditsFailed {
					src = h
				}
			}
			if src != nil {
				var chunks []Chunk
				for _, c := range f.Chunks {
					if containsInt(f.Missing, c.Index) {
						chunks = append(chunks, c)
					}
				}
				d.newTask(src.ID, f, KindRestore, chunks, ts)
				x.restore = true
				created++
			}
		}

		// 4d. Fayl holati
		status := FileDegraded
		switch {
		case !serverOK && len(onlineH) == 0:
			status = FileUnavailable
		case !serverOK:
			status = FileRestoring
		case desired > 0 && len(avail) >= desired:
			status = FileHealthy
		case len(x.copies) > 0:
			status = FileReplicating
		}
		if f.Status != status || f.ReplicaTarget != desired || f.ReplicasDone != len(avail) {
			f.Status = status
			f.ReplicaTarget = desired
			f.ReplicasDone = len(avail)
			d.markDirty()
		}
	}

	// 5. Eski (tugagan) vazifalarni tozalash
	const keepSec = 7 * 24 * 3600
	const maxFinished = 20000
	var finished []*Task
	for id, t := range d.Tasks {
		if t.Status == TaskPending {
			continue
		}
		if ts-t.DoneAt > keepSec {
			delete(d.Tasks, id)
			d.markDirty()
			continue
		}
		finished = append(finished, t)
	}
	if len(finished) > maxFinished {
		sort.Slice(finished, func(i, j int) bool { return finished[i].DoneAt < finished[j].DoneAt })
		for _, t := range finished[:len(finished)-maxFinished] {
			delete(d.Tasks, t.ID)
		}
		d.markDirty()
	}
	return created
}

// ================= Saqlash isboti (audit) =================
//
// Server tugunga tasodifiy chunk va nonce yuboradi; tugun sha256(nonce || chunk) ni qaytaradi.
// Javob faqat chunk haqiqatan saqlangan bo'lsa to'g'ri chiqadi. Audit o'tmasa fayl
// tugundan "yuklanmagan" deb hisoblanadi — to'lov to'xtaydi (5.3) va qayta yuklash vazifasi beriladi (3.2).

type audit struct {
	ID       string
	NodeID   string
	FileID   string
	Index    int
	Nonce    string
	IssuedAt int64
}

type auditView struct {
	ID     string `json:"id"`
	FileID string `json:"fileId"`
	Index  int    `json:"index"`
	Nonce  string `json:"nonce"`
}

func (s *Server) issueAuditsLocked(n *Node, ts int64) []auditView {
	out := []auditView{}
	for _, a := range s.audits {
		if a.NodeID == n.ID {
			out = append(out, auditView{a.ID, a.FileID, a.Index, a.Nonce})
		}
	}
	if len(out) > 0 || len(n.StoredFiles) == 0 {
		return out
	}
	fid := ""
	prio := s.priorityAudit[n.ID]
	for len(prio) > 0 && fid == "" {
		if contains(n.StoredFiles, prio[0]) {
			fid = prio[0]
		}
		prio = prio[1:]
	}
	if len(prio) == 0 {
		delete(s.priorityAudit, n.ID)
	} else {
		s.priorityAudit[n.ID] = prio
	}
	if fid == "" {
		if ts-s.lastAudit[n.ID] < int64(s.cfg.AuditInterval.Seconds()) {
			return out
		}
		fid = n.StoredFiles[mrand.IntN(len(n.StoredFiles))]
	}
	f := s.db.Files[fid]
	if f == nil {
		return out
	}
	var cands []Chunk
	for _, c := range f.Chunks {
		if !containsInt(f.Missing, c.Index) {
			cands = append(cands, c)
		}
	}
	if len(cands) == 0 {
		return out
	}
	c := cands[mrand.IntN(len(cands))]
	a := &audit{ID: randomID("a"), NodeID: n.ID, FileID: fid, Index: c.Index, Nonce: randomHex(16), IssuedAt: ts}
	s.audits[a.ID] = a
	s.lastAudit[n.ID] = ts
	return append(out, auditView{a.ID, a.FileID, a.Index, a.Nonce})
}

// auditProof — sha256(nonce || chunk) (hex)
func auditProof(nonceHex string, chunk []byte) string {
	nonce, _ := hex.DecodeString(nonceHex)
	h := newSHA256()
	h.Write(nonce)
	h.Write(chunk)
	return h.hex()
}

func (s *Server) handleAudits(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Results []struct {
			ID    string `json:"id"`
			Proof string `json:"proof"`
			Error string `json:"error"`
		} `json:"results"`
	}
	if !readJSON(w, r, &req) {
		return
	}
	type job struct {
		a     *audit
		c     Chunk
		proof string
		err   string
	}
	s.db.mu.Lock()
	n := s.authNodeLocked(w, r)
	if n == nil {
		s.db.mu.Unlock()
		return
	}
	nodeID := n.ID
	var jobs []job
	for _, res := range req.Results {
		a := s.audits[res.ID]
		if a == nil || a.NodeID != nodeID {
			continue
		}
		delete(s.audits, res.ID)
		f := s.db.Files[a.FileID]
		if f == nil {
			continue
		}
		c, ok := findChunk(f, a.Index)
		if !ok {
			continue
		}
		jobs = append(jobs, job{a: a, c: c, proof: res.Proof, err: res.Error})
	}
	s.db.mu.Unlock()

	// tekshiruv lock'dan tashqarida (diskdan o'qish)
	type verdict struct {
		a      *audit
		pass   bool
		reason string
	}
	var verdicts []verdict
	for _, j := range jobs {
		b, err := os.ReadFile(s.chunkPath(j.a.FileID, j.a.Index))
		if err != nil || sha256Hex(b) != j.c.SHA256 {
			s.markMissing(j.a.FileID, j.a.Index) // server nusxasi buzuq — tugun aybdor emas
			continue
		}
		if j.err != "" {
			verdicts = append(verdicts, verdict{j.a, false, "tugun: " + clip(j.err, 120)})
			continue
		}
		if j.proof == auditProof(j.a.Nonce, b) {
			verdicts = append(verdicts, verdict{j.a, true, ""})
		} else {
			verdicts = append(verdicts, verdict{j.a, false, "isbot noto'g'ri"})
		}
	}

	passed, failed := 0, 0
	s.db.mu.Lock()
	for _, v := range verdicts {
		if v.pass {
			if n := s.db.Nodes[nodeID]; n != nil {
				n.AuditsPassed++
			}
			passed++
		} else {
			s.auditFailedLocked(nodeID, v.a.FileID, v.reason)
			failed++
		}
	}
	s.db.markDirty()
	s.db.mu.Unlock()
	if failed > 0 {
		s.triggerReconcile()
		s.sim.wakeAll()
	}
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "passed": passed, "failed": failed})
}

// auditFailedLocked — fayl tugunda tasdiqlanmadi: to'lov to'xtaydi va qayta yuklash vazifasi beriladi
func (s *Server) auditFailedLocked(nodeID, fileID, reason string) {
	d := s.db
	n := d.Nodes[nodeID]
	if n == nil {
		return
	}
	n.AuditsFailed++
	n.StoredFiles = removeStr(n.StoredFiles, fileID)
	if f := d.Files[fileID]; f != nil && n.Status == NodeActive && len(f.Missing) == 0 &&
		!d.hasPendingTask(nodeID, fileID, KindRepair, KindReplica) {
		d.newTask(nodeID, f, KindRepair, f.Chunks, s.now())
	}
	d.markDirty()
	log.Printf("audit o'tmadi: tugun=%s fayl=%s (%s)", nodeID, fileID, reason)
}

// ================= Server chunk'larini tekshirish (scrub) =================

func (s *Server) markMissing(fileID string, idx int) {
	s.db.mu.Lock()
	if f, ok := s.db.Files[fileID]; ok && !containsInt(f.Missing, idx) {
		f.Missing = appendMissing(f.Missing, idx)
		s.db.markDirty()
		log.Printf("server chunk yo'q/buzilgan: %s/%d — tugundan tiklanadi", fileID, idx)
	}
	s.db.mu.Unlock()
	s.triggerReconcile()
}

func (s *Server) scrubLoop(ctx context.Context) {
	quick := time.NewTicker(time.Minute)
	full := time.NewTicker(s.cfg.ScrubInterval)
	defer quick.Stop()
	defer full.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-quick.C:
			s.scrubAll(ctx, false)
		case <-full.C:
			s.scrubAll(ctx, true)
		}
	}
}

func (s *Server) scrubAll(ctx context.Context, full bool) {
	s.db.mu.Lock()
	ids := make([]string, 0, len(s.db.Files))
	for id := range s.db.Files {
		ids = append(ids, id)
	}
	s.db.mu.Unlock()
	for _, id := range ids {
		if ctx.Err() != nil {
			return
		}
		s.scrubFile(id, full)
	}
}

// scrubFile — server chunk'larini tekshiradi (full: SHA-256, aks holda hajm).
// Qo'lda qaytarilgan chunk'lar ham aniqlanib, "missing" ro'yxatidan chiqariladi.
func (s *Server) scrubFile(fileID string, full bool) {
	s.db.mu.Lock()
	f, ok := s.db.Files[fileID]
	if !ok {
		s.db.mu.Unlock()
		return
	}
	chunks := append([]Chunk(nil), f.Chunks...)
	missing := append([]int(nil), f.Missing...)
	s.db.mu.Unlock()

	var bad, recovered []int
	for _, c := range chunks {
		p := s.chunkPath(fileID, c.Index)
		wasMissing := containsInt(missing, c.Index)
		if full || wasMissing {
			sum, n, err := hashFile(p)
			good := err == nil && n == c.Size && sum == c.SHA256
			if !good && !wasMissing {
				bad = append(bad, c.Index)
			} else if good && wasMissing {
				recovered = append(recovered, c.Index)
			}
			continue
		}
		if fi, err := os.Stat(p); err != nil || fi.Size() != c.Size {
			bad = append(bad, c.Index)
		}
	}
	if len(bad) == 0 && len(recovered) == 0 {
		return
	}
	s.db.mu.Lock()
	if f, ok := s.db.Files[fileID]; ok {
		for _, b := range bad {
			f.Missing = appendMissing(f.Missing, b)
		}
		var rest []int
		for _, m := range f.Missing {
			if !containsInt(recovered, m) {
				rest = append(rest, m)
			}
		}
		f.Missing = rest
		s.db.markDirty()
	}
	s.db.mu.Unlock()
	if len(bad) > 0 {
		log.Printf("scrub: %s faylida %d ta chunk yo'q/buzilgan — tiklash boshlandi", fileID, len(bad))
	}
	s.triggerReconcile()
}

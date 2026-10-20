package main

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"sort"
	"strings"
	"sync"
	"time"
)

// ================= Hisoblash (GPU/CPU) koordinatori =================
//
// Bosh PC (server) OpenAI-mos API beradi; so'rovlarni klasterdagi hisoblash tugunlariga
// ularning GPU/CPU/RAM imkoniyatiga qarab taqsimlaydi. Tugunlar FAQAT o'zidagi inference
// dvigatelini (ollama / llama.cpp / test "echo") ishga tushiradi — koordinator ularda
// ixtiyoriy buyruq bajara olmaydi, faqat {model, prompt, parametr} turidagi ishlar yuboriladi.
//
// Taqsimlash "pull" modelida: bo'sh tugun ish so'raydi (uzoq-poll) va mos ishni oladi.
// Band tugunlar so'ramaydi, shuning uchun yuk tabiiy ravishda teng taqsimlanadi.

type ChatMessage struct {
	Role    string `json:"role"`
	Content string `json:"content"`
}

const (
	JobQueued   = "queued"
	JobAssigned = "assigned"
	JobRunning  = "running"
	JobDone     = "done"
	JobFailed   = "failed"
	JobCanceled = "canceled"

	KindChat      = "chat"
	KindGenerate  = "generate"
	KindEmbedding = "embedding"
)

type ComputeJob struct {
	ID       string         `json:"id"`
	Model    string         `json:"model"`
	Kind     string         `json:"kind"`
	Messages []ChatMessage  `json:"messages,omitempty"`
	Prompt   string         `json:"prompt,omitempty"`
	Params   map[string]any `json:"params,omitempty"`

	Status    string `json:"status"`
	Worker    string `json:"worker,omitempty"`
	CreatedAt int64  `json:"createdAt"`
	StartedAt int64  `json:"startedAt,omitempty"`
	DoneAt    int64  `json:"doneAt,omitempty"`

	// oqim holati (mu ostida)
	mu           sync.Mutex
	cond         *sync.Cond
	tokens       []string
	result       string
	embedding    []float64
	finishReason string
	errMsg       string
	promptTokens int
	outTokens    int
	terminal     bool // done | failed | canceled
}

func (j *ComputeJob) snapshotStatus() string {
	j.mu.Lock()
	defer j.mu.Unlock()
	return j.Status
}

// workerView — poll so'rovidagi yangi imkoniyatlar (yoki saqlangan holat)
type workerView struct {
	nodeID    string
	models    map[string]bool
	allowPull bool
	engines   map[string]bool
}

func (w workerView) canRun(model string) bool {
	if len(w.engines) == 0 {
		return false
	}
	if w.models[model] {
		return true
	}
	return w.allowPull // ollama/llama.cpp birinchi ishlatishda modelni yuklab oladi
}

// sanitizeCompute — mijoz xabar qilgan imkoniyatlarni tozalaydi va cheklaydi
func sanitizeCompute(c *ComputeInfo) *ComputeInfo {
	if c == nil {
		return nil
	}
	clipList := func(in []string, max, n int) []string {
		out := []string{}
		seen := map[string]bool{}
		for _, v := range in {
			v = clip(v, max)
			if v != "" && !seen[v] && len(out) < n {
				seen[v] = true
				out = append(out, v)
			}
		}
		return out
	}
	known := map[string]bool{"ollama": true, "llama.cpp": true, "echo": true}
	engines := []string{}
	for _, e := range clipList(c.Engines, 24, 8) {
		if known[e] {
			engines = append(engines, e)
		}
	}
	out := &ComputeInfo{
		Enabled:      c.Enabled,
		Engines:      engines,
		Models:       clipList(c.Models, 128, 128),
		AllowPull:    c.AllowPull,
		RAMBytes:     c.RAMBytes,
		FreeRAMBytes: c.FreeRAMBytes,
		CPUCores:     c.CPUCores,
		GPU:          GPUInfo{Present: c.GPU.Present, Name: clip(c.GPU.Name, 64), VRAMBytes: c.GPU.VRAMBytes, Backend: clip(c.GPU.Backend, 16)},
		MaxParallel:  c.MaxParallel,
		RPCPort:      c.RPCPort,
	}
	if out.MaxParallel < 1 {
		out.MaxParallel = 1
	}
	if out.MaxParallel > 64 {
		out.MaxParallel = 64
	}
	if len(out.Engines) == 0 {
		out.Enabled = false
	}
	return out
}

type computeManager struct {
	s   *Server
	mu  sync.Mutex
	gen chan struct{} // har o'zgarishda yopiladi (pollerlarni uyg'otish)

	jobs    map[string]*ComputeJob
	queue   []string       // navbatdagi job ID lar (FIFO)
	running map[string]int // nodeID -> faol ishlar soni (statistika)
	byNode  map[string][]string
}

func newComputeManager(s *Server) *computeManager {
	return &computeManager{
		s:       s,
		gen:     make(chan struct{}),
		jobs:    map[string]*ComputeJob{},
		running: map[string]int{},
		byNode:  map[string][]string{},
	}
}

func (cm *computeManager) broadcastLocked() {
	close(cm.gen)
	cm.gen = make(chan struct{})
}

func (cm *computeManager) submit(j *ComputeJob) {
	j.cond = sync.NewCond(&j.mu)
	j.Status = JobQueued
	j.CreatedAt = cm.s.now()
	cm.mu.Lock()
	cm.jobs[j.ID] = j
	cm.queue = append(cm.queue, j.ID)
	cm.broadcastLocked()
	cm.mu.Unlock()
}

func (cm *computeManager) get(id string) *ComputeJob {
	cm.mu.Lock()
	defer cm.mu.Unlock()
	return cm.jobs[id]
}

// grabLocked — worker uchun navbatdagi birinchi mos ishni oladi (cm.mu ostida)
func (cm *computeManager) grabLocked(w workerView) *ComputeJob {
	for i, id := range cm.queue {
		j := cm.jobs[id]
		if j == nil || j.snapshotStatus() != JobQueued {
			continue
		}
		if !w.canRun(j.Model) {
			continue
		}
		cm.queue = append(cm.queue[:i], cm.queue[i+1:]...)
		j.mu.Lock()
		j.Status = JobAssigned
		j.Worker = w.nodeID
		j.mu.Unlock()
		cm.running[w.nodeID]++
		cm.byNode[w.nodeID] = append(cm.byNode[w.nodeID], id)
		return j
	}
	return nil
}

func (cm *computeManager) finishWorkerJob(nodeID, jobID string) {
	cm.mu.Lock()
	if cm.running[nodeID] > 0 {
		cm.running[nodeID]--
	}
	lst := cm.byNode[nodeID][:0]
	for _, id := range cm.byNode[nodeID] {
		if id != jobID {
			lst = append(lst, id)
		}
	}
	cm.byNode[nodeID] = lst
	cm.mu.Unlock()
}

// poll — bo'sh tugun mos ish kutadi (uzoq-poll). ctx tugasa yoki timeout bo'lsa nil.
func (cm *computeManager) poll(ctx context.Context, w workerView, timeout time.Duration) *ComputeJob {
	deadline := time.NewTimer(timeout)
	defer deadline.Stop()
	for {
		cm.mu.Lock()
		gen := cm.gen
		j := cm.grabLocked(w)
		cm.mu.Unlock()
		if j != nil {
			return j
		}
		select {
		case <-ctx.Done():
			return nil
		case <-gen:
		case <-deadline.C:
			return nil
		}
	}
}

// ---------------- Oqim (streaming) ----------------

func (j *ComputeJob) appendToken(tok string) bool {
	j.mu.Lock()
	defer j.mu.Unlock()
	if j.terminal {
		return false // bekor qilingan — worker to'xtashi kerak
	}
	if j.Status == JobAssigned {
		j.Status = JobRunning
	}
	j.tokens = append(j.tokens, tok)
	j.outTokens++
	j.cond.Broadcast()
	return true
}

func (j *ComputeJob) finish(status, result, finishReason, errMsg string, promptTokens int, embedding []float64, now int64) {
	j.mu.Lock()
	defer j.mu.Unlock()
	if j.terminal {
		return
	}
	j.Status = status
	j.result = result
	j.embedding = embedding
	if finishReason != "" {
		j.finishReason = finishReason
	}
	j.errMsg = errMsg
	if promptTokens > 0 {
		j.promptTokens = promptTokens
	}
	j.DoneAt = now
	j.terminal = true
	j.cond.Broadcast()
}

func (j *ComputeJob) cancel(now int64) {
	j.finish(JobCanceled, "", "canceled", "requester uzildi", 0, nil, now)
}

// waitDone — terminal holatgacha kutadi (ctx bekor bo'lsa tugaydi)
func (j *ComputeJob) waitDone(ctx context.Context) {
	done := make(chan struct{})
	go func() {
		j.mu.Lock()
		for !j.terminal {
			j.cond.Wait()
		}
		j.mu.Unlock()
		close(done)
	}()
	select {
	case <-done:
	case <-ctx.Done():
		j.cancel(timeNowUnix())
	}
}

func timeNowUnix() int64 { return time.Now().Unix() }

// stream — cursordan boshlab yangi tokenlarni va terminal holatni yetkazadi.
// emit(token) har token uchun, done(job) oxirida chaqiriladi. ctx uzilsa to'xtaydi.
func (j *ComputeJob) stream(ctx context.Context, emit func(string) bool, done func()) {
	stop := make(chan struct{})
	go func() {
		select {
		case <-ctx.Done():
			j.mu.Lock()
			j.cond.Broadcast()
			j.mu.Unlock()
		case <-stop:
		}
	}()
	defer close(stop)

	cursor := 0
	j.mu.Lock()
	for {
		for cursor < len(j.tokens) {
			tok := j.tokens[cursor]
			cursor++
			j.mu.Unlock()
			if ctx.Err() != nil || !emit(tok) {
				j.mu.Lock()
				goto end
			}
			j.mu.Lock()
		}
		if j.terminal || ctx.Err() != nil {
			break
		}
		j.cond.Wait()
	}
end:
	j.mu.Unlock()
	done()
}

// ---------------- Eligible workerlar va modellar ----------------

type computeWorkerInfo struct {
	ID          string   `json:"id"`
	Name        string   `json:"name"`
	OS          string   `json:"os"`
	Online      bool     `json:"online"`
	Engines     []string `json:"engines"`
	Models      []string `json:"models"`
	AllowPull   bool     `json:"allowPull"`
	GPU         GPUInfo  `json:"gpu"`
	RAMBytes    int64    `json:"ramBytes"`
	CPUCores    int      `json:"cpuCores"`
	MaxParallel int      `json:"maxParallel"`
	Running     int      `json:"running"`
}

func (cm *computeManager) workers(onlineOnly bool) []computeWorkerInfo {
	cm.s.db.mu.Lock()
	type row struct {
		n *Node
	}
	var rows []row
	for _, n := range cm.s.db.Nodes {
		if n.Status == NodeActive && n.Compute != nil && n.Compute.Enabled {
			if onlineOnly && !n.Online {
				continue
			}
			rows = append(rows, row{n})
		}
	}
	cm.s.db.mu.Unlock()

	cm.mu.Lock()
	run := map[string]int{}
	for k, v := range cm.running {
		run[k] = v
	}
	cm.mu.Unlock()

	out := make([]computeWorkerInfo, 0, len(rows))
	for _, r := range rows {
		n := r.n
		c := n.Compute
		out = append(out, computeWorkerInfo{
			ID: n.ID, Name: n.Name, OS: n.OS, Online: n.Online,
			Engines: c.Engines, Models: c.Models, AllowPull: c.AllowPull,
			GPU: c.GPU, RAMBytes: c.RAMBytes, CPUCores: c.CPUCores,
			MaxParallel: c.MaxParallel, Running: run[n.ID],
		})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out
}

func (cm *computeManager) models() []string {
	set := map[string]bool{}
	for _, w := range cm.workers(true) {
		for _, m := range w.Models {
			set[m] = true
		}
	}
	out := make([]string, 0, len(set))
	for m := range set {
		out = append(out, m)
	}
	sort.Strings(out)
	return out
}

// modelReachable — biror onlayn tugun shu modelni ishga tushira oladimi
func (cm *computeManager) modelReachable(model string) bool {
	for _, w := range cm.workers(true) {
		wv := workerView{models: map[string]bool{}, engines: map[string]bool{}, allowPull: w.AllowPull}
		for _, m := range w.Models {
			wv.models[m] = true
		}
		for _, e := range w.Engines {
			wv.engines[e] = true
		}
		if wv.canRun(model) {
			return true
		}
	}
	return false
}

// ---------------- Reaper: javobsiz ishlarni tugatish ----------------

func (cm *computeManager) reap(ctx context.Context) {
	t := time.NewTicker(2 * time.Second)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			now := cm.s.now()
			queueTimeout := int64(cm.s.cfg.ComputeQueueTimeout.Seconds())
			jobTimeout := int64(cm.s.cfg.ComputeJobTimeout.Seconds())
			cm.mu.Lock()
			var fails []*ComputeJob
			for _, id := range cm.queue {
				j := cm.jobs[id]
				if j != nil && now-j.CreatedAt > queueTimeout {
					fails = append(fails, j)
				}
			}
			// navbatdagi muddati o'tganlarni navbatdan olib tashlash
			if len(fails) > 0 {
				keep := cm.queue[:0]
				fmap := map[string]bool{}
				for _, j := range fails {
					fmap[j.ID] = true
				}
				for _, id := range cm.queue {
					if !fmap[id] {
						keep = append(keep, id)
					}
				}
				cm.queue = keep
			}
			var running []*ComputeJob
			for _, j := range cm.jobs {
				if (j.snapshotStatus() == JobRunning || j.snapshotStatus() == JobAssigned) && j.CreatedAt > 0 &&
					now-j.CreatedAt > jobTimeout {
					running = append(running, j)
				}
			}
			cm.mu.Unlock()
			for _, j := range fails {
				j.finish(JobFailed, "", "error", "hech bir hisoblash tuguni bu modelni ishga tushira olmadi: "+j.Model, 0, nil, now)
			}
			for _, j := range running {
				j.finish(JobFailed, "", "error", "ish vaqti tugadi", 0, nil, now)
				cm.finishWorkerJob(j.Worker, j.ID)
			}
			// eski tugagan ishlarni tozalash
			cm.mu.Lock()
			for id, j := range cm.jobs {
				if j.snapshotStatus() == JobDone || j.snapshotStatus() == JobFailed || j.snapshotStatus() == JobCanceled {
					if now-j.DoneAt > 600 {
						delete(cm.jobs, id)
					}
				}
			}
			cm.mu.Unlock()
		}
	}
}

// ======================= HTTP: worker (tugun) =======================

func workerViewFrom(n *Node, body *struct {
	Models    []string `json:"models"`
	Engines   []string `json:"engines"`
	AllowPull *bool    `json:"allowPull"`
}) workerView {
	wv := workerView{nodeID: n.ID, models: map[string]bool{}, engines: map[string]bool{}}
	if n.Compute != nil {
		wv.allowPull = n.Compute.AllowPull
		for _, m := range n.Compute.Models {
			wv.models[m] = true
		}
		for _, e := range n.Compute.Engines {
			wv.engines[e] = true
		}
	}
	if body != nil {
		if body.AllowPull != nil {
			wv.allowPull = *body.AllowPull
		}
		if body.Models != nil {
			wv.models = map[string]bool{}
			for _, m := range body.Models {
				wv.models[m] = true
			}
		}
		if body.Engines != nil {
			wv.engines = map[string]bool{}
			for _, e := range body.Engines {
				wv.engines[e] = true
			}
		}
	}
	return wv
}

// handleComputePoll — tugun bo'sh: mos ish kutadi (uzoq-poll)
func (s *Server) handleComputePoll(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Models    []string `json:"models"`
		Engines   []string `json:"engines"`
		AllowPull *bool    `json:"allowPull"`
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
	wv := workerViewFrom(n, &req)
	s.db.mu.Unlock()
	if len(wv.engines) == 0 {
		writeErr(w, http.StatusBadRequest, "hisoblash dvigateli yo'q")
		return
	}

	ctx := r.Context()
	j := s.compute.poll(ctx, wv, s.cfg.ComputePollTimeout)
	if j == nil {
		writeJSON(w, http.StatusOK, map[string]any{"job": nil})
		return
	}
	j.mu.Lock()
	j.StartedAt = s.now()
	view := map[string]any{
		"id":       j.ID,
		"model":    j.Model,
		"kind":     j.Kind,
		"messages": j.Messages,
		"prompt":   j.Prompt,
		"params":   j.Params,
	}
	j.mu.Unlock()
	writeJSON(w, http.StatusOK, map[string]any{"job": view})
}

// computeJobForWorker — ishni jobId bo'yicha topadi va Bearer token ishning tayinlangan
// tuguniga tegishli ekanini tekshiradi (bu yo'llarda {id} segmenti yo'q).
func (s *Server) computeJobForWorker(w http.ResponseWriter, r *http.Request) (*Node, *ComputeJob) {
	j := s.compute.get(r.PathValue("jobId"))
	if j == nil || j.Worker == "" {
		writeErr(w, http.StatusNotFound, "ish topilmadi")
		return nil, nil
	}
	s.db.mu.Lock()
	n := s.db.Nodes[j.Worker]
	hash, has := s.db.Tokens[j.Worker]
	s.db.mu.Unlock()
	tok := bearer(r)
	if n == nil || !has || tok == "" || !tokenEqual(sha256Hex([]byte(tok)), hash) {
		writeErr(w, http.StatusUnauthorized, "tugun tokeni noto'g'ri")
		return nil, nil
	}
	return n, j
}

// handleComputeChunk — tugun yangi token(lar)ni yuboradi
func (s *Server) handleComputeChunk(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Token  string `json:"token"`
		Tokens string `json:"tokens"`
	}
	if !readJSON(w, r, &req) {
		return
	}
	_, j := s.computeJobForWorker(w, r)
	if j == nil {
		return
	}
	tok := req.Token + req.Tokens
	if !j.appendToken(tok) {
		writeJSON(w, http.StatusConflict, map[string]any{"ok": false, "canceled": true})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

// handleComputeDone — tugun ishni yakunladi
func (s *Server) handleComputeDone(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Result       string    `json:"result"`
		FinishReason string    `json:"finishReason"`
		PromptTokens int       `json:"promptTokens"`
		Embedding    []float64 `json:"embedding"`
	}
	if !readJSON(w, r, &req) {
		return
	}
	n, j := s.computeJobForWorker(w, r)
	if j == nil {
		return
	}
	reason := req.FinishReason
	if reason == "" {
		reason = "stop"
	}
	j.finish(JobDone, req.Result, reason, "", req.PromptTokens, req.Embedding, s.now())
	s.compute.finishWorkerJob(n.ID, j.ID)
	writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

// handleComputeFail — tugun ishni bajara olmadi (modelni yuklab bo'lmadi va h.k.)
func (s *Server) handleComputeFail(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Error   string `json:"error"`
		Requeue bool   `json:"requeue"`
	}
	if !readJSON(w, r, &req) {
		return
	}
	n, j := s.computeJobForWorker(w, r)
	if j == nil {
		return
	}
	s.compute.finishWorkerJob(n.ID, j.ID)
	// boshqa tugun urinib ko'rishi mumkin bo'lsa — navbatga qaytaramiz
	if req.Requeue && s.now()-j.CreatedAt < int64(s.cfg.ComputeQueueTimeout.Seconds()) {
		j.mu.Lock()
		if !j.terminal && len(j.tokens) == 0 {
			j.Status = JobQueued
			j.Worker = ""
			j.mu.Unlock()
			s.compute.mu.Lock()
			s.compute.queue = append(s.compute.queue, j.ID)
			s.compute.broadcastLocked()
			s.compute.mu.Unlock()
			writeJSON(w, http.StatusOK, map[string]any{"ok": true, "requeued": true})
			return
		}
		j.mu.Unlock()
	}
	j.finish(JobFailed, "", "error", clip(req.Error, 300), 0, nil, s.now())
	writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

// ======================= HTTP: requester / operator =======================

func sseSetup(w http.ResponseWriter) (func(string, any), bool) {
	flusher, ok := w.(http.Flusher)
	if !ok {
		return nil, false
	}
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Connection", "keep-alive")
	w.Header().Set("X-Accel-Buffering", "no")
	w.WriteHeader(http.StatusOK)
	flusher.Flush()
	send := func(event string, data any) {
		if event != "" {
			fmt.Fprintf(w, "event: %s\n", event)
		}
		if s, isStr := data.(string); isStr {
			fmt.Fprintf(w, "data: %s\n\n", s)
		} else {
			b, _ := json.Marshal(data)
			fmt.Fprintf(w, "data: %s\n\n", b)
		}
		flusher.Flush()
	}
	return send, true
}

func (s *Server) parseJobRequest(w http.ResponseWriter, r *http.Request) (*ComputeJob, bool) {
	var req struct {
		Model    string         `json:"model"`
		Kind     string         `json:"kind"`
		Messages []ChatMessage  `json:"messages"`
		Prompt   string         `json:"prompt"`
		Params   map[string]any `json:"params"`
	}
	if !readJSON(w, r, &req) {
		return nil, false
	}
	if strings.TrimSpace(req.Model) == "" {
		writeErr(w, http.StatusBadRequest, "model kerak")
		return nil, false
	}
	kind := req.Kind
	if kind == "" {
		if len(req.Messages) > 0 {
			kind = KindChat
		} else {
			kind = KindGenerate
		}
	}
	if kind != KindChat && kind != KindGenerate && kind != KindEmbedding {
		writeErr(w, http.StatusBadRequest, "kind: chat | generate | embedding")
		return nil, false
	}
	if kind == KindChat && len(req.Messages) == 0 {
		writeErr(w, http.StatusBadRequest, "messages kerak")
		return nil, false
	}
	if (kind == KindGenerate || kind == KindEmbedding) && strings.TrimSpace(req.Prompt) == "" {
		writeErr(w, http.StatusBadRequest, "prompt kerak")
		return nil, false
	}
	return &ComputeJob{
		ID: randomID("job"), Model: clip(req.Model, 128), Kind: kind,
		Messages: req.Messages, Prompt: req.Prompt, Params: req.Params,
	}, true
}

// handleComputeSubmit — ish yuborish (bizning API). ?stream=1 bo'lsa SSE.
func (s *Server) handleComputeSubmit(w http.ResponseWriter, r *http.Request) {
	j, ok := s.parseJobRequest(w, r)
	if !ok {
		return
	}
	if !s.compute.modelReachable(j.Model) {
		writeErr(w, http.StatusServiceUnavailable, "bu modelni ishga tushira oladigan onlayn tugun yo'q: "+j.Model)
		return
	}
	s.compute.submit(j)
	if r.URL.Query().Get("stream") == "1" {
		s.streamJobSSE(w, r, j, false)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "jobId": j.ID})
}

func jobResultView(j *ComputeJob) map[string]any {
	j.mu.Lock()
	defer j.mu.Unlock()
	out := map[string]any{
		"id": j.ID, "model": j.Model, "kind": j.Kind, "status": j.Status,
		"worker": j.Worker, "createdAt": j.CreatedAt, "doneAt": j.DoneAt,
		"finishReason": j.finishReason,
	}
	if j.terminal {
		out["result"] = j.result
		out["error"] = j.errMsg
		out["promptTokens"] = j.promptTokens
		out["outputTokens"] = j.outTokens
		if len(j.embedding) > 0 {
			out["embedding"] = j.embedding
		}
	} else {
		out["partial"] = strings.Join(j.tokens, "")
	}
	return out
}

func (s *Server) handleComputeJob(w http.ResponseWriter, r *http.Request) {
	j := s.compute.get(r.PathValue("jobId"))
	if j == nil {
		writeErr(w, http.StatusNotFound, "ish topilmadi")
		return
	}
	writeJSON(w, http.StatusOK, jobResultView(j))
}

func (s *Server) handleComputeStream(w http.ResponseWriter, r *http.Request) {
	j := s.compute.get(r.PathValue("jobId"))
	if j == nil {
		writeErr(w, http.StatusNotFound, "ish topilmadi")
		return
	}
	s.streamJobSSE(w, r, j, false)
}

// streamJobSSE — bizning oddiy SSE formatimiz (token, done, error hodisalari)
func (s *Server) streamJobSSE(w http.ResponseWriter, r *http.Request, j *ComputeJob, _ bool) {
	send, ok := sseSetup(w)
	if !ok {
		writeErr(w, http.StatusInternalServerError, "streaming qo'llanmaydi")
		return
	}
	ctx := r.Context()
	j.stream(ctx, func(tok string) bool {
		send("token", map[string]any{"token": tok})
		return true
	}, func() {
		j.mu.Lock()
		if j.Status == JobDone {
			send("done", map[string]any{"result": j.result, "finishReason": j.finishReason,
				"promptTokens": j.promptTokens, "outputTokens": j.outTokens})
		} else {
			send("error", map[string]any{"error": j.errMsg, "status": j.Status})
		}
		j.mu.Unlock()
		send("", "[DONE]")
	})
}

func (s *Server) handleComputeWorkers(w http.ResponseWriter, r *http.Request) {
	cm := s.compute
	cm.mu.Lock()
	queued := len(cm.queue)
	cm.mu.Unlock()
	writeJSON(w, http.StatusOK, map[string]any{
		"workers": cm.workers(false),
		"models":  cm.models(),
		"queued":  queued,
	})
}

func (s *Server) handleComputeModels(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{"models": s.compute.models()})
}

// ======================= OpenAI-mos gateway =======================

func (s *Server) computeKeyOK(r *http.Request) bool {
	tok := bearer(r)
	if tok == "" {
		// OpenAI mijozlari "Authorization: Bearer" ishlatadi; bo'sh bo'lsa ruxsat yo'q
		return false
	}
	if tokenEqual(tok, s.adminToken) {
		return true
	}
	for _, k := range s.cfg.ComputeKeys {
		if tokenEqual(tok, k) {
			return true
		}
	}
	return false
}

// computeSubmitAuth — OpenAI kaliti (tashqi dasturlar) YOKI admin sessiya (brauzer paneli).
func (s *Server) computeSubmitAuth(h http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if s.computeKeyOK(r) {
			h(w, r)
			return
		}
		ok, csrf := s.isAdmin(r)
		if ok && csrf {
			h(w, r)
			return
		}
		if ok {
			writeErr(w, http.StatusForbidden, "CSRF tekshiruvi o'tmadi")
			return
		}
		writeErr(w, http.StatusUnauthorized, "kirish kerak (admin yoki API kaliti)")
	}
}

func (s *Server) openaiAuth(h http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !s.computeKeyOK(r) {
			writeJSON(w, http.StatusUnauthorized, map[string]any{
				"error": map[string]any{"message": "noto'g'ri API kaliti", "type": "invalid_request_error"}})
			return
		}
		h(w, r)
	}
}

func (s *Server) handleOpenAIModels(w http.ResponseWriter, r *http.Request) {
	models := s.compute.models()
	data := make([]map[string]any, 0, len(models))
	for _, m := range models {
		data = append(data, map[string]any{"id": m, "object": "model", "owned_by": "sdrive-cluster", "created": 0})
	}
	writeJSON(w, http.StatusOK, map[string]any{"object": "list", "data": data})
}

func (s *Server) handleOpenAIChat(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Model       string        `json:"model"`
		Messages    []ChatMessage `json:"messages"`
		Stream      bool          `json:"stream"`
		Temperature *float64      `json:"temperature"`
		TopP        *float64      `json:"top_p"`
		MaxTokens   *int          `json:"max_tokens"`
		Stop        any           `json:"stop"`
		Seed        *int          `json:"seed"`
	}
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 4<<20)).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]any{"error": map[string]any{"message": err.Error(), "type": "invalid_request_error"}})
		return
	}
	if req.Model == "" || len(req.Messages) == 0 {
		writeJSON(w, http.StatusBadRequest, map[string]any{"error": map[string]any{"message": "model va messages kerak", "type": "invalid_request_error"}})
		return
	}
	params := map[string]any{}
	if req.Temperature != nil {
		params["temperature"] = *req.Temperature
	}
	if req.TopP != nil {
		params["top_p"] = *req.TopP
	}
	if req.MaxTokens != nil {
		params["max_tokens"] = *req.MaxTokens
	}
	if req.Seed != nil {
		params["seed"] = *req.Seed
	}
	if req.Stop != nil {
		params["stop"] = req.Stop
	}
	j := &ComputeJob{ID: randomID("job"), Model: clip(req.Model, 128), Kind: KindChat, Messages: req.Messages, Params: params}
	if !s.compute.modelReachable(j.Model) {
		writeJSON(w, http.StatusServiceUnavailable, map[string]any{"error": map[string]any{
			"message": "bu modelni ishga tushira oladigan onlayn tugun yo'q: " + j.Model, "type": "model_not_available"}})
		return
	}
	s.compute.submit(j)
	created := s.now()
	cmplID := "chatcmpl-" + strings.TrimPrefix(j.ID, "job_")

	if req.Stream {
		send, ok := sseSetup(w)
		if !ok {
			writeErr(w, http.StatusInternalServerError, "streaming qo'llanmaydi")
			return
		}
		roleChunk := map[string]any{"id": cmplID, "object": "chat.completion.chunk", "created": created, "model": j.Model,
			"choices": []map[string]any{{"index": 0, "delta": map[string]any{"role": "assistant"}, "finish_reason": nil}}}
		send("", roleChunk)
		j.stream(r.Context(), func(tok string) bool {
			send("", map[string]any{"id": cmplID, "object": "chat.completion.chunk", "created": created, "model": j.Model,
				"choices": []map[string]any{{"index": 0, "delta": map[string]any{"content": tok}, "finish_reason": nil}}})
			return true
		}, func() {
			j.mu.Lock()
			reason := j.finishReason
			if j.Status != JobDone && reason == "" {
				reason = "error"
			}
			j.mu.Unlock()
			send("", map[string]any{"id": cmplID, "object": "chat.completion.chunk", "created": created, "model": j.Model,
				"choices": []map[string]any{{"index": 0, "delta": map[string]any{}, "finish_reason": reason}}})
			send("", "[DONE]")
		})
		return
	}

	j.waitDone(r.Context())
	j.mu.Lock()
	defer j.mu.Unlock()
	if j.Status != JobDone {
		writeJSON(w, http.StatusBadGateway, map[string]any{"error": map[string]any{"message": j.errMsg, "type": "inference_error"}})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"id": cmplID, "object": "chat.completion", "created": created, "model": j.Model,
		"choices": []map[string]any{{"index": 0, "message": map[string]any{"role": "assistant", "content": j.result},
			"finish_reason": j.finishReason}},
		"usage": map[string]any{"prompt_tokens": j.promptTokens, "completion_tokens": j.outTokens,
			"total_tokens": j.promptTokens + j.outTokens},
	})
}

func (s *Server) handleOpenAIEmbeddings(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Model string `json:"model"`
		Input any    `json:"input"`
	}
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 4<<20)).Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]any{"error": map[string]any{"message": err.Error(), "type": "invalid_request_error"}})
		return
	}
	var inputs []string
	switch v := req.Input.(type) {
	case string:
		inputs = []string{v}
	case []any:
		for _, x := range v {
			if s, ok := x.(string); ok {
				inputs = append(inputs, s)
			}
		}
	}
	if req.Model == "" || len(inputs) == 0 {
		writeJSON(w, http.StatusBadRequest, map[string]any{"error": map[string]any{"message": "model va input kerak", "type": "invalid_request_error"}})
		return
	}
	if !s.compute.modelReachable(req.Model) {
		writeJSON(w, http.StatusServiceUnavailable, map[string]any{"error": map[string]any{"message": "model mavjud emas", "type": "model_not_available"}})
		return
	}
	data := make([]map[string]any, 0, len(inputs))
	totalPrompt := 0
	for i, in := range inputs {
		j := &ComputeJob{ID: randomID("job"), Model: clip(req.Model, 128), Kind: KindEmbedding, Prompt: in}
		s.compute.submit(j)
		j.waitDone(r.Context())
		j.mu.Lock()
		if j.Status != JobDone {
			msg := j.errMsg
			j.mu.Unlock()
			writeJSON(w, http.StatusBadGateway, map[string]any{"error": map[string]any{"message": msg, "type": "inference_error"}})
			return
		}
		data = append(data, map[string]any{"object": "embedding", "index": i, "embedding": j.embedding})
		totalPrompt += j.promptTokens
		j.mu.Unlock()
	}
	writeJSON(w, http.StatusOK, map[string]any{"object": "list", "data": data, "model": req.Model,
		"usage": map[string]any{"prompt_tokens": totalPrompt, "total_tokens": totalPrompt}})
}

// ---------------- Router ----------------

func (s *Server) computeRoutes(mux *http.ServeMux) {
	// worker (tugun tokeni)
	mux.HandleFunc("POST /api/nodes/{id}/compute/poll", s.handleComputePoll)
	mux.HandleFunc("POST /api/compute/jobs/{jobId}/chunk", s.handleComputeChunk)
	mux.HandleFunc("POST /api/compute/jobs/{jobId}/done", s.handleComputeDone)
	mux.HandleFunc("POST /api/compute/jobs/{jobId}/fail", s.handleComputeFail)

	// requester / operator (compute kaliti yoki admin)
	mux.HandleFunc("POST /api/compute/jobs", s.computeSubmitAuth(s.handleComputeSubmit))
	mux.HandleFunc("GET /api/compute/jobs/{jobId}", s.handleComputeJob)
	mux.HandleFunc("GET /api/compute/jobs/{jobId}/stream", s.handleComputeStream)
	mux.HandleFunc("GET /api/compute/workers", s.handleComputeWorkers)
	mux.HandleFunc("GET /api/compute/models", s.handleComputeModels)

	// OpenAI-mos
	mux.HandleFunc("POST /v1/chat/completions", s.openaiAuth(s.handleOpenAIChat))
	mux.HandleFunc("POST /v1/embeddings", s.openaiAuth(s.handleOpenAIEmbeddings))
	mux.HandleFunc("GET /v1/models", s.openaiAuth(s.handleOpenAIModels))
}

package main

import (
	"bytes"
	"crypto/tls"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"log"
	mrand "math/rand/v2"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

// ================= Tugun protokoli (Go mijozi) =================
//
// nodeClient — C++ mijoz bilan bir xil HTTP protokolida ishlaydigan virtual tugun.
// Simulyator va integratsion testlar shu orqali haqiqiy API ni sinaydi.

type nodeClient struct {
	base             string
	hc               *http.Client
	ID               string
	Token            string
	Dir              string
	Name             string
	OS               string
	SpaceTotal       int64
	agreementVersion string
	Compute          *ComputeInfo // nil bo'lsa — faqat xotira tuguni
}

type nodeTask struct {
	ID     string  `json:"id"`
	Kind   string  `json:"kind"`
	FileID string  `json:"fileId"`
	Chunks []Chunk `json:"chunks"`
}

type heartbeatResp struct {
	TasksDue      int         `json:"tasksDue"`
	Audits        []auditView `json:"audits"`
	EarningsCents float64     `json:"earningsCents"`
	StoredBytes   int64       `json:"storedBytes"`
	Error         string      `json:"error"`
}

func (c *nodeClient) do(method, path string, body io.Reader, ctype string, out any) (int, error) {
	req, err := http.NewRequest(method, c.base+path, body)
	if err != nil {
		return 0, err
	}
	if c.Token != "" {
		req.Header.Set("Authorization", "Bearer "+c.Token)
	}
	if ctype != "" {
		req.Header.Set("Content-Type", ctype)
	}
	resp, err := c.hc.Do(req)
	if err != nil {
		return 0, err
	}
	defer resp.Body.Close()
	switch v := out.(type) {
	case nil:
		io.Copy(io.Discard, resp.Body)
	case *[]byte:
		*v, err = io.ReadAll(resp.Body)
	default:
		err = json.NewDecoder(resp.Body).Decode(out)
		if errors.Is(err, io.EOF) {
			err = nil
		}
	}
	return resp.StatusCode, err
}

func (c *nodeClient) postJSON(path string, in, out any) (int, error) {
	b, _ := json.Marshal(in)
	return c.do(http.MethodPost, path, bytes.NewReader(b), "application/json", out)
}

func (c *nodeClient) register() error {
	var resp struct {
		Token string `json:"token"`
		Error string `json:"error"`
	}
	code, err := c.postJSON("/api/nodes/register", map[string]any{
		"id":                c.ID,
		"name":              c.Name,
		"os":                c.OS,
		"platform":          "x64",
		"version":           "sim-2.0",
		"cpuCores":          4,
		"spaceTotal":        c.SpaceTotal,
		"agreementVersion":  c.agreementVersion,
		"agreementAccepted": true,
		"compute":           c.Compute,
	}, &resp)
	if err != nil {
		return err
	}
	if code != http.StatusOK {
		return fmt.Errorf("register: HTTP %d %s", code, resp.Error)
	}
	if resp.Token != "" {
		c.Token = resp.Token
	}
	return nil
}

func (c *nodeClient) usedBytes() int64 {
	var total int64
	filepath.WalkDir(c.Dir, func(p string, d fs.DirEntry, err error) error {
		if err == nil && d.Type().IsRegular() && strings.HasSuffix(p, ".bin") {
			if fi, err := d.Info(); err == nil {
				total += fi.Size()
			}
		}
		return nil
	})
	return total
}

func (c *nodeClient) heartbeat() (heartbeatResp, int, error) {
	var r heartbeatResp
	code, err := c.postJSON("/api/nodes/"+c.ID+"/heartbeat", map[string]any{
		"spaceUsed":  c.usedBytes(),
		"userActive": false,
	}, &r)
	return r, code, err
}

func (c *nodeClient) chunkFile(fileID string, idx int) string {
	return filepath.Join(c.Dir, fileID, fmt.Sprintf("%d.bin", idx))
}

func (c *nodeClient) answerAudits(audits []auditView) error {
	results := []map[string]string{}
	for _, a := range audits {
		b, err := os.ReadFile(c.chunkFile(a.FileID, a.Index))
		if err != nil {
			results = append(results, map[string]string{"id": a.ID, "error": "chunk topilmadi"})
			continue
		}
		results = append(results, map[string]string{"id": a.ID, "proof": auditProof(a.Nonce, b)})
	}
	_, err := c.postJSON("/api/nodes/"+c.ID+"/audits", map[string]any{"results": results}, nil)
	return err
}

func (c *nodeClient) runTask(t nodeTask) error {
	switch t.Kind {
	case KindReplica, KindRepair:
		for _, ch := range t.Chunks {
			p := c.chunkFile(t.FileID, ch.Index)
			if b, err := os.ReadFile(p); err == nil && sha256Hex(b) == ch.SHA256 {
				continue // allaqachon bor
			}
			var data []byte
			code, err := c.do(http.MethodGet, fmt.Sprintf("/api/nodes/%s/files/%s/chunks/%d", c.ID, t.FileID, ch.Index), nil, "", &data)
			if err != nil {
				return err
			}
			if code != http.StatusOK {
				return fmt.Errorf("chunk %d: HTTP %d", ch.Index, code)
			}
			if sha256Hex(data) != ch.SHA256 {
				return fmt.Errorf("chunk %d: sha256 mos emas", ch.Index)
			}
			if err := writeFileAtomic(p, data); err != nil {
				return err
			}
		}
	case KindDelete:
		return os.RemoveAll(filepath.Join(c.Dir, t.FileID))
	case KindRestore:
		for _, ch := range t.Chunks {
			b, err := os.ReadFile(c.chunkFile(t.FileID, ch.Index))
			if err != nil {
				return fmt.Errorf("chunk %d lokal topilmadi", ch.Index)
			}
			code, err := c.do(http.MethodPut, fmt.Sprintf("/api/nodes/%s/tasks/%s/chunks/%d", c.ID, t.ID, ch.Index),
				bytes.NewReader(b), "application/octet-stream", nil)
			if err != nil {
				return err
			}
			if code != http.StatusOK {
				return fmt.Errorf("restore chunk %d: HTTP %d", ch.Index, code)
			}
		}
	default:
		return fmt.Errorf("noma'lum vazifa turi: %s", t.Kind)
	}
	return nil
}

// processTasks — kutilayotgan vazifalarni bajaradi, bajarilganlar sonini qaytaradi
func (c *nodeClient) processTasks() (int, error) {
	var resp struct {
		Tasks []nodeTask `json:"tasks"`
	}
	code, err := c.do(http.MethodGet, "/api/nodes/"+c.ID+"/tasks", nil, "", &resp)
	if err != nil {
		return 0, err
	}
	if code != http.StatusOK {
		return 0, fmt.Errorf("tasks: HTTP %d", code)
	}
	done := 0
	for _, t := range resp.Tasks {
		if err := c.runTask(t); err != nil {
			c.postJSON("/api/nodes/"+c.ID+"/tasks/"+t.ID+"/fail", map[string]any{"error": err.Error()}, nil)
			continue
		}
		code, err := c.postJSON("/api/nodes/"+c.ID+"/tasks/"+t.ID+"/done", map[string]any{}, nil)
		if err != nil {
			continue
		}
		if code == http.StatusGone {
			os.RemoveAll(filepath.Join(c.Dir, t.FileID)) // vazifa bekor qilingan
		}
		done++
	}
	return done, nil
}

// tick — bitta to'liq sikl: heartbeat, audit javoblari, vazifalar
func (c *nodeClient) tick() error {
	hb, code, err := c.heartbeat()
	if err != nil {
		return err
	}
	if code != http.StatusOK {
		return fmt.Errorf("heartbeat: HTTP %d %s", code, hb.Error)
	}
	if len(hb.Audits) > 0 {
		c.answerAudits(hb.Audits)
	}
	if hb.TasksDue > 0 {
		_, err = c.processTasks()
	}
	return err
}

// ================= Simulyator =================

type simNode struct {
	c       *nodeClient
	quit    chan struct{}
	done    chan struct{}
	wake    chan struct{}
	running bool
}

type simManager struct {
	s     *Server
	dir   string
	hc    *http.Client
	mu    sync.Mutex
	nodes map[string]*simNode
	seq   int
}

func newSimManager(s *Server, dir string) *simManager {
	return &simManager{s: s, dir: dir, hc: &http.Client{Timeout: 2 * time.Minute}, nodes: map[string]*simNode{}}
}

// insecureTLS — server HTTPS da ishlaganda virtual tugunlar o'z serveriga (127.0.0.1)
// sertifikat nomi tekshiruvisiz ulanadi; tashqi ulanishlarga ta'sir qilmaydi.
func (m *simManager) insecureTLS() {
	m.hc = &http.Client{Timeout: 2 * time.Minute, Transport: &http.Transport{
		TLSClientConfig: &tls.Config{InsecureSkipVerify: true}, //nolint:gosec // faqat loopback
	}}
}

var simOS = []string{"Windows 11", "Windows 10", "macOS Sonoma", "Ubuntu 24.04", "Debian 12"}
var simSpaceGB = []int64{50, 100, 200, 500, 1000}

func (m *simManager) launch(sn *simNode) {
	sn.quit = make(chan struct{})
	sn.done = make(chan struct{})
	sn.wake = make(chan struct{}, 1)
	sn.running = true
	interval := m.s.cfg.HeartbeatInterval
	go func(quit, done, wake chan struct{}) {
		defer close(done)
		for {
			sn.c.tick()
			select {
			case <-quit:
				return
			case <-wake:
			case <-time.After(interval):
			}
		}
	}(sn.quit, sn.done, sn.wake)
}

func (m *simManager) halt(sn *simNode) {
	close(sn.quit)
	<-sn.done
}

func (m *simManager) start(count int) (int, error) {
	m.mu.Lock()
	if room := m.s.cfg.MaxSimNodes - len(m.nodes); count > room {
		count = room
	}
	m.mu.Unlock()
	if count <= 0 {
		return 0, fmt.Errorf("simulyator tugunlari chegarasi: %d", m.s.cfg.MaxSimNodes)
	}
	started := 0
	for i := 0; i < count; i++ {
		m.mu.Lock()
		m.seq++
		seq := m.seq
		m.mu.Unlock()
		id := "sim-" + randomHex(5)
		c := &nodeClient{
			base:             m.s.baseURL,
			hc:               m.hc,
			ID:               id,
			Dir:              filepath.Join(m.dir, id),
			Name:             fmt.Sprintf("Simulyator #%d", seq),
			OS:               simOS[mrand.IntN(len(simOS))],
			SpaceTotal:       simSpaceGB[mrand.IntN(len(simSpaceGB))] << 30,
			agreementVersion: m.s.agreementVersion,
		}
		if err := c.register(); err != nil {
			return started, err
		}
		if err := os.MkdirAll(c.Dir, 0o755); err != nil {
			return started, err
		}
		if err := os.WriteFile(filepath.Join(c.Dir, "token"), []byte(c.Token), 0o600); err != nil {
			return started, err
		}
		m.s.db.mu.Lock()
		if n := m.s.db.Nodes[id]; n != nil {
			n.Simulated = true
			m.s.db.markDirty()
		}
		m.s.db.mu.Unlock()
		sn := &simNode{c: c}
		m.mu.Lock()
		m.nodes[id] = sn
		m.launch(sn)
		m.mu.Unlock()
		started++
	}
	m.s.triggerReconcile()
	return started, nil
}

// resume — server qayta ishga tushganda oldingi simulyator tugunlarini davom ettiradi
func (m *simManager) resume() {
	type meta struct {
		id, name, os string
		space        int64
	}
	var list []meta
	var stale []string
	m.s.db.mu.Lock()
	for _, n := range m.s.db.Nodes {
		if !n.Simulated {
			continue
		}
		if n.Status != NodeActive {
			stale = append(stale, n.ID)
			continue
		}
		list = append(list, meta{n.ID, n.Name, n.OS, n.SpaceTotal})
	}
	m.s.db.mu.Unlock()
	for _, x := range list {
		dir := filepath.Join(m.dir, x.id)
		tok, err := os.ReadFile(filepath.Join(dir, "token"))
		if err != nil {
			stale = append(stale, x.id)
			continue
		}
		c := &nodeClient{base: m.s.baseURL, hc: m.hc, ID: x.id, Token: strings.TrimSpace(string(tok)), Dir: dir,
			Name: x.name, OS: x.os, SpaceTotal: x.space, agreementVersion: m.s.agreementVersion}
		sn := &simNode{c: c}
		m.mu.Lock()
		m.nodes[x.id] = sn
		m.seq++
		m.launch(sn)
		m.mu.Unlock()
	}
	if len(stale) > 0 {
		m.s.db.mu.Lock()
		for _, id := range stale {
			m.s.purgeNodeLocked(id)
		}
		m.s.db.mu.Unlock()
	}
	if len(list) > 0 {
		log.Printf("simulyator: %d ta virtual tugun davom ettirildi", len(list))
	}
}

func (m *simManager) wakeAll() {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, sn := range m.nodes {
		if sn.running {
			select {
			case sn.wake <- struct{}{}:
			default:
			}
		}
	}
}

// offline — tasodifiy count ta tugunni to'xtatadi (internet/elektr uzilishi simulyatsiyasi)
func (m *simManager) offline(count int) int {
	m.mu.Lock()
	var running []*simNode
	for _, sn := range m.nodes {
		if sn.running {
			running = append(running, sn)
		}
	}
	mrand.Shuffle(len(running), func(i, j int) { running[i], running[j] = running[j], running[i] })
	if count > len(running) {
		count = len(running)
	}
	victims := running[:count]
	for _, sn := range victims {
		sn.running = false
	}
	m.mu.Unlock()
	for _, sn := range victims {
		m.halt(sn)
	}
	return len(victims)
}

func (m *simManager) online() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	n := 0
	for _, sn := range m.nodes {
		if !sn.running {
			m.launch(sn)
			n++
		}
	}
	return n
}

func (m *simManager) stopAll() {
	m.mu.Lock()
	var list []*simNode
	for _, sn := range m.nodes {
		if sn.running {
			sn.running = false
			list = append(list, sn)
		}
	}
	m.mu.Unlock()
	for _, sn := range list {
		m.halt(sn)
	}
}

// remove — barcha virtual tugunlarni o'chiradi; ulardagi nusxalar real tugunlarga ko'chiriladi
func (m *simManager) remove() int {
	m.stopAll()
	m.mu.Lock()
	ids := make([]string, 0, len(m.nodes))
	for id := range m.nodes {
		ids = append(ids, id)
	}
	m.nodes = map[string]*simNode{}
	m.mu.Unlock()
	m.s.db.mu.Lock()
	for _, n := range m.s.db.Nodes {
		if n.Simulated && !contains(ids, n.ID) {
			ids = append(ids, n.ID)
		}
	}
	for _, id := range ids {
		m.s.purgeNodeLocked(id)
	}
	m.s.db.mu.Unlock()
	os.RemoveAll(m.dir)
	m.s.triggerReconcile()
	return len(ids)
}

func (m *simManager) status() map[string]any {
	m.mu.Lock()
	defer m.mu.Unlock()
	running := 0
	for _, sn := range m.nodes {
		if sn.running {
			running++
		}
	}
	return map[string]any{"total": len(m.nodes), "running": running, "stopped": len(m.nodes) - running, "max": m.s.cfg.MaxSimNodes}
}

// ---------------- Admin API ----------------

func (s *Server) handleSimStatus(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, s.sim.status())
}

func simCount(w http.ResponseWriter, r *http.Request) (int, bool) {
	var req struct {
		Count int `json:"count"`
	}
	if !readJSON(w, r, &req) {
		return 0, false
	}
	if req.Count < 1 || req.Count > 200 {
		writeErr(w, http.StatusBadRequest, "count 1..200 oralig'ida bo'lishi kerak")
		return 0, false
	}
	return req.Count, true
}

func (s *Server) handleSimStart(w http.ResponseWriter, r *http.Request) {
	count, ok := simCount(w, r)
	if !ok {
		return
	}
	n, err := s.sim.start(count)
	if err != nil && n == 0 {
		writeErr(w, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "started": n, "sim": s.sim.status()})
}

func (s *Server) handleSimOffline(w http.ResponseWriter, r *http.Request) {
	count, ok := simCount(w, r)
	if !ok {
		return
	}
	n := s.sim.offline(count)
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "stopped": n, "sim": s.sim.status(),
		"note": fmt.Sprintf("%s dan keyin oflayn deb belgilanadi, %s dan keyin nusxalar boshqa tugunlarga ko'chiriladi", s.cfg.OfflineTimeout, s.cfg.RepairDelay)})
}

func (s *Server) handleSimOnline(w http.ResponseWriter, r *http.Request) {
	n := s.sim.online()
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "resumed": n, "sim": s.sim.status()})
}

func (s *Server) handleSimRemove(w http.ResponseWriter, r *http.Request) {
	n := s.sim.remove()
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "removed": n, "sim": s.sim.status()})
}

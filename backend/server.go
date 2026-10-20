package main

import (
	"context"
	"embed"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"time"
)

//go:embed web/*.html web/*.js web/*.css
var embeddedWeb embed.FS

type Server struct {
	cfg       Config
	db        *DB
	clock     func() time.Time
	chunksDir string
	baseURL   string // simulyator tugunlari shu manzil orqali ulanadi

	agreementText    string
	agreementVersion string

	adminToken string
	sessions   *sessionStore

	// saqlash isboti (audit) holati — db.mu ostida
	audits        map[string]*audit
	lastAudit     map[string]int64
	priorityAudit map[string][]string

	kick    chan struct{}
	sim     *simManager
	compute *computeManager
	web     http.FileSystem

	downloadsDir string
	wg           sync.WaitGroup
}

func NewServer(cfg Config) (*Server, error) {
	dataDir, err := filepath.Abs(cfg.DataDir)
	if err != nil {
		return nil, err
	}
	cfg.DataDir = dataDir
	s := &Server{
		cfg:           cfg,
		clock:         time.Now,
		chunksDir:     filepath.Join(dataDir, "chunks"),
		sessions:      newSessionStore(),
		audits:        map[string]*audit{},
		lastAudit:     map[string]int64{},
		priorityAudit: map[string][]string{},
		kick:          make(chan struct{}, 1),
	}
	if err := os.MkdirAll(s.chunksDir, 0o755); err != nil {
		return nil, err
	}
	s.db, err = OpenDB(filepath.Join(dataDir, "db.json"))
	if err != nil {
		return nil, fmt.Errorf("db ochishda xato: %w", err)
	}
	s.migrate()

	if err := s.loadAgreement(); err != nil {
		return nil, err
	}
	if err := s.loadAdminToken(); err != nil {
		return nil, err
	}
	s.setupWeb()
	s.sim = newSimManager(s, filepath.Join(dataDir, "sim"))
	s.compute = newComputeManager(s)
	return s, nil
}

func (s *Server) now() int64 { return s.clock().Unix() }

// Start — fon jarayonlari: replikatsiya menejeri, diskka yozish, server chunk tekshiruvi
func (s *Server) Start(ctx context.Context) {
	s.wg.Add(4)
	go func() { defer s.wg.Done(); s.reconcileLoop(ctx) }()
	go func() { defer s.wg.Done(); s.flushLoop(ctx) }()
	go func() { defer s.wg.Done(); s.scrubLoop(ctx) }()
	go func() { defer s.wg.Done(); s.compute.reap(ctx) }()
	s.sim.resume()
}

func (s *Server) Close() {
	s.sim.stopAll()
	s.wg.Wait()
	if err := s.db.Flush(); err != nil {
		log.Printf("db saqlashda xato: %v", err)
	}
}

func (s *Server) flushLoop(ctx context.Context) {
	t := time.NewTicker(2 * time.Second)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			if err := s.db.Flush(); err != nil {
				log.Printf("db saqlashda xato: %v", err)
			}
		}
	}
}

// persist — muhim o'zgarishlarni (ro'yxatdan o'tish, yuklash, to'lov) darhol diskka yozadi
func (s *Server) persist() {
	if err := s.db.Flush(); err != nil {
		log.Printf("db saqlashda xato: %v", err)
	}
}

// triggerReconcile — replikatsiya menejerini darhol ishga tushirish (bloklamaydi)
func (s *Server) triggerReconcile() {
	select {
	case s.kick <- struct{}{}:
	default:
	}
}

// ---------------- Shartnoma ----------------

var agreementVersionRe = regexp.MustCompile(`Versiya:\s*([0-9][0-9.]*)`)

func (s *Server) loadAgreement() error {
	var candidates []string
	if s.cfg.AgreementPath != "" {
		candidates = []string{s.cfg.AgreementPath}
	} else {
		candidates = []string{"SHARTNOMA.md", filepath.Join("..", "SHARTNOMA.md")}
		if exe, err := os.Executable(); err == nil {
			dir := filepath.Dir(exe)
			candidates = append(candidates, filepath.Join(dir, "SHARTNOMA.md"), filepath.Join(dir, "..", "SHARTNOMA.md"))
		}
	}
	for _, p := range candidates {
		b, err := os.ReadFile(p)
		if err == nil && len(b) > 0 {
			s.setAgreement(string(b))
			return nil
		}
	}
	return fmt.Errorf("SHARTNOMA.md topilmadi (%s) — -agreement flag bilan yo'lini ko'rsating", strings.Join(candidates, ", "))
}

func (s *Server) setAgreement(text string) {
	s.agreementText = text
	s.agreementVersion = "1.0"
	if m := agreementVersionRe.FindStringSubmatch(text); m != nil {
		s.agreementVersion = strings.TrimRight(m[1], ".")
	}
}

// ---------------- Migratsiya (eski db.json formatidan) ----------------

func (s *Server) migrate() {
	d := s.db
	d.mu.Lock()
	defer d.mu.Unlock()
	for _, f := range d.Files {
		if f.ChunkSize == 0 {
			f.ChunkSize = s.cfg.ChunkSize
		}
		need := f.SHA256 == ""
		for _, c := range f.Chunks {
			if c.SHA256 == "" {
				need = true
			}
		}
		if !need {
			continue
		}
		// eski versiyada xesh saqlanmagan — diskdagi chunk'lardan hisoblaymiz
		var total int64
		ok := true
		for i := range f.Chunks {
			sum, n, err := hashFile(s.chunkPath(f.ID, f.Chunks[i].Index))
			if err != nil || n != f.Chunks[i].Size {
				ok = false
				f.Missing = appendMissing(f.Missing, f.Chunks[i].Index)
				continue
			}
			f.Chunks[i].SHA256 = sum
			total += n
		}
		if ok {
			if sum, err := s.wholeFileHash(f); err == nil {
				f.SHA256 = sum
			}
			f.Size = total
		}
		d.markDirty()
	}
	for _, t := range d.Tasks {
		if f, ok := d.Files[t.FileID]; ok && (t.Kind == KindReplica || t.Kind == KindRepair) {
			t.Chunks = f.Chunks
		}
	}
}

func (s *Server) chunkPath(fileID string, idx int) string {
	return filepath.Join(s.chunksDir, fileID, fmt.Sprintf("%d.bin", idx))
}

func (s *Server) wholeFileHash(f *File) (string, error) {
	h := newSHA256()
	for _, c := range f.Chunks {
		fh, err := os.Open(s.chunkPath(f.ID, c.Index))
		if err != nil {
			return "", err
		}
		_, err = io.Copy(h, fh)
		fh.Close()
		if err != nil {
			return "", err
		}
	}
	return h.hex(), nil
}

// ---------------- HTTP yordamchilar ----------------

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(v)
}

func writeErr(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, map[string]any{"ok": false, "error": msg})
}

// readJSON — so'rov tanasini (maks. 1 MiB) o'qiydi; bo'sh tana ham qabul qilinadi
func readJSON(w http.ResponseWriter, r *http.Request, v any) bool {
	body := http.MaxBytesReader(w, r.Body, 1<<20)
	dec := json.NewDecoder(body)
	if err := dec.Decode(v); err != nil && !errors.Is(err, io.EOF) {
		writeErr(w, http.StatusBadRequest, "noto'g'ri JSON so'rov: "+err.Error())
		return false
	}
	return true
}

// ---------------- Router ----------------

func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()

	// ochiq
	mux.HandleFunc("GET /api/info", s.handleInfo)
	mux.HandleFunc("GET /api/agreement", s.handleAgreement)
	mux.HandleFunc("GET /api/downloads", s.handleDownloads)
	mux.HandleFunc("POST /api/nodes/register", s.handleRegister)

	// tugun (Bearer token)
	mux.HandleFunc("GET /api/nodes/{id}", s.handleNodeInfo)
	mux.HandleFunc("POST /api/nodes/{id}/heartbeat", s.handleHeartbeat)
	mux.HandleFunc("GET /api/nodes/{id}/tasks", s.handleNodeTasks)
	mux.HandleFunc("GET /api/nodes/{id}/files/{fileId}/chunks/{idx}", s.handleNodeChunk)
	mux.HandleFunc("PUT /api/nodes/{id}/tasks/{taskId}/chunks/{idx}", s.handleRestoreChunk)
	mux.HandleFunc("POST /api/nodes/{id}/tasks/{taskId}/done", s.handleTaskDone)
	mux.HandleFunc("POST /api/nodes/{id}/tasks/{taskId}/fail", s.handleTaskFail)
	mux.HandleFunc("POST /api/nodes/{id}/audits", s.handleAudits)
	mux.HandleFunc("POST /api/nodes/{id}/repair", s.handleRepair)
	mux.HandleFunc("POST /api/nodes/{id}/payout", s.handlePayout)
	mux.HandleFunc("POST /api/nodes/{id}/uninstall", s.handleUninstall)

	// admin (sessiya cookie yoki Bearer admin token)
	mux.HandleFunc("POST /api/admin/login", s.handleLogin)
	mux.HandleFunc("POST /api/admin/logout", s.handleLogout)
	mux.HandleFunc("GET /api/admin/me", s.handleMe)
	mux.HandleFunc("GET /api/dashboard", s.admin(s.handleDashboard))
	mux.HandleFunc("POST /api/files/upload", s.admin(s.handleUpload))
	mux.HandleFunc("GET /api/files/{id}/download", s.admin(s.handleDownload))
	mux.HandleFunc("DELETE /api/files/{id}", s.admin(s.handleDeleteFile))
	mux.HandleFunc("POST /api/admin/reconcile", s.admin(s.handleReconcileNow))
	mux.HandleFunc("GET /api/admin/sim", s.admin(s.handleSimStatus))
	mux.HandleFunc("POST /api/admin/sim/start", s.admin(s.handleSimStart))
	mux.HandleFunc("POST /api/admin/sim/offline", s.admin(s.handleSimOffline))
	mux.HandleFunc("POST /api/admin/sim/online", s.admin(s.handleSimOnline))
	mux.HandleFunc("POST /api/admin/sim/remove", s.admin(s.handleSimRemove))

	// hisoblash (GPU/CPU) klasteri + OpenAI-mos gateway
	s.computeRoutes(mux)

	mux.HandleFunc("/api/", func(w http.ResponseWriter, r *http.Request) {
		writeErr(w, http.StatusNotFound, "API topilmadi: "+r.Method+" "+r.URL.Path)
	})

	// statik
	mux.HandleFunc("GET /client/{name}", s.handleClientDownload)
	mux.HandleFunc("GET /dashboard.html", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/#nodes", http.StatusFound)
	})
	files := http.FileServer(s.web)
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet && r.Method != http.MethodHead {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		files.ServeHTTP(w, r)
	})

	return securityHeaders(cors(drainBody(mux)))
}

// drainBody — handler o'qimagan so'rov tanasini o'qib tashlaydi. Go "Connection: close" so'rovlarida
// buni qilmaydi; o'qilmagan bayt bilan yopilgan soket TCP RST yuboradi va Windows mijozlari
// allaqachon kelgan javobni yo'qotadi (WSAECONNRESET).
func drainBody(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		next.ServeHTTP(w, r)
		if r.Body != nil && r.Method != http.MethodGet && r.Method != http.MethodHead {
			io.Copy(io.Discard, io.LimitReader(r.Body, 1<<20))
		}
	})
}

func cors(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.HasPrefix(r.URL.Path, "/api/") || strings.HasPrefix(r.URL.Path, "/v1/") {
			// Credential'siz ochiq kirish: tugunlar va tashqi monitoring uchun.
			// Admin cookie boshqa saytlarga yuborilmaydi (SameSite=Strict + X-Requested-With talabi).
			w.Header().Set("Access-Control-Allow-Origin", "*")
			w.Header().Set("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS")
			w.Header().Set("Access-Control-Allow-Headers", "Content-Type, Authorization")
			if r.Method == http.MethodOptions {
				w.WriteHeader(http.StatusNoContent)
				return
			}
		}
		next.ServeHTTP(w, r)
	})
}

func securityHeaders(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		h := w.Header()
		h.Set("X-Content-Type-Options", "nosniff")
		h.Set("Referrer-Policy", "no-referrer")
		h.Set("X-Frame-Options", "DENY")
		if !strings.HasPrefix(r.URL.Path, "/api/") {
			h.Set("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; font-src https://fonts.gstatic.com; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'")
		}
		next.ServeHTTP(w, r)
	})
}

// ---------------- Statik fayllar ----------------

func (s *Server) setupWeb() {
	dir := s.cfg.WebDir
	if dir == "" {
		for _, c := range []string{"web", filepath.Join("backend", "web")} {
			if fi, err := os.Stat(filepath.Join(c, "index.html")); err == nil && !fi.IsDir() {
				dir = c
				break
			}
		}
	}
	if dir != "" {
		abs, _ := filepath.Abs(dir)
		log.Printf("Statik veb papka: %s", abs)
		s.web = http.Dir(abs)
		if s.cfg.DownloadsDir == "" {
			s.cfg.DownloadsDir = filepath.Join(abs, "client")
		}
	} else {
		sub, _ := fs.Sub(embeddedWeb, "web")
		log.Printf("Statik veb: binarga joylangan nusxa")
		s.web = http.FS(sub)
	}
	if s.cfg.DownloadsDir == "" {
		s.cfg.DownloadsDir = filepath.Join(s.cfg.DataDir, "client")
	}
	s.downloadsDir, _ = filepath.Abs(s.cfg.DownloadsDir)
}

// Yuklab olinadigan mijoz dasturlari (veb sahifadagi tugmalar)
var clientDownloads = []struct {
	File, OS, Label string
}{
	{"sdrive-windows.exe", "windows", "Windows 10 / 11 (x64)"},
	{"sdrive-macos.dmg", "macos", "macOS (Intel / Apple Silicon)"},
	{"sdrive-linux.deb", "linux-deb", "Ubuntu / Debian (.deb)"},
	{"sdrive-linux.tar.gz", "linux", "Linux (x64, .tar.gz)"},
}

func (s *Server) handleDownloads(w http.ResponseWriter, r *http.Request) {
	type item struct {
		File      string `json:"file"`
		OS        string `json:"os"`
		Label     string `json:"label"`
		Available bool   `json:"available"`
		Size      int64  `json:"size"`
		Modified  int64  `json:"modified"`
		URL       string `json:"url"`
	}
	var out []item
	for _, d := range clientDownloads {
		it := item{File: d.File, OS: d.OS, Label: d.Label, URL: "/client/" + d.File}
		if fi, err := os.Stat(filepath.Join(s.downloadsDir, d.File)); err == nil && fi.Mode().IsRegular() {
			it.Available = true
			it.Size = fi.Size()
			it.Modified = fi.ModTime().Unix()
		}
		out = append(out, it)
	}
	writeJSON(w, http.StatusOK, map[string]any{"downloads": out})
}

func (s *Server) handleClientDownload(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	known := false
	for _, d := range clientDownloads {
		if d.File == name {
			known = true
		}
	}
	if !known {
		http.NotFound(w, r)
		return
	}
	f, err := os.Open(filepath.Join(s.downloadsDir, name))
	if err != nil {
		http.Error(w, "Bu platforma uchun dastur hali tayyorlanmagan (client/build skriptlarini ishga tushiring)", http.StatusNotFound)
		return
	}
	defer f.Close()
	fi, err := f.Stat()
	if err != nil || !fi.Mode().IsRegular() {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Disposition", `attachment; filename="`+name+`"`)
	http.ServeContent(w, r, name, fi.ModTime(), f)
}

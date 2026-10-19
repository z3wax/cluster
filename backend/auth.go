package main

import (
	"crypto/subtle"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

const (
	adminCookie     = "sdrive_admin"
	adminSessionTTL = 12 * time.Hour
)

// ---------------- Admin token ----------------

func (s *Server) loadAdminToken() error {
	if t := strings.TrimSpace(s.cfg.AdminToken); t != "" {
		s.adminToken = t
		return nil
	}
	path := filepath.Join(s.cfg.DataDir, "admin_token")
	if b, err := os.ReadFile(path); err == nil && len(strings.TrimSpace(string(b))) >= 16 {
		s.adminToken = strings.TrimSpace(string(b))
		log.Printf("Admin token: %s faylida", path)
		return nil
	}
	s.adminToken = randomHex(24)
	if err := os.WriteFile(path, []byte(s.adminToken+"\n"), 0o600); err != nil {
		return err
	}
	log.Printf("Yangi admin token yaratildi (%s): %s", path, s.adminToken)
	return nil
}

func bearer(r *http.Request) string {
	h := r.Header.Get("Authorization")
	if len(h) > 7 && strings.EqualFold(h[:7], "Bearer ") {
		return strings.TrimSpace(h[7:])
	}
	return ""
}

func tokenEqual(a, b string) bool {
	return len(a) > 0 && subtle.ConstantTimeCompare([]byte(a), []byte(b)) == 1
}

// ---------------- Admin sessiyalari ----------------

type sessionStore struct {
	mu sync.Mutex
	m  map[string]time.Time
}

func newSessionStore() *sessionStore { return &sessionStore{m: map[string]time.Time{}} }

func (ss *sessionStore) create() string {
	id := randomHex(32)
	ss.mu.Lock()
	defer ss.mu.Unlock()
	now := time.Now()
	for k, exp := range ss.m {
		if now.After(exp) {
			delete(ss.m, k)
		}
	}
	ss.m[id] = now.Add(adminSessionTTL)
	return id
}

func (ss *sessionStore) valid(id string) bool {
	if id == "" {
		return false
	}
	ss.mu.Lock()
	defer ss.mu.Unlock()
	exp, ok := ss.m[id]
	if !ok {
		return false
	}
	if time.Now().After(exp) {
		delete(ss.m, id)
		return false
	}
	return true
}

func (ss *sessionStore) drop(id string) {
	ss.mu.Lock()
	delete(ss.m, id)
	ss.mu.Unlock()
}

// isAdmin — Bearer admin token yoki amaldagi sessiya cookie.
// Cookie bilan kelgan o'zgartiruvchi so'rovlar X-Requested-With sarlavhasini talab qiladi (CSRF himoyasi).
func (s *Server) isAdmin(r *http.Request) (ok bool, csrfOK bool) {
	if tokenEqual(bearer(r), s.adminToken) {
		return true, true
	}
	c, err := r.Cookie(adminCookie)
	if err != nil || !s.sessions.valid(c.Value) {
		return false, false
	}
	if r.Method == http.MethodGet || r.Method == http.MethodHead {
		return true, true
	}
	return true, r.Header.Get("X-Requested-With") == "sdrive"
}

func (s *Server) admin(h http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		ok, csrf := s.isAdmin(r)
		if !ok {
			writeErr(w, http.StatusUnauthorized, "admin sifatida kiring")
			return
		}
		if !csrf {
			writeErr(w, http.StatusForbidden, "CSRF tekshiruvi o'tmadi")
			return
		}
		h(w, r)
	}
}

func (s *Server) handleLogin(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Token string `json:"token"`
	}
	if !readJSON(w, r, &req) {
		return
	}
	if !tokenEqual(strings.TrimSpace(req.Token), s.adminToken) {
		time.Sleep(400 * time.Millisecond) // taxmin qilishni sekinlashtirish
		writeErr(w, http.StatusUnauthorized, "token noto'g'ri")
		return
	}
	http.SetCookie(w, &http.Cookie{
		Name:     adminCookie,
		Value:    s.sessions.create(),
		Path:     "/",
		MaxAge:   int(adminSessionTTL.Seconds()),
		HttpOnly: true,
		Secure:   r.TLS != nil || r.Header.Get("X-Forwarded-Proto") == "https",
		SameSite: http.SameSiteStrictMode,
	})
	writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

func (s *Server) handleLogout(w http.ResponseWriter, r *http.Request) {
	if c, err := r.Cookie(adminCookie); err == nil {
		s.sessions.drop(c.Value)
	}
	http.SetCookie(w, &http.Cookie{Name: adminCookie, Value: "", Path: "/", MaxAge: -1, HttpOnly: true, SameSite: http.SameSiteStrictMode})
	writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

func (s *Server) handleMe(w http.ResponseWriter, r *http.Request) {
	ok, _ := s.isAdmin(r)
	writeJSON(w, http.StatusOK, map[string]any{"admin": ok})
}

// ---------------- Tugun autentifikatsiyasi ----------------

// authNodeLocked — db.mu ostida chaqiriladi. Yo'ldagi {id} va Bearer token mos kelsa tugunni qaytaradi,
// aks holda javob yozib nil qaytaradi.
func (s *Server) authNodeLocked(w http.ResponseWriter, r *http.Request) *Node {
	id := r.PathValue("id")
	n, ok := s.db.Nodes[id]
	if !ok {
		writeErr(w, http.StatusNotFound, "tugun topilmadi — avval ro'yxatdan o'ting")
		return nil
	}
	if n.Status != NodeActive {
		writeErr(w, http.StatusGone, "tugun o'chirilgan (uninstall)")
		return nil
	}
	hash, has := s.db.Tokens[id]
	tok := bearer(r)
	if !has || tok == "" || !tokenEqual(sha256Hex([]byte(tok)), hash) {
		writeErr(w, http.StatusUnauthorized, "tugun tokeni noto'g'ri")
		return nil
	}
	return n
}

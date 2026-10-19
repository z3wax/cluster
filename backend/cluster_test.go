package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"net"
	"net/http"
	"net/http/cookiejar"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

const testAdminToken = "test-admin-token-0123456789"

type fakeClock struct {
	mu sync.Mutex
	t  time.Time
}

func (c *fakeClock) Now() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.t
}

func (c *fakeClock) Advance(d time.Duration) {
	c.mu.Lock()
	c.t = c.t.Add(d)
	c.mu.Unlock()
}

func (c *fakeClock) Set(t time.Time) {
	c.mu.Lock()
	c.t = t
	c.mu.Unlock()
}

type env struct {
	t   *testing.T
	s   *Server
	ts  *httptest.Server
	clk *fakeClock
}

func testConfig(t *testing.T) Config {
	cfg := defaultConfig()
	cfg.DataDir = t.TempDir()
	cfg.AdminToken = testAdminToken
	cfg.AgreementPath = filepath.Join("..", "SHARTNOMA.md")
	cfg.WebDir = "web"
	cfg.ChunkSize = 1024
	cfg.HeartbeatInterval = time.Second
	cfg.OfflineTimeout = 3 * time.Second
	cfg.RepairDelay = 10 * time.Second
	cfg.ReconcileInterval = time.Hour
	cfg.RateCentsPerGBHour = 1e9 // kichik test fayllarida ham sezilarli daromad
	return cfg
}

func newEnv(t *testing.T, mod func(*Config)) *env {
	t.Helper()
	cfg := testConfig(t)
	if mod != nil {
		mod(&cfg)
	}
	return startEnv(t, cfg)
}

func startEnv(t *testing.T, cfg Config) *env {
	t.Helper()
	s, err := NewServer(cfg)
	if err != nil {
		t.Fatal(err)
	}
	clk := &fakeClock{t: time.Date(2026, 10, 15, 12, 0, 0, 0, time.UTC)}
	s.clock = clk.Now
	ts := httptest.NewServer(s.Handler())
	s.baseURL = ts.URL
	t.Cleanup(func() {
		s.sim.stopAll()
		ts.Close()
		s.Close()
	})
	return &env{t: t, s: s, ts: ts, clk: clk}
}

func (e *env) node(id string, spaceGB int64) *nodeClient {
	e.t.Helper()
	c := &nodeClient{
		base:             e.ts.URL,
		hc:               e.ts.Client(),
		ID:               id,
		Dir:              filepath.Join(e.t.TempDir(), id),
		Name:             "PC " + id,
		OS:               "Linux",
		SpaceTotal:       spaceGB << 30,
		agreementVersion: e.s.agreementVersion,
	}
	if err := c.register(); err != nil {
		e.t.Fatal(err)
	}
	return c
}

func (e *env) nodes(n int, spaceGB int64) []*nodeClient {
	var out []*nodeClient
	for i := 0; i < n; i++ {
		out = append(out, e.node(fmt.Sprintf("node-%03d", i), spaceGB))
	}
	return out
}

func (e *env) tick(nodes ...*nodeClient) {
	e.t.Helper()
	for _, c := range nodes {
		if err := c.tick(); err != nil {
			e.t.Fatalf("%s tick: %v", c.ID, err)
		}
	}
}

func (e *env) admin(method, path string, body io.Reader) *http.Response {
	e.t.Helper()
	req, _ := http.NewRequest(method, e.ts.URL+path, body)
	req.Header.Set("Authorization", "Bearer "+testAdminToken)
	resp, err := e.ts.Client().Do(req)
	if err != nil {
		e.t.Fatal(err)
	}
	return resp
}

func decode(t *testing.T, resp *http.Response, v any) {
	t.Helper()
	defer resp.Body.Close()
	if err := json.NewDecoder(resp.Body).Decode(v); err != nil {
		t.Fatal(err)
	}
}

func (e *env) upload(name string, data []byte) string {
	e.t.Helper()
	resp := e.admin("POST", "/api/files/upload?name="+url.QueryEscape(name)+"&size="+strconv.Itoa(len(data)), bytes.NewReader(data))
	var r struct {
		OK     bool   `json:"ok"`
		FileID string `json:"fileId"`
		Error  string `json:"error"`
	}
	decode(e.t, resp, &r)
	if !r.OK {
		e.t.Fatalf("upload: %s", r.Error)
	}
	return r.FileID
}

func (e *env) file(id string) File {
	e.s.db.mu.Lock()
	defer e.s.db.mu.Unlock()
	f := e.s.db.Files[id]
	if f == nil {
		return File{}
	}
	return *f
}

func (e *env) holders(fileID string) []string {
	e.s.db.mu.Lock()
	defer e.s.db.mu.Unlock()
	var out []string
	for _, n := range e.s.db.Nodes {
		if contains(n.StoredFiles, fileID) {
			out = append(out, n.ID)
		}
	}
	return out
}

func (e *env) nodeState(id string) Node {
	e.s.db.mu.Lock()
	defer e.s.db.mu.Unlock()
	return *e.s.db.Nodes[id]
}

func (e *env) pending(kind string) []*Task {
	e.s.db.mu.Lock()
	defer e.s.db.mu.Unlock()
	var out []*Task
	for _, t := range e.s.db.Tasks {
		if t.Status == TaskPending && (kind == "" || t.Kind == kind) {
			cp := *t
			out = append(out, &cp)
		}
	}
	return out
}

func testData(n int) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = byte(i*7 + i/251)
	}
	return b
}

// ---------------- Testlar ----------------

func TestDesiredFor(t *testing.T) {
	cases := map[int]int{0: 0, 1: 1, 2: 1, 3: 1, 4: 2, 10: 3, 11: 4, 100: 30, 1000: 300}
	for n, want := range cases {
		if got := desiredFor(n, 0.30); got != want {
			t.Errorf("desiredFor(%d) = %d, kutilgan %d", n, got, want)
		}
	}
}

func TestAgreementAndInfo(t *testing.T) {
	e := newEnv(t, nil)
	resp, _ := http.Get(e.ts.URL + "/api/agreement")
	var a struct{ Version, Text string }
	decode(t, resp, &a)
	if a.Version != "1.0" || !strings.Contains(a.Text, "SHARTNOMA") {
		t.Fatalf("shartnoma: %q", a.Version)
	}
	resp, _ = http.Get(e.ts.URL + "/api/info")
	var info map[string]any
	decode(t, resp, &info)
	if info["replicaRatio"].(float64) != 0.30 {
		t.Fatalf("info: %v", info)
	}
	resp, _ = http.Get(e.ts.URL + "/")
	body, _ := io.ReadAll(resp.Body)
	resp.Body.Close()
	if resp.StatusCode != 200 || !strings.Contains(string(body), "SDrive") {
		t.Fatalf("index.html: %d", resp.StatusCode)
	}
}

func TestRegisterAndNodeAuth(t *testing.T) {
	e := newEnv(t, nil)
	post := func(body map[string]any, token string) int {
		b, _ := json.Marshal(body)
		req, _ := http.NewRequest("POST", e.ts.URL+"/api/nodes/register", bytes.NewReader(b))
		if token != "" {
			req.Header.Set("Authorization", "Bearer "+token)
		}
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		resp.Body.Close()
		return resp.StatusCode
	}
	base := map[string]any{"id": "pc-alpha", "spaceTotal": 1 << 30, "agreementVersion": "1.0"}
	if c := post(base, ""); c != http.StatusPreconditionFailed {
		t.Fatalf("shartnomasiz ro'yxatdan o'tish: %d", c)
	}
	base["agreementAccepted"] = true
	base["agreementVersion"] = "0.9"
	if c := post(base, ""); c != http.StatusPreconditionFailed {
		t.Fatalf("eski shartnoma versiyasi: %d", c)
	}
	base["id"] = "x"
	base["agreementVersion"] = "1.0"
	if c := post(base, ""); c != http.StatusBadRequest {
		t.Fatalf("noto'g'ri ID: %d", c)
	}

	n := e.node("pc-alpha", 10)
	if len(n.Token) != 64 {
		t.Fatalf("token: %q", n.Token)
	}
	base["id"] = "pc-alpha"
	if c := post(base, ""); c != http.StatusConflict {
		t.Fatalf("tokensiz ID egallash: %d", c)
	}
	if c := post(base, n.Token); c != http.StatusOK {
		t.Fatalf("token bilan qayta ro'yxatdan o'tish: %d", c)
	}

	_, code, _ := n.heartbeat()
	if code != 200 {
		t.Fatalf("heartbeat: %d", code)
	}
	bad := *n
	bad.Token = strings.Repeat("0", 64)
	if _, code, _ := bad.heartbeat(); code != http.StatusUnauthorized {
		t.Fatalf("noto'g'ri token: %d", code)
	}
	other := e.node("pc-beta", 10)
	bad = *other
	bad.ID = "pc-alpha"
	if _, code, _ := bad.heartbeat(); code != http.StatusUnauthorized {
		t.Fatalf("boshqa tugun tokeni: %d", code)
	}
}

func TestAdminAuth(t *testing.T) {
	e := newEnv(t, nil)
	resp, _ := http.Get(e.ts.URL + "/api/dashboard")
	resp.Body.Close()
	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("auth'siz dashboard: %d", resp.StatusCode)
	}
	jar, _ := cookiejar.New(nil)
	hc := &http.Client{Jar: jar}
	resp, _ = hc.Post(e.ts.URL+"/api/admin/login", "application/json", strings.NewReader(`{"token":"wrong-token"}`))
	resp.Body.Close()
	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("noto'g'ri login: %d", resp.StatusCode)
	}
	resp, _ = hc.Post(e.ts.URL+"/api/admin/login", "application/json", strings.NewReader(`{"token":"`+testAdminToken+`"}`))
	resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("login: %d", resp.StatusCode)
	}
	resp, _ = hc.Get(e.ts.URL + "/api/dashboard")
	resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("cookie bilan dashboard: %d", resp.StatusCode)
	}
	// CSRF: cookie bilan POST X-Requested-With siz rad etiladi
	resp, _ = hc.Post(e.ts.URL+"/api/files/upload?name=a.txt", "application/octet-stream", strings.NewReader("hello"))
	resp.Body.Close()
	if resp.StatusCode != http.StatusForbidden {
		t.Fatalf("CSRF himoyasi: %d", resp.StatusCode)
	}
	req, _ := http.NewRequest("POST", e.ts.URL+"/api/files/upload?name=a.txt", strings.NewReader("hello"))
	req.Header.Set("X-Requested-With", "sdrive")
	resp, _ = hc.Do(req)
	resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("cookie + header bilan upload: %d", resp.StatusCode)
	}
	resp, _ = hc.Post(e.ts.URL+"/api/admin/logout", "application/json", nil)
	resp.Body.Close()
	resp, _ = hc.Get(e.ts.URL + "/api/dashboard")
	resp.Body.Close()
	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("logout'dan keyin: %d", resp.StatusCode)
	}
}

func TestUploadValidation(t *testing.T) {
	e := newEnv(t, func(c *Config) { c.MaxUploadBytes = 4096 })
	resp := e.admin("POST", "/api/files/upload?name=bo'sh.txt", bytes.NewReader(nil))
	resp.Body.Close()
	if resp.StatusCode != http.StatusBadRequest {
		t.Fatalf("bo'sh fayl: %d", resp.StatusCode)
	}
	resp = e.admin("POST", "/api/files/upload?name=a.bin&size=10", bytes.NewReader(testData(9)))
	resp.Body.Close()
	if resp.StatusCode != http.StatusBadRequest {
		t.Fatalf("hajm mos emas: %d", resp.StatusCode)
	}
	resp = e.admin("POST", "/api/files/upload?name=big.bin", bytes.NewReader(testData(5000)))
	resp.Body.Close()
	if resp.StatusCode != http.StatusRequestEntityTooLarge {
		t.Fatalf("katta fayl: %d", resp.StatusCode)
	}
	id := e.upload("../../etc/passwd", testData(100))
	if f := e.file(id); f.Name != "passwd" {
		t.Fatalf("nom tozalanmadi: %q", f.Name)
	}
	entries, _ := os.ReadDir(e.s.chunksDir)
	if len(entries) != 1 {
		t.Fatalf("muvaffaqiyatsiz yuklashlar chunk qoldirdi: %d papka", len(entries))
	}
}

func TestReplicationThirtyPercent(t *testing.T) {
	e := newEnv(t, nil)
	nodes := e.nodes(10, 100)
	data := testData(3500) // 4 chunk (1024 bayt)
	fid := e.upload("hujjat.pdf", data)

	f := e.file(fid)
	if len(f.Chunks) != 4 || f.ReplicaTarget != 3 {
		t.Fatalf("chunks=%d target=%d", len(f.Chunks), f.ReplicaTarget)
	}
	tasks := e.pending(KindReplica)
	if len(tasks) != 3 {
		t.Fatalf("30%% dan 3 ta vazifa kutilgan, %d", len(tasks))
	}
	seen := map[string]bool{}
	for _, tk := range tasks {
		if seen[tk.NodeID] {
			t.Fatalf("bitta tugunga ikki nusxa: %s", tk.NodeID)
		}
		seen[tk.NodeID] = true
	}

	e.tick(nodes...) // yuklab olish + done
	e.tick(nodes...) // audit javoblari
	e.s.reconcile()
	f = e.file(fid)
	if f.Status != FileHealthy || f.ReplicasDone != 3 {
		t.Fatalf("holat=%s nusxa=%d", f.Status, f.ReplicasDone)
	}
	h := e.holders(fid)
	if len(h) != 3 {
		t.Fatalf("holders: %v", h)
	}
	for _, c := range nodes {
		if !contains(h, c.ID) {
			continue
		}
		var got []byte
		for i := 0; i < 4; i++ {
			b, err := os.ReadFile(c.chunkFile(fid, i))
			if err != nil {
				t.Fatal(err)
			}
			got = append(got, b...)
		}
		if !bytes.Equal(got, data) {
			t.Fatalf("%s dagi nusxa asl fayl bilan mos emas", c.ID)
		}
		if n := e.nodeState(c.ID); n.AuditsPassed != 1 || n.AuditsFailed != 0 {
			t.Fatalf("%s audit: +%d -%d", c.ID, n.AuditsPassed, n.AuditsFailed)
		}
	}
	if len(e.pending("")) != 0 {
		t.Fatalf("vazifalar qoldi: %d", len(e.pending("")))
	}
}

func TestReplicationScales(t *testing.T) {
	e := newEnv(t, nil)
	e.nodes(100, 50)
	e.upload("a.bin", testData(2000))
	if n := len(e.pending(KindReplica)); n != 30 {
		t.Fatalf("100 tugun: 30 nusxa kutilgan, %d", n)
	}
	// klaster o'sganda nusxalar soni ham ortadi
	for i := 100; i < 110; i++ {
		e.node(fmt.Sprintf("node-%03d", i), 50)
	}
	e.s.reconcile()
	if n := len(e.pending(KindReplica)); n != 33 {
		t.Fatalf("110 tugun: 33 nusxa kutilgan, %d", n)
	}
}

func TestCapacityRespected(t *testing.T) {
	e := newEnv(t, func(c *Config) { c.ChunkSize = 1 << 20 })
	big := e.node("big-0001", 1)
	tiny := &nodeClient{base: e.ts.URL, hc: e.ts.Client(), ID: "tiny-001", Dir: t.TempDir(), SpaceTotal: 1000, agreementVersion: "1.0"}
	if err := tiny.register(); err != nil {
		t.Fatal(err)
	}
	e.upload("x.bin", testData(5000))
	for _, tk := range e.pending(KindReplica) {
		if tk.NodeID == "tiny-001" {
			t.Fatal("joyi yetmaydigan tugunga vazifa berildi")
		}
	}
	if len(e.pending(KindReplica)) != 1 || e.pending(KindReplica)[0].NodeID != big.ID {
		t.Fatalf("vazifa katta tugunga berilishi kerak edi")
	}
}

func TestEarningsOnlyForVerifiedOnlineTime(t *testing.T) {
	e := newEnv(t, nil)
	nodes := e.nodes(1, 10)
	n := nodes[0]
	data := testData(4096)
	e.upload("pul.bin", data)
	e.tick(n) // saqlaydi
	e.tick(n) // audit
	before := e.nodeState(n.ID)
	if len(before.StoredFiles) != 1 {
		t.Fatal("fayl saqlanmadi")
	}
	e.clk.Advance(2 * time.Second)
	e.tick(n)
	after := e.nodeState(n.ID)
	want := float64(len(data)) / GB * (2.0 / 3600.0) * e.s.cfg.RateCentsPerGBHour
	if got := after.EarningsCents - before.EarningsCents; math.Abs(got-want) > want*1e-9 {
		t.Fatalf("2 soniya uchun daromad %v, kutilgan %v", got, want)
	}
	if after.VerifiedUptimeSec-before.VerifiedUptimeSec != 2 {
		t.Fatalf("uptime: %d", after.VerifiedUptimeSec-before.VerifiedUptimeSec)
	}
	// internet uzildi (heartbeat oralig'i > OfflineTimeout) — bu vaqt uchun to'lanmaydi
	e.clk.Advance(30 * time.Second)
	e.tick(n)
	gap := e.nodeState(n.ID)
	if gap.EarningsCents != after.EarningsCents || gap.VerifiedUptimeSec != after.VerifiedUptimeSec {
		t.Fatalf("uzilish vaqti hisoblandi: %v -> %v", after.EarningsCents, gap.EarningsCents)
	}
}

func TestOfflineNodeReplicaMovedAndTrimmed(t *testing.T) {
	e := newEnv(t, nil)
	nodes := e.nodes(10, 100)
	fid := e.upload("video.mp4", testData(3000))
	e.tick(nodes...)
	e.tick(nodes...)
	byID := map[string]*nodeClient{}
	for _, c := range nodes {
		byID[c.ID] = c
	}
	h := e.holders(fid)
	lost := h[0]
	var others []*nodeClient
	for _, c := range nodes {
		if c.ID != lost {
			others = append(others, c)
		}
	}

	// 1) qisqa uzilish: oflayn, lekin RepairDelay ichida — yangi nusxa yaratilmaydi
	e.clk.Advance(5 * time.Second)
	e.tick(others...)
	e.s.reconcile()
	if e.nodeState(lost).Online {
		t.Fatal("heartbeat yubormagan tugun oflayn bo'lishi kerak")
	}
	if len(e.pending(KindReplica)) != 0 {
		t.Fatal("RepairDelay tugamasdan nusxa ko'chirildi")
	}

	// 2) uzoq uzilish — nusxa boshqa tugunga ko'chiriladi
	e.clk.Advance(6 * time.Second)
	e.tick(others...)
	e.s.reconcile()
	rep := e.pending(KindReplica)
	if len(rep) != 1 || rep[0].NodeID == lost || contains(h, rep[0].NodeID) {
		t.Fatalf("yangi nusxa vazifasi noto'g'ri: %+v", rep)
	}
	e.tick(others...)
	e.s.reconcile()
	if f := e.file(fid); f.Status != FileHealthy || f.ReplicasDone != 3 {
		t.Fatalf("tiklangandan keyin: %s %d", f.Status, f.ReplicasDone)
	}

	// 3) tugun qaytdi — ortiqcha (4-) nusxa olib tashlanadi
	e.tick(byID[lost])
	e.s.reconcile()
	del := e.pending(KindDelete)
	if len(del) != 1 {
		t.Fatalf("ortiqcha nusxa uchun delete vazifasi kutilgan, %d", len(del))
	}
	if n := len(e.holders(fid)); n != 3 {
		t.Fatalf("holders: %d", n)
	}
	victim := byID[del[0].NodeID]
	e.tick(victim)
	if _, err := os.Stat(filepath.Join(victim.Dir, fid)); !os.IsNotExist(err) {
		t.Fatal("delete vazifasidan keyin lokal nusxa qoldi")
	}
	e.s.reconcile()
	if f := e.file(fid); f.Status != FileHealthy || f.ReplicasDone != 3 {
		t.Fatalf("yakuniy holat: %s %d", f.Status, f.ReplicasDone)
	}
}

func TestAuditFailureStopsPaymentAndRepairs(t *testing.T) {
	e := newEnv(t, nil)
	n := e.nodes(1, 10)[0]
	fid := e.upload("x.bin", testData(2500))
	e.tick(n) // saqladi
	// foydalanuvchi faylni buzdi
	for i := 0; i < 3; i++ {
		os.WriteFile(n.chunkFile(fid, i), bytes.Repeat([]byte{0xFF}, 1024), 0o644)
	}
	e.tick(n) // audit -> muvaffaqiyatsiz
	st := e.nodeState(n.ID)
	if st.AuditsFailed != 1 || contains(st.StoredFiles, fid) {
		t.Fatalf("audit natijasi: failed=%d stored=%v", st.AuditsFailed, st.StoredFiles)
	}
	if rep := e.pending(KindRepair); len(rep) != 1 || rep[0].NodeID != n.ID {
		t.Fatalf("repair vazifasi kutilgan: %+v", rep)
	}
	// tasdiqlanmagan fayl uchun to'lanmaydi (5.3)
	earn := st.EarningsCents
	e.clk.Advance(time.Second)
	e.tick(n) // repair: qayta yuklab oladi
	if e.nodeState(n.ID).EarningsCents != earn {
		t.Fatal("tasdiqlanmagan fayl uchun daromad yozildi")
	}
	if !contains(e.nodeState(n.ID).StoredFiles, fid) {
		t.Fatal("repair'dan keyin fayl tiklanmadi")
	}
	e.tick(n) // yangi audit -> o'tadi
	if st := e.nodeState(n.ID); st.AuditsPassed != 1 {
		t.Fatalf("tiklangandan keyingi audit: +%d -%d", st.AuditsPassed, st.AuditsFailed)
	}
}

func TestUserDeletedFileIsRedownloaded(t *testing.T) {
	e := newEnv(t, nil)
	n := e.nodes(1, 10)[0]
	fid := e.upload("x.bin", testData(1500))
	e.tick(n)
	os.RemoveAll(filepath.Join(n.Dir, fid))
	var r struct {
		TasksCreated int `json:"tasksCreated"`
	}
	if code, _ := n.postJSON("/api/nodes/"+n.ID+"/repair", map[string]any{"fileId": fid}, &r); code != 200 || r.TasksCreated != 1 {
		t.Fatalf("repair: %d %d", code, r.TasksCreated)
	}
	if contains(e.nodeState(n.ID).StoredFiles, fid) {
		t.Fatal("qayta yuklanguncha fayl hisobdan chiqarilishi kerak (3.3)")
	}
	e.tick(n)
	if !contains(e.nodeState(n.ID).StoredFiles, fid) {
		t.Fatal("fayl qayta yuklanmadi")
	}
	if _, err := os.Stat(n.chunkFile(fid, 1)); err != nil {
		t.Fatal("chunk diskda yo'q")
	}
}

func TestServerCopyRestoredFromNodes(t *testing.T) {
	e := newEnv(t, nil)
	nodes := e.nodes(4, 10)
	data := testData(3000)
	fid := e.upload("arxiv.zip", data)
	e.tick(nodes...)
	e.tick(nodes...)

	// server diskidagi chunk yo'qoldi, ikkinchisi buzildi
	os.Remove(e.s.chunkPath(fid, 0))
	os.WriteFile(e.s.chunkPath(fid, 2), bytes.Repeat([]byte{1}, 952), 0o644)
	e.s.scrubFile(fid, true)
	if f := e.file(fid); len(f.Missing) != 2 {
		t.Fatalf("scrub topgan yo'qolgan chunk'lar: %v", f.Missing)
	}
	resp := e.admin("GET", "/api/files/"+fid+"/download", nil)
	resp.Body.Close()
	if resp.StatusCode != http.StatusServiceUnavailable {
		t.Fatalf("to'liq bo'lmagan fayl yuklab berildi: %d", resp.StatusCode)
	}
	e.s.reconcile()
	if f := e.file(fid); f.Status != FileRestoring {
		t.Fatalf("holat: %s", f.Status)
	}
	if r := e.pending(KindRestore); len(r) != 1 || len(r[0].Chunks) != 2 {
		t.Fatalf("restore vazifasi: %+v", r)
	}
	e.tick(nodes...)
	e.s.reconcile()
	f := e.file(fid)
	if len(f.Missing) != 0 || f.Status != FileHealthy {
		t.Fatalf("tiklanmadi: missing=%v status=%s", f.Missing, f.Status)
	}
	resp = e.admin("GET", "/api/files/"+fid+"/download", nil)
	got, _ := io.ReadAll(resp.Body)
	resp.Body.Close()
	if resp.StatusCode != 200 || !bytes.Equal(got, data) {
		t.Fatalf("tiklangan fayl mos emas: %d, %d bayt", resp.StatusCode, len(got))
	}
	sum := sha256.Sum256(data)
	if resp.Header.Get("X-Content-SHA256") != hex.EncodeToString(sum[:]) {
		t.Fatal("X-Content-SHA256 noto'g'ri")
	}
}

func TestDownloadHeaders(t *testing.T) {
	e := newEnv(t, nil)
	fid := e.upload("Hisobot 2026 — yakuniy.pdf", testData(10))
	resp := e.admin("GET", "/api/files/"+fid+"/download", nil)
	resp.Body.Close()
	cd := resp.Header.Get("Content-Disposition")
	if !strings.HasPrefix(cd, "attachment") || !strings.Contains(cd, "filename*=utf-8''Hisobot") {
		t.Fatalf("Content-Disposition: %q", cd)
	}
	if resp.Header.Get("Content-Length") != "10" {
		t.Fatalf("Content-Length: %q", resp.Header.Get("Content-Length"))
	}
}

func TestChunkAccessRequiresTask(t *testing.T) {
	e := newEnv(t, nil)
	nodes := e.nodes(4, 10) // 30% -> 2 nusxa
	fid := e.upload("maxfiy.txt", testData(500))
	assigned := map[string]bool{}
	for _, tk := range e.pending(KindReplica) {
		assigned[tk.NodeID] = true
	}
	for _, c := range nodes {
		code, _ := c.do("GET", "/api/nodes/"+c.ID+"/files/"+fid+"/chunks/0", nil, "", nil)
		if assigned[c.ID] && code != 200 {
			t.Fatalf("vazifasi bor tugun chunk ololmadi: %d", code)
		}
		if !assigned[c.ID] && code != http.StatusForbidden {
			t.Fatalf("vazifasiz tugun chunk oldi: %d", code)
		}
	}
	resp, _ := http.Get(e.ts.URL + "/api/files/" + fid + "/download")
	resp.Body.Close()
	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("anonim yuklab olish: %d", resp.StatusCode)
	}
}

func TestDeleteFile(t *testing.T) {
	e := newEnv(t, nil)
	nodes := e.nodes(4, 10)
	fid := e.upload("eski.log", testData(1200))
	e.tick(nodes...)
	h := e.holders(fid)
	resp := e.admin("DELETE", "/api/files/"+fid, nil)
	resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("delete: %d", resp.StatusCode)
	}
	if len(e.pending(KindDelete)) != len(h) {
		t.Fatalf("delete vazifalari: %d, holders %d", len(e.pending(KindDelete)), len(h))
	}
	e.tick(nodes...)
	for _, c := range nodes {
		if _, err := os.Stat(filepath.Join(c.Dir, fid)); !os.IsNotExist(err) {
			t.Fatalf("%s da nusxa qoldi", c.ID)
		}
	}
	if _, err := os.Stat(filepath.Join(e.s.chunksDir, fid)); !os.IsNotExist(err) {
		t.Fatal("server chunk'lari o'chirilmadi")
	}
	if len(e.pending("")) != 0 {
		t.Fatal("vazifalar qoldi")
	}
}

func TestUninstallForfeitsAndRereplicates(t *testing.T) {
	e := newEnv(t, nil)
	nodes := e.nodes(10, 10)
	fid := e.upload("x.bin", testData(2000))
	e.tick(nodes...)
	e.tick(nodes...)
	e.clk.Advance(time.Second)
	e.tick(nodes...)
	victim := e.holders(fid)[0]
	var vc *nodeClient
	for _, c := range nodes {
		if c.ID == victim {
			vc = c
		}
	}
	earned := e.nodeState(victim).EarningsCents
	if earned <= 0 {
		t.Fatal("daromad yozilmadi")
	}
	var r struct {
		Forfeited float64 `json:"forfeitedCents"`
	}
	if code, _ := vc.postJSON("/api/nodes/"+victim+"/uninstall", map[string]any{}, &r); code != 200 {
		t.Fatalf("uninstall: %d", code)
	}
	if r.Forfeited != earned {
		t.Fatalf("yo'qotilgan summa %v, kutilgan %v", r.Forfeited, earned)
	}
	e.s.db.mu.Lock()
	var forfeited *Payout
	for _, p := range e.s.db.Payouts {
		if p.NodeID == victim {
			forfeited = p
		}
	}
	e.s.db.mu.Unlock()
	if forfeited == nil || forfeited.Status != "forfeited" || forfeited.AmountCents != earned {
		t.Fatalf("forfeited payout: %+v", forfeited)
	}
	if _, code, _ := vc.heartbeat(); code != http.StatusGone {
		t.Fatalf("o'chirilgan tugun heartbeat: %d", code)
	}
	if err := vc.register(); err == nil {
		t.Fatal("o'chirilgan ID qayta ro'yxatdan o'tdi")
	}
	e.s.reconcile()
	rep := e.pending(KindReplica)
	if len(rep) != 1 || rep[0].NodeID == victim {
		t.Fatalf("o'chirilgan tugundagi nusxa ko'chirilmadi: %+v", rep)
	}
}

func TestPayoutWindow(t *testing.T) {
	e := newEnv(t, nil)
	n := e.nodes(1, 10)[0]
	e.upload("x.bin", testData(2000))
	e.tick(n)
	e.tick(n)
	e.clk.Advance(time.Second)
	e.tick(n)
	var r struct {
		OK     bool    `json:"ok"`
		Error  string  `json:"error"`
		Payout *Payout `json:"payout"`
	}
	if code, _ := n.postJSON("/api/nodes/"+n.ID+"/payout", map[string]any{}, &r); code != http.StatusForbidden {
		t.Fatalf("oy o'rtasida payout: %d", code)
	}
	earned := e.nodeState(n.ID).EarningsCents
	e.clk.Set(time.Date(2026, 10, 29, 9, 0, 0, 0, time.UTC)) // oktyabrning oxirgi 3 kuni
	r.OK = false
	if code, _ := n.postJSON("/api/nodes/"+n.ID+"/payout", map[string]any{}, &r); code != 200 || !r.OK {
		t.Fatalf("oy oxirida payout: %d %s", code, r.Error)
	}
	if r.Payout.AmountCents != earned || r.Payout.Period != "2026-10" || r.Payout.Status != "paid" {
		t.Fatalf("payout: %+v", r.Payout)
	}
	if e.nodeState(n.ID).EarningsCents != 0 {
		t.Fatal("payout'dan keyin balans nollanmadi")
	}
	if code, _ := n.postJSON("/api/nodes/"+n.ID+"/payout", map[string]any{}, &r); code != http.StatusBadRequest {
		t.Fatalf("bo'sh balans: %d", code)
	}
}

func TestPersistenceAcrossRestart(t *testing.T) {
	cfg := testConfig(t)
	e := startEnv(t, cfg)
	nodes := e.nodes(3, 10)
	data := testData(2048)
	fid := e.upload("doimiy.bin", data)
	e.tick(nodes...)
	if err := e.s.db.Flush(); err != nil {
		t.Fatal(err)
	}

	e2 := startEnv(t, cfg)
	for _, c := range nodes {
		c.base = e2.ts.URL
		c.hc = e2.ts.Client()
	}
	if _, code, _ := nodes[0].heartbeat(); code != 200 {
		t.Fatalf("qayta ishga tushgandan keyin token ishlamadi: %d", code)
	}
	f := e2.file(fid)
	if f.Size != 2048 || f.SHA256 == "" || len(e2.holders(fid)) != 1 {
		t.Fatalf("holat tiklanmadi: %+v holders=%v", f, e2.holders(fid))
	}
	resp := e2.admin("GET", "/api/files/"+fid+"/download", nil)
	got, _ := io.ReadAll(resp.Body)
	resp.Body.Close()
	if !bytes.Equal(got, data) {
		t.Fatal("qayta ishga tushgandan keyin fayl mos emas")
	}
}

func TestLegacyDBMigration(t *testing.T) {
	cfg := testConfig(t)
	dir := filepath.Join(cfg.DataDir, "chunks", "f_00112233aabbccdd")
	os.MkdirAll(dir, 0o755)
	os.WriteFile(filepath.Join(dir, "0.bin"), []byte("hello "), 0o644)
	os.WriteFile(filepath.Join(dir, "1.bin"), []byte("world"), 0o644)
	legacy := `{"nodes":{"node1":{"id":"node1","name":"PC-1","spaceTotal":1073741824,"online":false,"agreementAccepted":true,"agreementVersion":"1.0","status":"active","storedFiles":null}},
	"files":{"f_00112233aabbccdd":{"id":"f_00112233aabbccdd","name":"a.txt","size":11,"chunkSize":4194304,"chunks":[{"index":0,"size":6},{"index":1,"size":5}],"status":"active"}},
	"tasks":{},"payouts":{},"seq":0}`
	os.WriteFile(filepath.Join(cfg.DataDir, "db.json"), []byte(legacy), 0o644)
	e := startEnv(t, cfg)
	f := e.file("f_00112233aabbccdd")
	sum := sha256.Sum256([]byte("hello world"))
	if f.SHA256 != hex.EncodeToString(sum[:]) || f.Chunks[1].SHA256 == "" {
		t.Fatalf("migratsiya: %+v", f)
	}
	// eski tugun (tokensiz) birinchi ro'yxatdan o'tishda token oladi
	c := &nodeClient{base: e.ts.URL, hc: e.ts.Client(), ID: "node1", Dir: t.TempDir(), SpaceTotal: 1 << 30, agreementVersion: "1.0"}
	if err := c.register(); err != nil || c.Token == "" {
		t.Fatalf("eski tugun: %v", err)
	}
}

func TestSimulatorEndToEnd(t *testing.T) {
	if testing.Short() {
		t.Skip("real vaqtli simulyator testi")
	}
	e := newEnv(t, func(c *Config) {
		c.HeartbeatInterval = 200 * time.Millisecond
		c.OfflineTimeout = time.Second
		c.RepairDelay = 2 * time.Second
	})
	e.s.clock = time.Now
	waitFor := func(what string, cond func() bool) {
		t.Helper()
		deadline := time.Now().Add(15 * time.Second)
		for time.Now().Before(deadline) {
			e.s.reconcile()
			if cond() {
				return
			}
			time.Sleep(100 * time.Millisecond)
		}
		t.Fatalf("kutish vaqti tugadi: %s", what)
	}
	resp := e.admin("POST", "/api/admin/sim/start", strings.NewReader(`{"count":10}`))
	resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("sim start: %d", resp.StatusCode)
	}
	fid := e.upload("demo.bin", testData(5000))
	waitFor("10 tugunda 3 nusxa", func() bool { f := e.file(fid); return f.Status == FileHealthy && len(e.holders(fid)) == 3 })

	// faqat 3 ta tugun qoladi — nusxalar deyarli har doim ularga ko'chirilishi kerak bo'ladi
	resp = e.admin("POST", "/api/admin/sim/offline", strings.NewReader(`{"count":7}`))
	resp.Body.Close()
	counts := func() (online, onlineHolders int) {
		e.s.db.mu.Lock()
		defer e.s.db.mu.Unlock()
		for _, n := range e.s.db.Nodes {
			if n.Online {
				online++
				if contains(n.StoredFiles, fid) {
					onlineHolders++
				}
			}
		}
		return
	}
	waitFor("to'xtatilgan tugunlar oflayn deb belgilanishi", func() bool { o, _ := counts(); return o == 3 })
	waitFor("oflayn tugunlardan keyin o'z-o'zini tiklash", func() bool {
		_, h := counts()
		return h == 3 && e.file(fid).Status == FileHealthy
	})

	resp = e.admin("POST", "/api/admin/sim/remove", nil)
	resp.Body.Close()
	e.s.db.mu.Lock()
	left := len(e.s.db.Nodes)
	e.s.db.mu.Unlock()
	if left != 0 {
		t.Fatalf("simulyator tugunlari o'chirilmadi: %d", left)
	}
}

// Windows mijozlari uchun: handler o'qimagan so'rov tanasi kernel buferida qolsa, Go ulanishni
// "Connection: close" da RST bilan yopadi va Windows allaqachon kelgan javobni yo'qotadi (WSAECONNRESET).
// drainBody tanani o'qib tashlaydi, ulanish oddiy FIN bilan yopiladi.
func TestUnreadBodyDoesNotResetConnection(t *testing.T) {
	check := func(h http.Handler) error {
		ts := httptest.NewServer(h)
		defer ts.Close()
		conn, err := net.Dial("tcp", strings.TrimPrefix(ts.URL, "http://"))
		if err != nil {
			return err
		}
		defer conn.Close()
		conn.Write([]byte("POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\nConnection: close\r\n\r\n"))
		time.Sleep(30 * time.Millisecond) // tana handler ishlayotganda keladi
		conn.Write([]byte("{}"))
		conn.SetReadDeadline(time.Now().Add(5 * time.Second))
		b, err := io.ReadAll(conn)
		if err == nil && !strings.HasPrefix(string(b), "HTTP/1.1 200") {
			err = fmt.Errorf("javob: %q", string(b))
		}
		return err
	}
	slow := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		time.Sleep(150 * time.Millisecond) // tanani o'qimaydi (masalan, payout/uninstall)
		writeJSON(w, http.StatusOK, map[string]any{"ok": true})
	})
	if err := check(slow); err == nil {
		t.Log("diqqat: bu muhitda RST qayta hosil bo'lmadi (drainBody'siz ham)")
	} else {
		t.Logf("drainBody'siz: %v (kutilgan xato)", err)
	}
	if err := check(drainBody(slow)); err != nil {
		t.Fatalf("drainBody bilan ham ulanish uzildi: %v", err)
	}
}

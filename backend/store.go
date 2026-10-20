package main

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"hash"
	"io"
	"os"
	"path/filepath"
	"sort"
	"sync"
)

// ---------------- Model ----------------

type Chunk struct {
	Index  int    `json:"index"`
	Size   int64  `json:"size"`
	SHA256 string `json:"sha256"`
}

type Node struct {
	ID                string   `json:"id"`
	Name              string   `json:"name"`
	OS                string   `json:"os"`
	Platform          string   `json:"platform"`
	Version           string   `json:"version"`
	CPUCores          int      `json:"cpuCores"`
	SpaceTotal        int64    `json:"spaceTotal"`
	SpaceUsed         int64    `json:"spaceUsed"`
	Online            bool     `json:"online"`
	UserActive        bool     `json:"userActive"`
	LastSeen          int64    `json:"lastSeen"`
	JoinedAt          int64    `json:"joinedAt"`
	VerifiedUptimeSec int64    `json:"verifiedUptimeSec"`
	EarningsCents     float64  `json:"earningsCents"`
	AgreementVersion  string   `json:"agreementVersion"`
	AgreementAccepted bool     `json:"agreementAccepted"`
	Status            string   `json:"status"` // active | uninstalled
	StoredFiles       []string `json:"storedFiles"`
	AuditsPassed      int      `json:"auditsPassed"`
	AuditsFailed      int      `json:"auditsFailed"`
	Simulated         bool     `json:"simulated,omitempty"`
	UninstalledAt     int64    `json:"uninstalledAt,omitempty"`

	// Hisoblash (GPU/CPU) klasteri — tugun ixtiyoriy ravishda yoqadi (shartnoma 1.1, 2.5)
	Compute *ComputeInfo `json:"compute,omitempty"`
}

// ComputeInfo — tugunning hisoblash imkoniyatlari (mijoz tomonidan xabar qilinadi)
type ComputeInfo struct {
	Enabled      bool     `json:"enabled"`
	Engines      []string `json:"engines"`   // ollama | llama.cpp | echo
	Models       []string `json:"models"`    // allaqachon mavjud modellar
	AllowPull    bool     `json:"allowPull"` // yangi modelni avtomatik yuklab olishga ruxsat
	RAMBytes     int64    `json:"ramBytes"`  // umumiy operativ xotira
	FreeRAMBytes int64    `json:"freeRamBytes"`
	CPUCores     int      `json:"cpuCores"`
	GPU          GPUInfo  `json:"gpu"`
	MaxParallel  int      `json:"maxParallel"`       // bir vaqtda nechta ish
	RPCPort      int      `json:"rpcPort,omitempty"` // llama.cpp rpc-server porti (katta modelni birlashtirish)
}

type GPUInfo struct {
	Present   bool   `json:"present"`
	Name      string `json:"name"`
	VRAMBytes int64  `json:"vramBytes"`
	Backend   string `json:"backend"` // cuda | metal | rocm | vulkan | none
}

type File struct {
	ID            string  `json:"id"`
	Name          string  `json:"name"`
	Size          int64   `json:"size"`
	SHA256        string  `json:"sha256"`
	ChunkSize     int64   `json:"chunkSize"`
	Chunks        []Chunk `json:"chunks"`
	UploadedAt    int64   `json:"uploadedAt"`
	ReplicaTarget int     `json:"replicaTarget"`
	ReplicasDone  int     `json:"replicasDone"`
	Status        string  `json:"status"`            // healthy | replicating | degraded | restoring | unavailable
	Missing       []int   `json:"missing,omitempty"` // serverda yo'q yoki buzilgan chunk'lar (tugunlardan tiklanadi)
}

type Task struct {
	ID         string  `json:"id"`
	NodeID     string  `json:"nodeId"`
	FileID     string  `json:"fileId"`
	Kind       string  `json:"kind"` // replica | repair | restore | delete
	Chunks     []Chunk `json:"chunks"`
	Status     string  `json:"status"` // pending | done | failed | cancelled
	AssignedAt int64   `json:"assignedAt"`
	DoneAt     int64   `json:"doneAt"`
	Error      string  `json:"error,omitempty"`
}

type Payout struct {
	ID          string  `json:"id"`
	NodeID      string  `json:"nodeId"`
	Period      string  `json:"period"`
	AmountCents float64 `json:"amountCents"`
	Status      string  `json:"status"` // paid | forfeited
	CreatedAt   int64   `json:"createdAt"`
}

const (
	NodeActive      = "active"
	NodeUninstalled = "uninstalled"

	TaskPending   = "pending"
	TaskDone      = "done"
	TaskFailed    = "failed"
	TaskCancelled = "cancelled"

	KindReplica = "replica" // yangi nusxa
	KindRepair  = "repair"  // tugunda yo'qolgan/buzilgan nusxani qayta yuklash (shartnoma 3.2)
	KindRestore = "restore" // tugundagi nusxadan server nusxasini tiklash
	KindDelete  = "delete"  // tugundan nusxani o'chirish

	FileHealthy     = "healthy"
	FileReplicating = "replicating"
	FileDegraded    = "degraded"
	FileRestoring   = "restoring"
	FileUnavailable = "unavailable"
)

// DB — butun klaster holati. Barcha maydonlar mu ostida o'zgartiriladi.
type DB struct {
	mu     sync.Mutex
	saveMu sync.Mutex
	path   string
	dirty  bool

	Nodes   map[string]*Node   `json:"nodes"`
	Files   map[string]*File   `json:"files"`
	Tasks   map[string]*Task   `json:"tasks"`
	Payouts map[string]*Payout `json:"payouts"`
	Tokens  map[string]string  `json:"tokens"` // nodeID -> sha256(token)
}

const GB = float64(1 << 30)

// ---------------- Yordamchi ----------------

func randomHex(nbytes int) string {
	b := make([]byte, nbytes)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	return hex.EncodeToString(b)
}

func randomID(prefix string) string { return prefix + "_" + randomHex(8) }

func sha256Hex(b []byte) string {
	s := sha256.Sum256(b)
	return hex.EncodeToString(s[:])
}

type hasher struct{ hash.Hash }

func newSHA256() hasher      { return hasher{sha256.New()} }
func (h hasher) hex() string { return hex.EncodeToString(h.Sum(nil)) }

func contains(list []string, s string) bool {
	for _, v := range list {
		if v == s {
			return true
		}
	}
	return false
}

func removeStr(list []string, s string) []string {
	out := list[:0]
	for _, v := range list {
		if v != s {
			out = append(out, v)
		}
	}
	return out
}

// appendMissing — tartiblangan, takrorsiz ro'yxatga qo'shadi
func appendMissing(list []int, v int) []int {
	if containsInt(list, v) {
		return list
	}
	list = append(list, v)
	sort.Ints(list)
	return list
}

func containsInt(list []int, v int) bool {
	for _, x := range list {
		if x == v {
			return true
		}
	}
	return false
}

// writeFileAtomic — tmp faylga yozib, diskka sinxronlab, keyin nomini almashtiradi.
// Elektr o'chib qolganda ham fayl yarim yozilgan holda qolmaydi.
func writeFileAtomic(path string, data []byte) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	tmp := path + ".tmp"
	f, err := os.OpenFile(tmp, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	if _, err := f.Write(data); err != nil {
		f.Close()
		os.Remove(tmp)
		return err
	}
	if err := f.Sync(); err != nil {
		f.Close()
		os.Remove(tmp)
		return err
	}
	if err := f.Close(); err != nil {
		os.Remove(tmp)
		return err
	}
	return os.Rename(tmp, path)
}

// ---------------- Saqlash ----------------

func OpenDB(path string) (*DB, error) {
	db := &DB{
		path:    path,
		Nodes:   map[string]*Node{},
		Files:   map[string]*File{},
		Tasks:   map[string]*Task{},
		Payouts: map[string]*Payout{},
		Tokens:  map[string]string{},
	}
	b, err := os.ReadFile(path)
	if err != nil && !os.IsNotExist(err) {
		return nil, err
	}
	if len(b) > 0 {
		if err := json.Unmarshal(b, db); err != nil {
			return nil, fmt.Errorf("%s buzilgan: %w", path, err)
		}
	}
	// eski formatdagi bo'sh (null) xaritalarni tiklash
	if db.Nodes == nil {
		db.Nodes = map[string]*Node{}
	}
	if db.Files == nil {
		db.Files = map[string]*File{}
	}
	if db.Tasks == nil {
		db.Tasks = map[string]*Task{}
	}
	if db.Payouts == nil {
		db.Payouts = map[string]*Payout{}
	}
	if db.Tokens == nil {
		db.Tokens = map[string]string{}
	}
	for id, n := range db.Nodes {
		if n == nil {
			delete(db.Nodes, id)
			continue
		}
		if n.Status == "" {
			n.Status = NodeActive
		}
	}
	return db, nil
}

// markDirty — mu ostida chaqiriladi; o'zgarish keyingi Flush da diskka yoziladi.
func (d *DB) markDirty() { d.dirty = true }

// Flush — o'zgarishlar bo'lsa db.json ga atomik yozadi.
func (d *DB) Flush() error {
	d.saveMu.Lock()
	defer d.saveMu.Unlock()
	d.mu.Lock()
	if !d.dirty {
		d.mu.Unlock()
		return nil
	}
	b, err := json.MarshalIndent(d, "", "  ")
	d.dirty = false
	d.mu.Unlock()
	if err == nil {
		err = writeFileAtomic(d.path, b)
	}
	if err != nil {
		d.mu.Lock()
		d.dirty = true
		d.mu.Unlock()
	}
	return err
}

// ---------------- Biznes yordamchilar (mu ostida) ----------------

// nodeStoredBytes — tugunda tasdiqlangan (saqlangan) fayllarning umumiy hajmi
func (d *DB) nodeStoredBytes(n *Node) int64 {
	var total int64
	for _, fid := range n.StoredFiles {
		if f, ok := d.Files[fid]; ok {
			total += f.Size
		}
	}
	return total
}

func (d *DB) pendingTasksOf(nodeID string) []*Task {
	var out []*Task
	for _, t := range d.Tasks {
		if t.NodeID == nodeID && t.Status == TaskPending {
			out = append(out, t)
		}
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].AssignedAt != out[j].AssignedAt {
			return out[i].AssignedAt < out[j].AssignedAt
		}
		return out[i].ID < out[j].ID
	})
	return out
}

func (d *DB) hasPendingTask(nodeID, fileID string, kinds ...string) bool {
	for _, t := range d.Tasks {
		if t.NodeID == nodeID && t.FileID == fileID && t.Status == TaskPending {
			for _, k := range kinds {
				if t.Kind == k {
					return true
				}
			}
		}
	}
	return false
}

func (d *DB) newTask(nodeID string, f *File, kind string, chunks []Chunk, ts int64) *Task {
	t := &Task{
		ID:         randomID("t"),
		NodeID:     nodeID,
		FileID:     f.ID,
		Kind:       kind,
		Chunks:     chunks,
		Status:     TaskPending,
		AssignedAt: ts,
	}
	d.Tasks[t.ID] = t
	d.markDirty()
	return t
}

// hashFile — diskdagi faylning SHA-256 si (migratsiya va tekshiruv uchun)
func hashFile(path string) (string, int64, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", 0, err
	}
	defer f.Close()
	h := sha256.New()
	n, err := io.Copy(h, f)
	if err != nil {
		return "", 0, err
	}
	return hex.EncodeToString(h.Sum(nil)), n, nil
}

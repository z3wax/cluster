package main

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"sync"
	"time"
)

// ---------------- Model ----------------

type Chunk struct {
	Index int   `json:"index"`
	Size  int64 `json:"size"`
}

type Node struct {
	ID                string   `json:"id"`
	Name              string   `json:"name"`
	OS                string   `json:"os"`
	Platform          string   `json:"platform"`
	SpaceTotal        int64    `json:"spaceTotal"`
	SpaceUsed         int64    `json:"spaceUsed"`
	Online            bool     `json:"online"`
	LastSeen          int64    `json:"lastSeen"`
	JoinedAt          int64    `json:"joinedAt"`
	VerifiedUptimeSec int64    `json:"verifiedUptimeSec"`
	EarningsCents     float64  `json:"earningsCents"`
	AgreementVersion  string   `json:"agreementVersion"`
	AgreementAccepted bool     `json:"agreementAccepted"`
	Status            string   `json:"status"` // active | uninstalled
	StoredFiles       []string `json:"storedFiles"`
}

type File struct {
	ID            string  `json:"id"`
	Name          string  `json:"name"`
	Size          int64   `json:"size"`
	ChunkSize     int64   `json:"chunkSize"`
	Chunks        []Chunk `json:"chunks"`
	UploadedAt    int64   `json:"uploadedAt"`
	ReplicaTarget int     `json:"replicaTarget"`
	ReplicasDone  int     `json:"replicasDone"`
	Status        string  `json:"status"` // active | degraded
}

type Task struct {
	ID         string  `json:"id"`
	NodeID     string  `json:"nodeId"`
	FileID     string  `json:"fileId"`
	Chunks     []Chunk `json:"chunks"`
	Status     string  `json:"status"` // pending | done | failed
	AssignedAt int64   `json:"assignedAt"`
	DoneAt     int64   `json:"doneAt"`
	Kind       string  `json:"kind"` // replica | repair
}

type Payout struct {
	ID          string  `json:"id"`
	NodeID      string  `json:"nodeId"`
	Period      string  `json:"period"`
	AmountCents float64 `json:"amountCents"`
	Status      string  `json:"status"` // paid | forfeited
	CreatedAt   int64   `json:"createdAt"`
}

type DB struct {
	mu      sync.RWMutex
	saveMu  sync.Mutex
	path    string
	Nodes   map[string]*Node   `json:"nodes"`
	Files   map[string]*File   `json:"files"`
	Tasks   map[string]*Task   `json:"tasks"`
	Payouts map[string]*Payout `json:"payouts"`
	Seq     int64              `json:"seq"`
}

// ---------------- Konfig sobitlar ----------------

const (
	HeartbeatTimeout = 90 * time.Second // uzilish deb hisoblash chegarasi
	ChunkSize        = 4 << 20          // 4 MiB
	ReplicaRatio     = 0.30             // 30% replikatsiya
	RateCentsPerGBH  = 0.05             // 1 GB / 1 soat = 0.05 sent
	GB               = float64(1 << 30)
)

// ---------------- Yordamchi ----------------

func randomID(prefix string) string {
	b := make([]byte, 8)
	rand.Read(b)
	return prefix + "_" + hex.EncodeToString(b)
}

func now() int64 { return time.Now().Unix() }

func contains(list []string, s string) bool {
	for _, v := range list {
		if v == s {
			return true
		}
	}
	return false
}

// ---------------- Saqlash ----------------

func OpenDB(path string) (*DB, error) {
	db := &DB{
		path:    path,
		Nodes:   map[string]*Node{},
		Files:   map[string]*File{},
		Tasks:   map[string]*Task{},
		Payouts: map[string]*Payout{},
	}
	b, err := os.ReadFile(path)
	if err != nil && !os.IsNotExist(err) {
		return nil, err
	}
	if err == nil {
		if err := json.Unmarshal(b, db); err != nil {
			return nil, err
		}
	}
	// JSON null collections must not turn the first write into a map panic.
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
	return db, nil
}

func (d *DB) Save() error {
	// Serialize the snapshot and replacement together: concurrent callers must
	// neither overwrite the same temporary file nor publish an older snapshot.
	d.saveMu.Lock()
	defer d.saveMu.Unlock()
	d.mu.RLock()
	b, err := json.MarshalIndent(d, "", "  ")
	d.mu.RUnlock()
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(d.path), 0o755); err != nil {
		return err
	}
	tmp := d.path + ".tmp"
	if err := os.WriteFile(tmp, b, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, d.path) // atomik yozish
}

func (d *DB) nextSeq() int64 {
	d.Seq++
	return d.Seq
}

// ---------------- Biznes yordamchilar ----------------

func (d *DB) onlineNodes() []*Node {
	var out []*Node
	for _, n := range d.Nodes {
		if n.Online && n.Status == "active" && n.AgreementAccepted {
			out = append(out, n)
		}
	}
	return out
}

// fileVerifiedSize — tugunda tasdiqlangan (saqlangan) fayllarning umumiy hajmi
func (d *DB) nodeStoredBytes(n *Node) int64 {
	var total int64
	for _, fid := range n.StoredFiles {
		if f, ok := d.Files[fid]; ok {
			total += f.Size
		}
	}
	return total
}

// replicaPick — n ta tugundan 30% ini tanlaydi (round-robin)
func replicaPick(nodes []*Node) []*Node {
	target := float64(len(nodes)) * ReplicaRatio
	n := int(target)
	if float64(n) < target {
		n++ // ceil
	}
	if n < 1 {
		n = 1
	}
	if n > len(nodes) {
		n = len(nodes)
	}
	return nodes[:n]
}

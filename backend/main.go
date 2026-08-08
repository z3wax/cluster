package main

import (
	"context"
	"flag"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"time"
)

func findWebDir(root string) string {
	candidates := []string{
		filepath.Join(root, "web"),
		filepath.Join(root, "backend", "web"),
		"web",
		"backend/web",
		"../backend/web",
	}
	for _, c := range candidates {
		if fi, err := os.Stat(c); err == nil && fi.IsDir() {
			abs, _ := filepath.Abs(c)
			return abs
		}
	}
	return filepath.Join(root, "web")
}

func findDataDir(dir string) string {
	if fi, err := os.Stat(dir); err == nil && fi.IsDir() {
		abs, _ := filepath.Abs(dir)
		return abs
	}
	candidates := []string{
		dir,
		filepath.Join("backend", dir),
		filepath.Join("..", dir),
	}
	for _, c := range candidates {
		if fi, err := os.Stat(c); err == nil && fi.IsDir() {
			abs, _ := filepath.Abs(c)
			return abs
		}
	}
	os.MkdirAll(dir, 0o755)
	abs, _ := filepath.Abs(dir)
	return abs
}

func main() {
	addr := flag.String("addr", ":8080", "server manzili")
	dataDirFlag := flag.String("data", "data", "ma'lumotlar papkasi")
	flag.Parse()

	root, err := os.Getwd()
	if err != nil {
		log.Fatal(err)
	}

	webDir := findWebDir(root)
	dataDir := findDataDir(*dataDirFlag)

	dbPath := filepath.Join(dataDir, "db.json")
	db, err := OpenDB(dbPath)
	if err != nil {
		log.Fatalf("db ochishda xato: %v", err)
	}

	chunksDir := filepath.Join(dataDir, "chunks")
	if err := os.MkdirAll(chunksDir, 0o755); err != nil {
		log.Fatal(err)
	}

	// o'chirilgan tugunlarni skanerlash (offline aniqlash)
	go sweeper(db)

	mux := http.NewServeMux()
	a := &api{db: db, data: dataDir}
	a.routes(mux)

	// statik frontend
	log.Printf("Statik Web Papkasi: %s", webDir)
	mux.Handle("/", http.FileServer(http.Dir(webDir)))

	// CORS (dashboard va mijozlardan ochiq kirish uchun)
	handler := cors(mux)

	log.Printf("SDrive Cluster server ishga tushdi: http://localhost%s", *addr)
	log.Printf("DB: %s | Chunks: %s", dbPath, chunksDir)
	if err := http.ListenAndServe(*addr, handler); err != nil {
		log.Fatal(err)
	}
}

func sweeper(db *DB) {
	tick := time.NewTicker(30 * time.Second)
	for range tick.C {
		changed := false
		db.mu.Lock()
		ts := time.Now().Unix()
		for _, n := range db.Nodes {
			if n.Online && ts-n.LastSeen > int64(HeartbeatTimeout.Seconds()) {
				// Internet/elektr uzilishi — onlayn vaqt to'xtatiladi
				n.Online = false
				changed = true
			}
		}
		db.mu.Unlock()
		if changed {
			db.Save()
		}
	}
}

func cors(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type")
		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		ctx := context.WithValue(r.Context(), "root", mustCwd())
		next.ServeHTTP(w, r.WithContext(ctx))
	})
}

func mustCwd() string {
	wd, err := os.Getwd()
	if err != nil {
		return "."
	}
	return wd
}

package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"
)

// Config — server sozlamalari (flag'lar orqali o'zgartiriladi)
type Config struct {
	Addr          string
	DataDir       string
	WebDir        string // bo'sh bo'lsa: ./web yoki binarga joylangan nusxa
	DownloadsDir  string // mijoz dasturlari (/client/...)
	AgreementPath string
	AdminToken    string
	TLSCert       string
	TLSKey        string

	HeartbeatInterval time.Duration // mijoz heartbeat oralig'i
	OfflineTimeout    time.Duration // shu vaqt heartbeat kelmasa — uzilish (internet/elektr)
	RepairDelay       time.Duration // oflayn tugundagi nusxa shu vaqtdan keyin boshqa tugunga ko'chiriladi
	ReconcileInterval time.Duration // replikatsiya menejeri sikli
	AuditInterval     time.Duration // har bir tugunni saqlash isboti (proof-of-storage) bilan tekshirish oralig'i
	ScrubInterval     time.Duration // server chunk'larini to'liq SHA-256 tekshiruvi
	TaskTimeout       time.Duration // bajarilmagan vazifa shu vaqtdan keyin boshqa tugunga beriladi
	ActiveWindow      time.Duration // 30% hisobi shu vaqt ichida ko'ringan tugunlar bo'yicha

	ReplicaRatio       float64 // 30% replikatsiya (shartnoma 2.3)
	RateCentsPerGBHour float64 // 1 GB / 1 soat uchun to'lov (sent)
	ChunkSize          int64
	MaxUploadBytes     int64
	PayoutAnytime      bool // test uchun: payout oynasini cheklamaslik
	PayoutWindowDays   int  // oy oxiridagi payout oynasi (kun)
	MaxSimNodes        int

	// Hisoblash (GPU/CPU) klasteri
	ComputeKeys         []string      // OpenAI API kalitlari (admin token ham ishlaydi)
	ComputePollTimeout  time.Duration // worker uzoq-poll muddati
	ComputeJobTimeout   time.Duration // bitta inference ishi uchun chegara
	ComputeQueueTimeout time.Duration // bo'sh tugun kutish chegarasi
}

func defaultConfig() Config {
	return Config{
		Addr:               ":8080",
		DataDir:            "data",
		HeartbeatInterval:  30 * time.Second,
		OfflineTimeout:     90 * time.Second,
		RepairDelay:        5 * time.Minute,
		ReconcileInterval:  10 * time.Second,
		AuditInterval:      10 * time.Minute,
		ScrubInterval:      6 * time.Hour,
		TaskTimeout:        6 * time.Hour,
		ActiveWindow:       24 * time.Hour,
		ReplicaRatio:       0.30,
		RateCentsPerGBHour: 0.05,
		ChunkSize:          4 << 20, // 4 MiB
		MaxUploadBytes:     16 << 30,
		PayoutWindowDays:   3,
		MaxSimNodes:        500,

		ComputePollTimeout:  25 * time.Second,
		ComputeJobTimeout:   10 * time.Minute,
		ComputeQueueTimeout: 60 * time.Second,
	}
}

func main() {
	cfg := defaultConfig()
	// Hosting platformalari (Render, Railway, Fly, HF Spaces) portni $PORT orqali beradi
	if p := os.Getenv("PORT"); p != "" {
		cfg.Addr = ":" + p
	}
	if d := os.Getenv("SDRIVE_DATA_DIR"); d != "" {
		cfg.DataDir = d
	}
	flag.StringVar(&cfg.Addr, "addr", cfg.Addr, "server manzili (host:port) — $PORT ham ishlaydi")
	flag.StringVar(&cfg.DataDir, "data", cfg.DataDir, "ma'lumotlar papkasi (db.json, chunks)")
	flag.StringVar(&cfg.WebDir, "web", "", "statik veb papka (bo'sh: ./web, topilmasa binarga joylangan nusxa)")
	flag.StringVar(&cfg.DownloadsDir, "downloads", "", "mijoz dasturlari papkasi, /client/ orqali beriladi (bo'sh: <web>/client)")
	flag.StringVar(&cfg.AgreementPath, "agreement", "", "SHARTNOMA.md yo'li (bo'sh: avtomatik qidiriladi)")
	flag.StringVar(&cfg.AdminToken, "admin-token", os.Getenv("SDRIVE_ADMIN_TOKEN"), "admin token (bo'sh: <data>/admin_token dan o'qiladi yoki yaratiladi)")
	flag.StringVar(&cfg.TLSCert, "tls-cert", "", "HTTPS sertifikati (PEM); -tls-key bilan birga")
	flag.StringVar(&cfg.TLSKey, "tls-key", "", "HTTPS kaliti (PEM)")
	flag.DurationVar(&cfg.HeartbeatInterval, "heartbeat", cfg.HeartbeatInterval, "mijoz heartbeat oralig'i")
	flag.DurationVar(&cfg.OfflineTimeout, "offline-timeout", cfg.OfflineTimeout, "heartbeat kelmasa oflayn deb hisoblash vaqti")
	flag.DurationVar(&cfg.RepairDelay, "repair-delay", cfg.RepairDelay, "oflayn tugundagi nusxani boshqa tugunga ko'chirishdan oldin kutish")
	flag.DurationVar(&cfg.ReconcileInterval, "reconcile", cfg.ReconcileInterval, "replikatsiya menejeri oralig'i")
	flag.DurationVar(&cfg.AuditInterval, "audit", cfg.AuditInterval, "tugun saqlash isboti tekshiruvi oralig'i")
	flag.DurationVar(&cfg.ScrubInterval, "scrub", cfg.ScrubInterval, "server chunk'larini to'liq tekshirish oralig'i")
	flag.Float64Var(&cfg.RateCentsPerGBHour, "rate", cfg.RateCentsPerGBHour, "tarif: 1 GB saqlangan fayl uchun 1 soatda to'lanadigan sent")
	flag.Int64Var(&cfg.MaxUploadBytes, "max-upload", cfg.MaxUploadBytes, "maksimal yuklanadigan fayl hajmi (bayt)")
	flag.BoolVar(&cfg.PayoutAnytime, "payout-anytime", false, "payout'ni oy oxirini kutmasdan ruxsat berish (test)")
	computeKeys := flag.String("compute-keys", os.Getenv("SDRIVE_COMPUTE_KEYS"), "OpenAI API kalitlari (vergul bilan); bo'sh bo'lsa admin token ishlatiladi")
	flag.DurationVar(&cfg.ComputeJobTimeout, "compute-job-timeout", cfg.ComputeJobTimeout, "bitta inference ishi uchun vaqt chegarasi")
	flag.Parse()

	for _, k := range strings.Split(*computeKeys, ",") {
		if k = strings.TrimSpace(k); k != "" {
			cfg.ComputeKeys = append(cfg.ComputeKeys, k)
		}
	}

	if cfg.OfflineTimeout <= cfg.HeartbeatInterval {
		log.Fatalf("-offline-timeout (%s) -heartbeat (%s) dan katta bo'lishi kerak", cfg.OfflineTimeout, cfg.HeartbeatInterval)
	}

	s, err := NewServer(cfg)
	if err != nil {
		log.Fatal(err)
	}

	ln, err := net.Listen("tcp", cfg.Addr)
	if err != nil {
		log.Fatalf("%s tinglab bo'lmadi: %v", cfg.Addr, err)
	}
	port := ln.Addr().(*net.TCPAddr).Port
	useTLS := cfg.TLSCert != "" || cfg.TLSKey != ""
	scheme := "http"
	if useTLS {
		if cfg.TLSCert == "" || cfg.TLSKey == "" {
			log.Fatal("-tls-cert va -tls-key birga berilishi kerak")
		}
		scheme = "https"
		s.sim.insecureTLS() // simulyator tugunlari serverga 127.0.0.1 orqali ulanadi
	}
	s.baseURL = fmt.Sprintf("%s://127.0.0.1:%d", scheme, port)

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	s.Start(ctx)

	srv := &http.Server{
		Handler:           s.Handler(),
		ReadHeaderTimeout: 15 * time.Second,
		IdleTimeout:       2 * time.Minute,
	}
	go func() {
		<-ctx.Done()
		log.Printf("to'xtatilmoqda...")
		s.sim.stopAll()
		sctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		srv.Shutdown(sctx)
	}()

	log.Printf("SDrive Cluster server ishga tushdi: %s://localhost:%d", scheme, port)
	if useTLS {
		err = srv.ServeTLS(ln, cfg.TLSCert, cfg.TLSKey)
	} else {
		err = srv.Serve(ln)
	}
	if err != nil && !errors.Is(err, http.ErrServerClosed) {
		log.Fatal(err)
	}
	s.Close()
	log.Printf("server to'xtadi, ma'lumotlar saqlandi")
}

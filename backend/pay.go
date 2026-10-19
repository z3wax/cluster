package main

import (
	"fmt"
	"log"
	"net/http"
	"time"
)

// addEarningsLocked — tasdiqlangan onlayn vaqt uchun daromad hisoblaydi.
// Shartnoma qoidalari:
//   - internet uzilgan vaqt hisobga olinmaydi (interval > OfflineTimeout)
//   - elektr yo'q paytdagi vaqt hisobga olinmaydi (xuddi shu interval)
//   - yuklanmagan deb topilgan fayllar hisobga kirmaydi (StoredFiles ro'yxati, auditlar)
//   - to'lov fayl hajmiga qarab (4.1-band): 1 GB / 1 soat = RateCentsPerGBHour sent
func (s *Server) addEarningsLocked(n *Node, elapsedSec int64) {
	if elapsedSec <= 0 {
		return
	}
	stored := float64(s.db.nodeStoredBytes(n))
	n.EarningsCents += stored / GB * (float64(elapsedSec) / 3600.0) * s.cfg.RateCentsPerGBHour
}

func currentPeriod(t time.Time) string { return t.Format("2006-01") }

// payoutWindow — oy oxiridagi payout oynasi (shartnoma 6.1, 6.3)
func (s *Server) payoutWindow() (open bool, opensAt, closesAt time.Time) {
	t := s.clock()
	y, m, _ := t.Date()
	first := time.Date(y, m, 1, 0, 0, 0, 0, t.Location())
	next := first.AddDate(0, 1, 0)
	opens := next.AddDate(0, 0, -s.cfg.PayoutWindowDays)
	if s.cfg.PayoutAnytime {
		return true, first, next
	}
	return !t.Before(opens), opens, next
}

func (s *Server) handlePayout(w http.ResponseWriter, r *http.Request) {
	open, opens, _ := s.payoutWindow()
	s.db.mu.Lock()
	n := s.authNodeLocked(w, r)
	if n == nil {
		s.db.mu.Unlock()
		return
	}
	if !open {
		s.db.mu.Unlock()
		writeJSON(w, http.StatusForbidden, map[string]any{
			"ok":      false,
			"error":   fmt.Sprintf("Cash chiqarish faqat oy oxirida mumkin: %s dan boshlab", opens.Format("2006-01-02")),
			"opensAt": opens.Unix(),
		})
		return
	}
	amount := n.EarningsCents
	if amount < 0.01 {
		s.db.mu.Unlock()
		writeErr(w, http.StatusBadRequest, "chiqarish uchun balans yo'q")
		return
	}
	p := &Payout{
		ID:          randomID("pay"),
		NodeID:      n.ID,
		Period:      currentPeriod(s.clock()),
		AmountCents: amount,
		Status:      "paid",
		CreatedAt:   s.now(),
	}
	s.db.Payouts[p.ID] = p
	n.EarningsCents = 0
	s.db.markDirty()
	s.db.mu.Unlock()
	if err := s.db.Flush(); err != nil {
		log.Printf("payout saqlanmadi: %v", err)
	}
	log.Printf("payout: tugun=%s summa=%.4f sent", p.NodeID, p.AmountCents)
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "payout": p, "note": "Oy oxiridagi payout rasmiylashtirildi"})
}

// uninstallNodeLocked — dastur o'chirilganda chaqiriladi.
// Shartnoma 6.2: oylik pul chiqarib olinmagan bo'lsa, balans yo'qotiladi (forfeited).
func (s *Server) uninstallNodeLocked(nodeID string) (forfeited float64, ok bool) {
	n, ok := s.db.Nodes[nodeID]
	if !ok || n.Status == NodeUninstalled {
		return 0, ok
	}
	ts := s.now()
	forfeited = n.EarningsCents
	if forfeited > 0 {
		p := &Payout{
			ID:          randomID("pay"),
			NodeID:      nodeID,
			Period:      currentPeriod(s.clock()),
			AmountCents: forfeited,
			Status:      "forfeited",
			CreatedAt:   ts,
		}
		s.db.Payouts[p.ID] = p
	}
	n.EarningsCents = 0
	n.Status = NodeUninstalled
	n.Online = false
	n.UserActive = false
	n.StoredFiles = nil
	n.UninstalledAt = ts
	delete(s.db.Tokens, nodeID)
	for _, t := range s.db.Tasks {
		if t.NodeID == nodeID && t.Status == TaskPending {
			finishTask(t, TaskCancelled, "tugun o'chirildi", ts)
		}
	}
	for id, a := range s.audits {
		if a.NodeID == nodeID {
			delete(s.audits, id)
		}
	}
	s.db.markDirty()
	return forfeited, true
}

// purgeNodeLocked — tugunni bazadan butunlay o'chiradi (simulyator tugunlari uchun)
func (s *Server) purgeNodeLocked(nodeID string) {
	s.uninstallNodeLocked(nodeID)
	delete(s.db.Nodes, nodeID)
	for id, t := range s.db.Tasks {
		if t.NodeID == nodeID {
			delete(s.db.Tasks, id)
		}
	}
	for id, p := range s.db.Payouts {
		if p.NodeID == nodeID {
			delete(s.db.Payouts, id)
		}
	}
	delete(s.lastAudit, nodeID)
	delete(s.priorityAudit, nodeID)
	s.db.markDirty()
}

func (s *Server) handleUninstall(w http.ResponseWriter, r *http.Request) {
	s.db.mu.Lock()
	n := s.authNodeLocked(w, r)
	if n == nil {
		s.db.mu.Unlock()
		return
	}
	forfeited, _ := s.uninstallNodeLocked(n.ID)
	s.db.mu.Unlock()
	s.persist()
	s.triggerReconcile() // undagi nusxalar boshqa tugunlarga ko'chiriladi
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":             true,
		"forfeitedCents": forfeited,
		"note":           "payout olinmagan balans yo'qotildi (shartnoma 6.2-band)",
	})
}

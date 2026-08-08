package main

import (
	"fmt"
	"time"
)

// addEarnings — tasdiqlangan onlayn vaqt uchun daromad hisoblaydi.
// Shartnoma qoidalari:
//   - internet uzilgan vaqt hisobga olinmaydi (interval > HeartbeatTimeout)
//   - elektr yo'q paytdagi vaqt hisobga olinmaydi (xuddi shu interval)
//   - yuklanmagan deb topilgan fayllar hisobga kirmaydi (StoredFiles ro'yxati)
func (d *DB) addEarnings(n *Node, elapsedSec int64) {
	if elapsedSec <= 0 {
		return
	}
	stored := float64(d.nodeStoredBytes(n))
	// 1 GB uchun 1 soat = RateCentsPerGBH sent
	cents := stored / GB * (float64(elapsedSec) / 3600.0) * RateCentsPerGBH
	n.EarningsCents += cents
}

// claimPayout — oy oxirida cash chiqarish.
// Qoida: hisobni o'chirish (uninstall) paytida oylik payout olinmagan bo'lsa,
// to'lov o'tkazilmaydi (forfeited).
func (d *DB) claimPayout(nodeID, period string) (*Payout, error) {
	n, ok := d.Nodes[nodeID]
	if !ok {
		return nil, fmt.Errorf("node topilmadi")
	}
	if n.Status == "uninstalled" {
		// Shartnoma 6.2-band: oylik pulni olmasdan hisob o'chirilgan — to'lanmaydi
		p := &Payout{
			ID:          randomID("pay"),
			NodeID:      nodeID,
			Period:      period,
			AmountCents: 0,
			Status:      "forfeited",
			CreatedAt:   now(),
		}
		d.Payouts[p.ID] = p
		return p, nil
	}
	amount := n.EarningsCents
	n.EarningsCents = 0
	p := &Payout{
		ID:          randomID("pay"),
		NodeID:      nodeID,
		Period:      period,
		AmountCents: amount,
		Status:      "paid",
		CreatedAt:   now(),
	}
	d.Payouts[p.ID] = p
	return p, nil
}

func currentPeriod(t time.Time) string {
	return t.Format("2006-01")
}

// uninstallNode — dastur o'chirilganda chaqiriladi.
// Agar joriy oy uchun payout olinmagan bo'lsa, daromad yo'qotiladi
// (payout keyin faqat forfeited bo'ladi).
func (d *DB) uninstallNode(nodeID string) {
	if n, ok := d.Nodes[nodeID]; ok {
		n.Status = "uninstalled"
		n.Online = false
		d.claimPayout(nodeID, currentPeriod(time.Now()))
	}
}

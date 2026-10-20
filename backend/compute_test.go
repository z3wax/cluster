package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

// ---------------- Test uchun hisoblash tuguni (echo dvigateli) ----------------

type cjob struct {
	ID       string         `json:"id"`
	Model    string         `json:"model"`
	Kind     string         `json:"kind"`
	Messages []ChatMessage  `json:"messages"`
	Prompt   string         `json:"prompt"`
	Params   map[string]any `json:"params"`
}

type engineResult struct {
	tokens    []string
	embedding []float64
	err       error
}

// echoEngine — deterministik: oxirgi foydalanuvchi xabarini (yoki prompt'ni) so'zma-so'z qaytaradi.
func echoEngine(j cjob) engineResult {
	text := j.Prompt
	if j.Kind == KindChat {
		for i := len(j.Messages) - 1; i >= 0; i-- {
			if j.Messages[i].Role == "user" {
				text = j.Messages[i].Content
				break
			}
		}
	}
	if j.Kind == KindEmbedding {
		vec := make([]float64, 8)
		for i, r := range text {
			vec[i%8] += float64(r%17) / 17.0
		}
		return engineResult{embedding: vec}
	}
	return engineResult{tokens: splitKeepSpace(text)}
}

func splitKeepSpace(s string) []string {
	if s == "" {
		return nil
	}
	return strings.SplitAfter(s, " ")
}

// computeWorker — bitta poll-loop (C++ mijozidagi mantiq bilan bir xil)
func (c *nodeClient) computeWorker(ctx context.Context, engine func(cjob) engineResult) {
	pollBody := map[string]any{"models": c.Compute.Models, "engines": c.Compute.Engines, "allowPull": c.Compute.AllowPull}
	bodyBytes, _ := json.Marshal(pollBody)
	for ctx.Err() == nil {
		req, _ := http.NewRequestWithContext(ctx, http.MethodPost, c.base+"/api/nodes/"+c.ID+"/compute/poll", bytes.NewReader(bodyBytes))
		req.Header.Set("Authorization", "Bearer "+c.Token)
		req.Header.Set("Content-Type", "application/json")
		resp, err := c.hc.Do(req)
		if err != nil {
			if ctx.Err() != nil {
				return
			}
			time.Sleep(50 * time.Millisecond)
			continue
		}
		var pr struct {
			Job *cjob `json:"job"`
		}
		json.NewDecoder(resp.Body).Decode(&pr)
		resp.Body.Close()
		if pr.Job == nil {
			continue
		}
		c.runComputeJob(ctx, *pr.Job, engine)
	}
}

func (c *nodeClient) runComputeJob(ctx context.Context, j cjob, engine func(cjob) engineResult) {
	res := engine(j)
	base := "/api/compute/jobs/" + j.ID
	if res.err != nil {
		c.postJSON(base+"/fail", map[string]any{"error": res.err.Error(), "requeue": true}, nil)
		return
	}
	for _, tok := range res.tokens {
		if ctx.Err() != nil {
			return
		}
		var cr struct {
			OK       bool `json:"ok"`
			Canceled bool `json:"canceled"`
		}
		code, _ := c.postJSON(base+"/chunk", map[string]any{"token": tok}, &cr)
		if code == http.StatusConflict || cr.Canceled {
			return // bekor qilingan
		}
	}
	c.postJSON(base+"/done", map[string]any{
		"result":       strings.Join(res.tokens, ""),
		"finishReason": "stop",
		"promptTokens": len(j.Prompt) + len(j.Messages),
		"embedding":    res.embedding,
	}, nil)
}

// startComputeWorkers — har bir mijoz uchun maxParallel poll-loop ishga tushiradi
func (e *env) startComputeWorkers(nodes []*nodeClient, engine func(cjob) engineResult) {
	ctx, cancel := context.WithCancel(context.Background())
	var wg sync.WaitGroup
	for _, c := range nodes {
		par := 1
		if c.Compute != nil && c.Compute.MaxParallel > par {
			par = c.Compute.MaxParallel
		}
		for i := 0; i < par; i++ {
			wg.Add(1)
			go func(c *nodeClient) { defer wg.Done(); c.computeWorker(ctx, engine) }(c)
		}
	}
	e.t.Cleanup(func() { cancel(); wg.Wait() })
}

func echoInfo(models []string, maxPar int, gpu bool) *ComputeInfo {
	ci := &ComputeInfo{Enabled: true, Engines: []string{"echo"}, Models: models, MaxParallel: maxPar,
		RAMBytes: 16 << 30, CPUCores: 8}
	if gpu {
		ci.GPU = GPUInfo{Present: true, Name: "Test GPU", VRAMBytes: 8 << 30, Backend: "cuda"}
	}
	return ci
}

func (e *env) computeNode(id string, ci *ComputeInfo) *nodeClient {
	c := e.node(id, 10)
	c.Compute = ci
	if err := c.register(); err != nil { // compute bilan qayta ro'yxatdan o'tish
		e.t.Fatal(err)
	}
	return c
}

// ---------------- Requester yordamchilari ----------------

func (e *env) openai(path string, body any) *http.Response {
	e.t.Helper()
	b, _ := json.Marshal(body)
	req, _ := http.NewRequest("POST", e.ts.URL+path, bytes.NewReader(b))
	req.Header.Set("Authorization", "Bearer "+testAdminToken)
	req.Header.Set("Content-Type", "application/json")
	resp, err := e.ts.Client().Do(req)
	if err != nil {
		e.t.Fatal(err)
	}
	return resp
}

// readSSE — SSE oqimidan "data:" qatorlarini yig'adi
func readSSE(t *testing.T, resp *http.Response) []string {
	t.Helper()
	defer resp.Body.Close()
	var out []string
	sc := bufio.NewScanner(resp.Body)
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	for sc.Scan() {
		line := sc.Text()
		if strings.HasPrefix(line, "data: ") {
			out = append(out, strings.TrimPrefix(line, "data: "))
		}
	}
	return out
}

func testComputeConfig(c *Config) {
	c.ComputePollTimeout = 2 * time.Second
	c.ComputeQueueTimeout = 4 * time.Second
	c.ComputeJobTimeout = 20 * time.Second
}

// ---------------- Testlar ----------------

func TestComputeCapabilityAndModels(t *testing.T) {
	e := newEnv(t, testComputeConfig)
	e.computeNode("gpu-node-01", echoInfo([]string{"gemma:2b", "llama3:8b"}, 2, true))
	e.computeNode("cpu-node-01", echoInfo([]string{"gemma:2b"}, 1, false))
	e.node("storage-01", 10) // hisoblashsiz tugun

	resp := e.admin("GET", "/api/compute/workers", nil)
	var d struct {
		Workers []computeWorkerInfo `json:"workers"`
		Models  []string            `json:"models"`
	}
	decode(t, resp, &d)
	if len(d.Workers) != 2 {
		t.Fatalf("2 hisoblash tuguni kutilgan, %d", len(d.Workers))
	}
	if len(d.Models) != 2 || d.Models[0] != "gemma:2b" || d.Models[1] != "llama3:8b" {
		t.Fatalf("modellar: %v", d.Models)
	}
	gpu := false
	for _, w := range d.Workers {
		if w.ID == "gpu-node-01" {
			if !w.GPU.Present || w.GPU.Backend != "cuda" {
				t.Fatal("GPU xabar qilinmadi")
			}
			gpu = true
		}
	}
	if !gpu {
		t.Fatal("gpu-node topilmadi")
	}
}

func TestOpenAIModelsAuth(t *testing.T) {
	e := newEnv(t, testComputeConfig)
	e.computeNode("node-01", echoInfo([]string{"gemma:2b"}, 1, false))

	resp, _ := http.Get(e.ts.URL + "/v1/models")
	resp.Body.Close()
	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("kalitsiz /v1/models: %d", resp.StatusCode)
	}
	req, _ := http.NewRequest("GET", e.ts.URL+"/v1/models", nil)
	req.Header.Set("Authorization", "Bearer "+testAdminToken)
	resp, _ = e.ts.Client().Do(req)
	var d struct {
		Data []struct {
			ID string `json:"id"`
		} `json:"data"`
	}
	decode(t, resp, &d)
	if len(d.Data) != 1 || d.Data[0].ID != "gemma:2b" {
		t.Fatalf("/v1/models: %+v", d.Data)
	}
}

func TestComputeChatRoundTrip(t *testing.T) {
	e := newEnv(t, testComputeConfig)
	n := e.computeNode("worker-01", echoInfo([]string{"gemma:2b"}, 1, false))
	e.startComputeWorkers([]*nodeClient{n}, echoEngine)

	resp := e.admin("POST", "/api/compute/jobs", strings.NewReader(
		`{"model":"gemma:2b","messages":[{"role":"user","content":"Salom dunyo, bu test"}]}`))
	var sub struct {
		OK    bool   `json:"ok"`
		JobID string `json:"jobId"`
	}
	decode(t, resp, &sub)
	if !sub.OK || sub.JobID == "" {
		t.Fatalf("submit: %+v", sub)
	}
	// natijani kutish
	var jr map[string]any
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		r := e.admin("GET", "/api/compute/jobs/"+sub.JobID, nil)
		decode(t, r, &jr)
		if jr["status"] == "done" || jr["status"] == "failed" {
			break
		}
		time.Sleep(50 * time.Millisecond)
	}
	if jr["status"] != "done" {
		t.Fatalf("ish holati: %v (%v)", jr["status"], jr["error"])
	}
	if jr["result"] != "Salom dunyo, bu test" {
		t.Fatalf("natija: %q", jr["result"])
	}
	if jr["worker"] != "worker-01" {
		t.Fatalf("worker: %v", jr["worker"])
	}
}

func TestOpenAIChatNonStream(t *testing.T) {
	e := newEnv(t, testComputeConfig)
	n := e.computeNode("worker-01", echoInfo([]string{"gemma:2b"}, 1, false))
	e.startComputeWorkers([]*nodeClient{n}, echoEngine)

	resp := e.openai("/v1/chat/completions", map[string]any{
		"model":    "gemma:2b",
		"messages": []ChatMessage{{Role: "system", Content: "sen yordamchisan"}, {Role: "user", Content: "Nabiyev"}},
	})
	var d struct {
		Object  string `json:"object"`
		Model   string `json:"model"`
		Choices []struct {
			Message      ChatMessage `json:"message"`
			FinishReason string      `json:"finish_reason"`
		} `json:"choices"`
		Usage struct {
			CompletionTokens int `json:"completion_tokens"`
			TotalTokens      int `json:"total_tokens"`
		} `json:"usage"`
	}
	decode(t, resp, &d)
	if d.Object != "chat.completion" || d.Model != "gemma:2b" {
		t.Fatalf("javob: %+v", d)
	}
	if len(d.Choices) != 1 || d.Choices[0].Message.Content != "Nabiyev" || d.Choices[0].Message.Role != "assistant" {
		t.Fatalf("choices: %+v", d.Choices)
	}
	if d.Choices[0].FinishReason != "stop" || d.Usage.TotalTokens == 0 {
		t.Fatalf("finish/usage: %+v %+v", d.Choices[0], d.Usage)
	}
}

func TestOpenAIChatStreaming(t *testing.T) {
	e := newEnv(t, testComputeConfig)
	n := e.computeNode("worker-01", echoInfo([]string{"gemma:2b"}, 1, false))
	e.startComputeWorkers([]*nodeClient{n}, echoEngine)

	resp := e.openai("/v1/chat/completions", map[string]any{
		"model":    "gemma:2b",
		"stream":   true,
		"messages": []ChatMessage{{Role: "user", Content: "bir ikki uch tort besh"}},
	})
	if ct := resp.Header.Get("Content-Type"); !strings.HasPrefix(ct, "text/event-stream") {
		t.Fatalf("Content-Type: %q", ct)
	}
	lines := readSSE(t, resp)
	if lines[len(lines)-1] != "[DONE]" {
		t.Fatalf("oxirgi qator [DONE] emas: %q", lines[len(lines)-1])
	}
	var content strings.Builder
	sawStop := false
	for _, ln := range lines {
		if ln == "[DONE]" {
			continue
		}
		var chunk struct {
			Object  string `json:"object"`
			Choices []struct {
				Delta struct {
					Content string `json:"content"`
				} `json:"delta"`
				FinishReason *string `json:"finish_reason"`
			} `json:"choices"`
		}
		if err := json.Unmarshal([]byte(ln), &chunk); err != nil {
			t.Fatalf("chunk JSON: %v (%s)", err, ln)
		}
		if chunk.Object != "chat.completion.chunk" {
			t.Fatalf("object: %s", chunk.Object)
		}
		for _, ch := range chunk.Choices {
			content.WriteString(ch.Delta.Content)
			if ch.FinishReason != nil && *ch.FinishReason == "stop" {
				sawStop = true
			}
		}
	}
	if content.String() != "bir ikki uch tort besh" {
		t.Fatalf("yig'ilgan matn: %q", content.String())
	}
	if !sawStop {
		t.Fatal("finish_reason=stop ko'rinmadi")
	}
}

func TestComputeNoWorkerAvailable(t *testing.T) {
	e := newEnv(t, testComputeConfig)
	resp := e.openai("/v1/chat/completions", map[string]any{
		"model": "gemma:2b", "messages": []ChatMessage{{Role: "user", Content: "salom"}}})
	resp.Body.Close()
	if resp.StatusCode != http.StatusServiceUnavailable {
		t.Fatalf("tugunsiz: %d", resp.StatusCode)
	}
	// xotira tuguni bor, lekin hisoblash yoqilmagan
	e.node("storage-01", 10)
	resp = e.admin("POST", "/api/compute/jobs", strings.NewReader(`{"model":"x","messages":[{"role":"user","content":"a"}]}`))
	resp.Body.Close()
	if resp.StatusCode != http.StatusServiceUnavailable {
		t.Fatalf("faqat xotira tuguni: %d", resp.StatusCode)
	}
}

func TestComputeModelRouting(t *testing.T) {
	e := newEnv(t, testComputeConfig)
	a := e.computeNode("has-gemma", echoInfo([]string{"gemma:2b"}, 1, false))
	b := e.computeNode("has-llama", echoInfo([]string{"llama3:8b"}, 1, false))
	e.startComputeWorkers([]*nodeClient{a, b}, echoEngine)

	run := func(model, content string) map[string]any {
		resp := e.openai("/v1/chat/completions", map[string]any{
			"model": model, "messages": []ChatMessage{{Role: "user", Content: content}}})
		if resp.StatusCode != 200 {
			resp.Body.Close()
			t.Fatalf("%s: HTTP %d", model, resp.StatusCode)
		}
		var d map[string]any
		decode(t, resp, &d)
		return d
	}
	run("gemma:2b", "x")
	run("llama3:8b", "y")
	// ikkala tugun ham mos ishlarni oldi — jobs orqali tekshiramiz emas, worker fieldini submit API bilan
	ra := e.submitAndWait(t, "gemma:2b", "faqat gemma")
	if ra["worker"] != "has-gemma" {
		t.Fatalf("gemma ishi noto'g'ri tugunga ketdi: %v", ra["worker"])
	}
	rb := e.submitAndWait(t, "llama3:8b", "faqat llama")
	if rb["worker"] != "has-llama" {
		t.Fatalf("llama ishi noto'g'ri tugunga ketdi: %v", rb["worker"])
	}
	// hech kimda yo'q model
	resp := e.openai("/v1/chat/completions", map[string]any{
		"model": "mistral:7b", "messages": []ChatMessage{{Role: "user", Content: "z"}}})
	resp.Body.Close()
	if resp.StatusCode != http.StatusServiceUnavailable {
		t.Fatalf("noma'lum model: %d", resp.StatusCode)
	}
}

func (e *env) submitAndWait(t *testing.T, model, content string) map[string]any {
	t.Helper()
	resp := e.admin("POST", "/api/compute/jobs", strings.NewReader(
		fmt.Sprintf(`{"model":%q,"messages":[{"role":"user","content":%q}]}`, model, content)))
	var sub struct {
		JobID string `json:"jobId"`
	}
	decode(t, resp, &sub)
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		var jr map[string]any
		r := e.admin("GET", "/api/compute/jobs/"+sub.JobID, nil)
		decode(t, r, &jr)
		if jr["status"] == "done" || jr["status"] == "failed" {
			return jr
		}
		time.Sleep(40 * time.Millisecond)
	}
	t.Fatalf("ish tugamadi: %s", sub.JobID)
	return nil
}

func TestComputeDistributionAcrossWorkers(t *testing.T) {
	e := newEnv(t, testComputeConfig)
	var workers []*nodeClient
	for i := 0; i < 5; i++ {
		workers = append(workers, e.computeNode(fmt.Sprintf("w-%02d", i), echoInfo([]string{"gemma:2b"}, 2, false)))
	}
	e.startComputeWorkers(workers, func(j cjob) engineResult {
		time.Sleep(60 * time.Millisecond) // ishni biroz cho'zamiz — yuk taqsimlansin
		return echoEngine(j)
	})

	const N = 40
	var wg sync.WaitGroup
	seen := sync.Map{}
	var okCount int64
	for i := 0; i < N; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			r := e.submitAndWait(t, "gemma:2b", fmt.Sprintf("so'rov-%d", i))
			if r["status"] == "done" {
				atomic.AddInt64(&okCount, 1)
				seen.Store(r["worker"], true)
				if r["result"] != fmt.Sprintf("so'rov-%d", i) {
					t.Errorf("natija mos emas: %v", r["result"])
				}
			}
		}(i)
	}
	wg.Wait()
	if okCount != N {
		t.Fatalf("%d/%d ish bajarildi", okCount, N)
	}
	used := 0
	seen.Range(func(_, _ any) bool { used++; return true })
	if used < 3 {
		t.Fatalf("yuk taqsimlanmadi — faqat %d tugun ishladi (5 dan)", used)
	}
	t.Logf("%d ish %d tugunga taqsimlandi", N, used)
}

func (c *nodeClient) pollOnce(t *testing.T) *cjob {
	t.Helper()
	var pr struct {
		Job *cjob `json:"job"`
	}
	code, err := c.postJSON("/api/nodes/"+c.ID+"/compute/poll", map[string]any{
		"models": c.Compute.Models, "engines": c.Compute.Engines, "allowPull": c.Compute.AllowPull}, &pr)
	if err != nil || code != 200 {
		t.Fatalf("poll: %d %v", code, err)
	}
	return pr.Job
}

func TestComputeWorkerFailureRequeue(t *testing.T) {
	e := newEnv(t, testComputeConfig)
	flaky := e.computeNode("flaky-node", echoInfo([]string{"gemma:2b"}, 1, false))
	good := e.computeNode("good-node", echoInfo([]string{"gemma:2b"}, 1, false))

	resp := e.admin("POST", "/api/compute/jobs", strings.NewReader(
		`{"model":"gemma:2b","messages":[{"role":"user","content":"qayta navbat"}]}`))
	var sub struct {
		JobID string `json:"jobId"`
	}
	decode(t, resp, &sub)

	// flaky tugun ishni oladi va requeue bilan rad etadi
	j1 := flaky.pollOnce(t)
	if j1 == nil || j1.ID != sub.JobID {
		t.Fatalf("flaky poll: %+v", j1)
	}
	var fr struct {
		Requeued bool `json:"requeued"`
	}
	flaky.postJSON("/api/compute/jobs/"+j1.ID+"/fail", map[string]any{"error": "model yuklanmadi", "requeue": true}, &fr)
	if !fr.Requeued {
		t.Fatal("ish qayta navbatga qo'yilmadi")
	}
	if st := e.submitStatus(t, sub.JobID); st != "queued" {
		t.Fatalf("requeue'dan keyingi holat: %s", st)
	}

	// sog'lom tugun endi o'sha ishni oladi va bajaradi
	j2 := good.pollOnce(t)
	if j2 == nil || j2.ID != sub.JobID {
		t.Fatalf("good poll: %+v", j2)
	}
	good.runComputeJob(context.Background(), *j2, echoEngine)

	r := e.waitJob(t, sub.JobID)
	if r["status"] != "done" || r["worker"] != "good-node" || r["result"] != "qayta navbat" {
		t.Fatalf("yakuniy: %+v", r)
	}
}

func (e *env) submitStatus(t *testing.T, jobID string) string {
	t.Helper()
	var jr map[string]any
	decode(t, e.admin("GET", "/api/compute/jobs/"+jobID, nil), &jr)
	s, _ := jr["status"].(string)
	return s
}

func (e *env) waitJob(t *testing.T, jobID string) map[string]any {
	t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		var jr map[string]any
		decode(t, e.admin("GET", "/api/compute/jobs/"+jobID, nil), &jr)
		if jr["status"] == "done" || jr["status"] == "failed" {
			return jr
		}
		time.Sleep(40 * time.Millisecond)
	}
	t.Fatalf("ish tugamadi: %s", jobID)
	return nil
}

func TestComputeStreamCancelOnDisconnect(t *testing.T) {
	e := newEnv(t, testComputeConfig)
	release := make(chan struct{})
	n := e.computeNode("slow", echoInfo([]string{"gemma:2b"}, 1, false))
	e.startComputeWorkers([]*nodeClient{n}, func(j cjob) engineResult {
		<-release // test signal bergancha ushlab turamiz
		return echoEngine(j)
	})
	ctx, cancel := context.WithCancel(context.Background())
	b, _ := json.Marshal(map[string]any{"model": "gemma:2b", "stream": true,
		"messages": []ChatMessage{{Role: "user", Content: "uzoq"}}})
	req, _ := http.NewRequestWithContext(ctx, "POST", e.ts.URL+"/v1/chat/completions", bytes.NewReader(b))
	req.Header.Set("Authorization", "Bearer "+testAdminToken)
	resp, err := e.ts.Client().Do(req)
	if err != nil {
		t.Fatal(err)
	}
	// birinchi (role) chunk keladi, keyin requester uziladi
	buf := make([]byte, 64)
	resp.Body.Read(buf)
	cancel()
	resp.Body.Close()
	close(release)
	// job bekor qilingan deb belgilanishi kerak (worker chunk'lari 409 oladi)
	time.Sleep(300 * time.Millisecond)
}

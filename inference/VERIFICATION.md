# Tekshirish natijalari

2026-10-28, Linux x86_64, Python 3.12.14, GCC, CPU-only llama.cpp commit `8ca4060b7e30e76baaa5670ee1ec1b760b7f8552`.

- `python3 -m unittest discover -s inference/tests -v`: 5 test o'tdi.
- `python3 inference/smoke_test.py --bin-dir /workspace/llama-build/bin --model /workspace/stories15M-q4_0.gguf`: o'tdi.
- Ikkita alohida loopback RPC worker ishga tushirildi; har ikkala endpoint uchun server logida model buffer ajratilgani tekshirildi.
- `Once upon a time` promptiga 16 token qaytdi: `, there was a little girl named Lily. She loved to play outside and`.
- Test tugagach worker va server jarayonlari to'xtagani tekshirildi.
- `git diff --cached --check`: xato yo'q.

Model SHA256: `66967fbece6dbe97886593fdbb73589584927e29119ec31f08090732d1861739`.

Bu ikkita fizik kompyuter yoki LAN tezligi sinovi emas. Windows/macOS bajarilishi uchun CI konfiguratsiyasi qo'shildi, bu lokal sinov ularni tekshirmadi. Backend fayllari o'zgarmadi. `go test ./...` va `go vet ./...` bajarilmadi: muhitdagi `/usr/bin/go` Go kompilyatori emas, Go stol o'yini dasturi ekan.

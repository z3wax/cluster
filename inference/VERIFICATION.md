# Tekshirish natijalari

2026-10-28, Linux x86_64, Python 3.12.14.

## Ko'p formatli CPU pipeline

```bash
TF_CPP_MIN_LOG_LEVEL=2 TF_NUM_INTRAOP_THREADS=1 TF_NUM_INTEROP_THREADS=1 OMP_NUM_THREADS=1 python3 -m unittest discover -s inference/tests -v
```

**65 test o'tdi, skip yoki xato yo'q** (22.8 soniya). Testlar kichik, lokal yaratilgan modellar bilan bajarildi; bu katta model benchmark'i emas.

- ONNX Runtime: haqiqiy ONNX graph CPU'da bajarildi. Ikki alohida worker jarayoni va coordinator orqali `y = 2*x + 1` natijasi tekshirildi.
- PyTorch: saqlangan TorchScript CPU'da bajarildi; oddiy state_dict avtomatik pickle model sifatida ochilmasligi tekshirildi.
- TensorFlow SavedModel, Keras `.keras` va `.h5`: haqiqiy saqlangan modellar CPU'da bajarildi.
- Hugging Face: lokal GPT2 Safetensors va `pytorch_model.bin`, Llama, Qwen2 va Mistral Safetensors checkpoint'lari haqiqiy TorchScript qatlamlariga ajratildi. Ajratilgan va to'liq model logits'lari turli context/batch uzunliklarida, jumladan bir tokenli inputda solishtirildi. Mistral sliding window ham tekshirildi.
- Hugging Face GPT2: ikki alohida TorchScript worker va coordinator orqali logits solishtirildi; tokenizer bilan `/v1/completions` va `/v1/chat/completions` ishladi.
- Multi-EOS, chat special-token takrorlanmasligi, model context chegarasi, tensor dtype/shape/hajmi, worker graph, autentifikatsiya va noto'g'ri so'rovlarga javoblar tekshirildi.
- Test tugagach worker va server jarayonlari to'xtadi.

Tekshirilgan paketlar: NumPy 2.3.5, CPU PyTorch 2.14.1, Transformers 4.57.6, ONNX 1.23.2, ONNX Runtime 1.31.1, TensorFlow 2.21.0 va Keras 3.15.1. HF eksportchi ishlatadigan Transformers versiyasi `requirements-hf.txt` da belgilangan.

## GGUF regressiyasi

CPU-only llama.cpp commit: `8ca4060b7e30e76baaa5670ee1ec1b760b7f8552`.

```bash
python3 inference/smoke_test.py --bin-dir /workspace/llama-build/bin --model /workspace/stories15M-q4_0.gguf
```

Sinov qayta bajarildi va o'tdi. Ikkita alohida RPC worker'da model buffer ajratilgani tekshirildi. `Once upon a time` promptiga 16 token qaytdi: `, there was a little girl named Lily. She loved to play outside and`.

Model SHA256: `66967fbece6dbe97886593fdbb73589584927e29119ec31f08090732d1861739`.

## Amaliy chegaralar

Bu ikki fizik kompyuter yoki LAN tezligi sinovi emas. Linux/macOS/Windows bazaviy testlari va Linux native runtime testlari uchun CI yangilandi; lokal natijalar Windows/macOS native runtime'lari bajarilganini bildirmaydi. CI natijalari bu lokal hisobotdan alohida.

Istalgan format/arxitekturani avtomatik bo'lish tekshirilmagan va va'da qilinmaydi. HF eksportchi qo'llaydigan oilalar aniq sanalgan; boshqa modellar uchun mos adapter va qismlarga ajratish kerak.

Qo'shimcha tekshiruvda topilgan inference va storage xatolari tuzatildi: [audit natijalari](../docs/BUGFIX_REVIEW.md). Alohida o'rnatilgan Go 1.27.3 toolchain bilan backend `test -race` va `vet` o'tdi; C++ mijoz Release build va CTest 1/1 o'tdi. `git diff --cached --check` ham bajarildi.

GitHub CI job'lari hisobdagi billing/spending limit sabab boshlanmagan; lokal natijalar CI yashil ekanini bildirmaydi.

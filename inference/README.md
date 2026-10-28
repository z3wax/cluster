# Bir modelni bir nechta kompyuter CPU'sida ishlatish

SDrive inference bitta modelning qismlarini turli kompyuterlarda bajaradi. GGUF uchun `llama.cpp` RPC, boshqa formatlar uchun tensorlarni worker'lar orasida uzatadigan pipeline backend ishlatiladi. Til, tasvir va boshqa sonli tensor bilan ishlaydigan modellarni ulash mumkin. Go storage serveri va C++ storage mijozi alohida ishlaydi.

## Format va runtime tanlash

| Model ko'rinishi | Backend / engine | Modelni taqsimlash |
|---|---|---|
| GGUF | `gguf` / llama.cpp RPC | Qatlamlarni runtime taqsimlaydi; [GGUF qo'llanmasi](GGUF.md) |
| ONNX `.onnx` | `pipeline` / `onnx` | Oldindan ajratilgan graph qismlari |
| TorchScript `.pt`, `.pth` | `pipeline` / `torchscript` | Oldindan ajratilgan scripted/traced modullar |
| TensorFlow SavedModel | `pipeline` / `tensorflow` | Har bir qismning SavedModel katalogi |
| Keras `.keras`, `.h5` | `pipeline` / `keras` | Har bir qismning saqlangan Keras modeli |
| Hugging Face `config.json` + `.safetensors` / `pytorch_model.bin` | HF eksporti → `torchscript` | Qo'llab-quvvatlangan arxitektura qatlamlarini eksportchi ajratadi |
| Boshqa format yoki xom checkpoint | `pipeline` / `python` | Model arxitekturasini biladigan adapter orqali |

Format faylning qanday saqlanganini bildiradi. Modelni bajarish uchun uning arxitekturasi va mos runtime ham kerak: masalan, oddiy `.pth` yoki `.safetensors` fayli o'zidan-o'zi bajariladigan graph emas. Istalgan modelni avtomatik ajratish kafolati yo'q. ONNX, TensorFlow va TorchScript qismlarining input/output tensorlari bir-biriga mos bo'lishi kerak; butun modelning nusxasini har bir worker'ga yuklash modelni taqsimlash hisoblanmaydi.

## Pipeline: ikki kompyuterda ishga tushirish

Har bir kompyuterga Python 3.10+ va ishlatiladigan engine bog'liqliklarini o'rnating. Faqat kerakli runtime'ni tanlang:

```bash
python -m pip install -r inference/requirements-onnx.txt
# yoki:
python -m pip install -r inference/requirements-torch.txt
python -m pip install -r inference/requirements-tensorflow.txt
```

CPU uchun PyTorch wheel'larini alohida o'rnatish mumkin: `python -m pip install torch --index-url https://download.pytorch.org/whl/cpu`. So'ng `requirements-torch.txt` qolgan bog'liqliklarni o'rnatadi.

Quyidagi misolda birinchi worker `encoder.onnx`, ikkinchisi `head.onnx` qismini bajaradi. [worker-encoder.json](examples/worker-encoder.json), [worker-head.json](examples/worker-head.json) va [pipeline.json](examples/pipeline.json) namunalari tayyor. Model yo'llari worker manifesti joylashgan katalogga nisbatan olinadi.

Birinchi kompyuterga `worker-encoder.json` va `encoder.onnx` fayllarini ko'chiring:

```bash
python inference/cluster.py worker --backend pipeline --manifest /models/worker-encoder.json --host 192.168.1.11 --port 50052 --threads 4
```

Ikkinchi kompyuterga `worker-head.json` va `head.onnx` fayllarini ko'chiring:

```bash
python inference/cluster.py worker --backend pipeline --manifest /models/worker-head.json --host 192.168.1.12 --port 50052 --threads 4
```

Asosiy kompyuterda pipeline manifestidagi manzillarni haqiqiy IP'larga almashtiring va serverni ishga tushiring:

```bash
python inference/cluster.py serve --backend pipeline --manifest /models/pipeline.json --port 8081
```

Worker manifestidagi `inputs` va `outputs` model tensor nomlaridir. Pipeline bosqichidagi mapping kaliti shu nom, qiymati pipeline ichidagi tensor nomi. Masalan, `"inputs": {"hidden": "encoder_result"}` worker'ning `hidden` inputiga oldingi bosqichdan kelgan `encoder_result` tensorini uzatadi. Oraliq tensorlar worker'lar orasida asosiy server orqali o'tadi; har bir worker faqat o'zidagi model qismini yuklaydi.

### Tayyor kichik ONNX misoli

Bu demo `y = 2*x + 1` hisobini ikki model qismiga ajratadi. Model fayllarini yaratish uchun `onnx` paketi ham kerak:

```bash
python -m pip install onnx
python inference/examples/create_onnx_demo.py --output onnx-demo
```

`onnx-demo/` ichida ikki model va uchta manifest yaratiladi. Yuqoridagi tartibda kompyuterlarga ko'chiring. Serverga tensor yuboring:

```bash
curl http://127.0.0.1:8081/v1/infer -H 'Content-Type: application/json' -d '{"inputs":{"x":{"dtype":"float32","shape":[1,2],"data":[[1,2]]}}}'
```

Natijada `y` tensori `[[3,5]]` bo'ladi. O'z modelingizda ham avval bir kompyuterdagi to'liq model natijasini pipeline natijasi bilan solishtiring.

## Hugging Face checkpoint'lari

`export_hf.py` **GPT2, Llama, Qwen2 va Mistral** oilalarining oddiy dense causal LM checkpoint'larini alohida TorchScript qismlariga ajratadi. GGUF'ga aylantirish talab qilinmaydi. Kvantlangan HF checkpoint, dynamic RoPE/LongRoPE, custom model kodi yoki boshqa arxitektura uchun mos `python` adapter kerak. Eksport paytida to'liq model vaqtincha FP32 ko'rinishida RAM'ga yuklanadi; keyin worker'lar faqat o'z qismlarini yuklaydi.

```bash
python -m pip install -r inference/requirements-hf.txt
python inference/export_hf.py --model /models/my-hf-model --output hf-parts --workers http://192.168.1.11:50052,http://192.168.1.12:50052
```

Eksport qilingan til modelining standart chiqishi faqat oxirgi pozitsiyaning logits'i (`[batch, 1, vocab]`); bu tarmoqdan o'tadigan ma'lumotni kamaytiradi. Butun ketma-ketlik logits'i kerak bo'lsa, eksportga `--full-logits` qo'shing.

Har bir kompyuterga tegishli `stage-N.pt` va `worker-N.json` ni bir katalogga ko'chiring, keyin `worker --backend pipeline --manifest worker-N.json` bilan boshlang. Asosiy kompyuterda eksport qilingan `pipeline.json` ishlatiladi. Agar manba modelda tokenizer bo'lsa, eksportchi uni `hf-parts/tokenizer/` ga saqlaydi:

```bash
python inference/cluster.py serve --backend pipeline --manifest hf-parts/pipeline.json --tokenizer hf-parts/tokenizer --port 8081
```

Tokenizer ulangan serverda `/v1/completions` orqali matn yuborish mumkin. `/v1/chat/completions` uchun tokenizer'da chat template ham bo'lishi kerak. Tokenizersiz `/v1/infer` sonli tensor API'si ishlaydi.

```bash
curl http://127.0.0.1:8081/v1/completions -H 'Content-Type: application/json' -d '{"prompt":"Salom","max_tokens":32,"temperature":0}'
```

Hozir matn API'si `temperature=0` (greedy), `max_tokens` 1–256 va bitta padding'siz ketma-ketlikni qo'llaydi. Streaming va KV cache yo'q; har bir yangi token uchun to'liq context qayta hisoblanadi. `--context` prompt va javob uchun umumiy limitni belgilaydi; server uni eksport manifestidagi model context chegarasi bilan ham cheklaydi.

## Boshqa modelni adapter orqali ulash

Worker manifestida `engine` ga `python`, `factory` ga o'rnatilgan Python modulidagi funksiya ko'rsatiladi:

```json
{
  "version": 1,
  "stages": [{
    "id": "custom_stage",
    "engine": "python",
    "factory": "my_model_adapter:load_stage",
    "inputs": ["x"],
    "outputs": ["hidden"],
    "model": "weights.safetensors",
    "options": {"architecture": "my_encoder"}
  }]
}
```

`load_stage` funksiya `config=...`, `root=Path(...)`, `threads=...` keyword argumentlarini qabul qiladi va `run(inputs)` metodi bor obyekt qaytaradi. `inputs` va natija `dict[str, numpy.ndarray]` bo'ladi. Funksiya model arxitekturasini yaratadi, mos loader bilan vaznlarni o'qiydi va CPU'da kerakli qismni bajaradi. Shu yo'l orqali xom PyTorch state_dict, safetensors, boshqa framework yoki format ulanadi. Adapter moduli worker'da o'rnatilgan va import qilinadigan bo'lishi kerak.

Pipeline transporti boolean, integer va floating tensorlarni tashiydi. Matn, rasm yoki audio faylini mos tensorlarga aylantirish modelning preprocessing qismida yoki mijozda bajariladi.

Model, manifest va adapter kodini administrator oldindan joylaydi. HTTP so'rov orqali yangi fayl yoki Python kod yuklanmaydi. Faqat ishonchli model artefaktlari va adapterlardan foydalaning.

## Ulanish, xotira va cheklovlar

Standart tinglash manzili `127.0.0.1`. Boshqa kompyuterlar uchun aniq LAN/VPN IP yozing. Pipeline uchun ixtiyoriy umumiy token `SDRIVE_INFERENCE_TOKEN` muhit o'zgaruvchisida worker'lar va asosiy serverda bir xil o'rnatiladi. Mijoz so'roviga `Authorization: Bearer ...` header'i qo'shiladi. HTTP tokenni shifrlamaydi: ishonchli LAN/VPN yoki HTTPS reverse proxy ishlating. GGUF RPC ulanishi alohida protokol; [uning tarmoq talablari](GGUF.md) farq qiladi.

Bu tayyor modelni bajarish — inference; trening emas. CPU worker'lar o'z qismining vaznlari va hisoblash buffer'lari uchun RAM talab qiladi. Bitta so'rovdagi tensorlarning jami xom hajmi 32 MiB, HTTP body limiti 64 MiB. Katta aktivatsiyalar uchun context yoki batch hajmini kamaytiring. Oraliq tensor hajmi va tarmoq tezligi natijaga ta'sir qiladi; ko'proq kompyuter doim tezroq degani emas. Worker uzilganda so'rov xato qaytaradi va `/health` 503 holatini beradi; avtomatik ko'chirish yoki model qayta bo'lish hozir yo'q.

## Tekshirish

```bash
python -m unittest discover -s inference/tests -v
```

Runtime talab qiladigan testlar tegishli paket bo'lmasa o'tkazib yuboriladi. Haqiqiy model sinovi natijalari [VERIFICATION.md](VERIFICATION.md) da qayd qilinadi. GGUF'ning ikkita worker smoke testi [alohida qo'llanmada](GGUF.md#tekshirish).

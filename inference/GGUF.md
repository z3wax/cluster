# GGUF: llama.cpp orqali CPU cluster

[Barcha backendlar qo'llanmasiga qaytish](README.md).

Bu backend bitta GGUF til modelining qatlamlarini `llama.cpp` RPC orqali bir nechta CPU worker orasida taqsimlaydi. Asosiy kompyuter modelni yuklaydi, worker'larga vaznlarni uzatadi va OpenAI formatidagi HTTP API beradi. Mavjud Go storage serveri va C++ storage mijozi alohida ishlaydi; ularni ishga tushirish CPU inference'ni avtomatik boshlamaydi.

## Talablar va yig'ish

Har bir qatnashuvchi kompyuterda Git, CMake 3.16+, C++17 kompilyator va Python 3.10+ kerak. Windows'da Visual Studio C++ Build Tools, Linux'da GCC/Clang ishlating. Barcha kompyuterlarda **bir xil commit** yig'ilsin; RPC protokoli versiyalar orasida mos kelmasligi mumkin.

Tekshirish uchun tanlangan llama.cpp commit: `8ca4060b7e30e76baaa5670ee1ec1b760b7f8552`.

```bash
git clone https://github.com/ggml-org/llama.cpp.git
git -C llama.cpp checkout 8ca4060b7e30e76baaa5670ee1ec1b760b7f8552
cmake -S llama.cpp -B llama.cpp/build -DCMAKE_BUILD_TYPE=Release -DGGML_RPC=ON -DGGML_CUDA=OFF -DGGML_METAL=OFF -DGGML_VULKAN=OFF -DGGML_SYCL=OFF -DLLAMA_BUILD_TESTS=OFF
cmake --build llama.cpp/build --config Release --target ggml-rpc-server llama-server -j 4
```

Quyidagi buyruqlarni `cluster/` katalogidan bajaring. `--bin-dir` haqiqiy binary katalogi bo'lsin; Visual Studio build'da odatda `llama.cpp/build/bin/Release`. Mac'da ham Metal o'chirilgani uchun CPU ishlatiladi.

## 1. Worker kompyuterlar

Misolda LAN IP'lari `192.168.1.11` va `192.168.1.12`. Har birida o'z IP manzilini yozing:

```bash
python3 inference/cluster.py worker --bin-dir ../llama.cpp/build/bin --host 192.168.1.11 --port 50052 --threads 4
```

Ikkinchi kompyuterda `--host 192.168.1.12` bo'ladi. Windows'da `python3` o'rniga `python` ishlatish mumkin. Model fayli faqat asosiy kompyuterda kerak. Worker'dagi `--threads` o'sha kompyuter CPU ishchi oqimlari sonini belgilaydi; standart qiymat mantiqiy CPU'larning yarmi.

**RPC autentifikatsiya va TLS bermaydi, upstream uni tajribaviy deb belgilagan.** Faqat ishonchli LAN/VPN ishlating; worker portiga firewall orqali faqat asosiy kompyuterni kiriting. Portni internetga ochmang. Standart bind `127.0.0.1`; boshqa kompyuterdan ulanish uchun LAN IP'ni aniq ko'rsatish kerak.

## 2. Asosiy kompyuter

O'zingiz ishlatish huquqiga ega bo'lgan GGUF model faylini tayyorlang. Ikki worker uchun:

```bash
python3 inference/cluster.py serve --bin-dir ../llama.cpp/build/bin --model /models/model.gguf --workers 192.168.1.11:50052,192.168.1.12:50052 --tensor-split 1,1 --context 2048 --port 8081
```

`--tensor-split 1,2` ikkinchi worker'ga taxminan ikki hissa ko'p model qatlamlari ajratadi. Qatlamlar butun holda taqsimlanadi; juda kichik modelda barcha worker'ga qatlam tushmasligi mumkin. Flag berilmasa upstream mavjud xotiraga qarab taqsimlaydi. Launcher worker TCP portlarini tekshiradi; haqiqiy RPC mosligi va xotira yetarliligini llama-server model yuklashda tekshiradi. Worker uzilsa avtomatik tiklash yo'q: worker'ni, keyin model serverini qayta ishga tushiring.

`--n-gpu-layers 999` upstream'ning RPC qurilmalariga qatlamlarni yuborish flagidir. Worker `--device CPU` bilan ishlagani va yig'ishda GPU backend'lar o'chirilgani sababli hisoblash CPU'da bajariladi. Server logida **ikkala RPC manziliga model buffer/qatlamlari ajratilganini** tekshiring.

API: `http://127.0.0.1:8081`. Go backend'ning `8080` porti bilan to'qnashmaydi. Model yuklanib tayyor bo'lgach:

```bash
curl http://127.0.0.1:8081/health
curl http://127.0.0.1:8081/v1/chat/completions -H 'Content-Type: application/json' -d '{"messages":[{"role":"user","content":"Salom!"}],"max_tokens":64}'
```

Chat uchun modelning chat template'i kerak. Oddiy completion modelida `/completion` ga `{"prompt":"Once upon a time","n_predict":32}` yuboring. Boshqa kompyuterga HTTP API ochish uchun `serve --host LAN_IP` ishlating va kirishni o'z tarmog'ingizda cheklang.

## Cheklovlar

Bu inference (tayyor modeldan javob olish); trening emas. RAM sarfiga vaznlar, KV cache va hisoblash buffer'lari kiradi. Asosiy kompyuter ham modelni yuklash va lokal operatsiyalar uchun RAM talab qiladi; bu yechim unda model umuman sig'masligini kafolatlamaydi. Avval kichik quantized model va kichik context bilan sinang. Qatlamlar ketma-ket hisoblanadi, shuning uchun qo'shimcha kompyuter har doim tezlikni oshirmaydi. Gigabit Ethernet Wi-Fi'dan afzal.

## Tekshirish

```bash
python3 -m unittest discover -s inference/tests -v
python3 inference/cluster.py worker --bin-dir ../llama.cpp/build/bin --dry-run
```

`--dry-run` jarayonni boshlamasdan buyruq argumentlarini JSON shaklida chiqaradi. Unit testlar launcher validatsiyasini tekshiradi; haqiqiy tarqatilgan hisoblash uchun ikkita worker, model yuklash loglari va muvaffaqiyatli completion kerak.

Upstream: https://github.com/ggml-org/llama.cpp/tree/8ca4060b7e30e76baaa5670ee1ec1b760b7f8552/tools/rpc

Ikkita lokal RPC jarayoni bilan haqiqiy model smoke testi:

```bash
python3 inference/smoke_test.py --bin-dir ../llama.cpp/build/bin --model /models/stories15M-q4_0.gguf
```

Test har ikkala worker'da model buffer'i borligini va bo'sh bo'lmagan completion qaytishini tekshiradi; yakunda jarayonlarni to'xtatadi. CI shu testni kichik TinyStories modeli bilan Linux'da bajaradi. Bu test alohida jismoniy kompyuterlar orasidagi tarmoq tezligini o'lchamaydi.

# Xatolar tekshiruvi va tuzatishlar

2026-10-28. Inference, Go storage backend va C++ HTTP mijozi tekshirildi. Quyidagi holatlar test yoki real jarayon bilan takrorlandi va tuzatildi.

| Muammo | Tuzatilgan xulq | Dalil |
|---|---|---|
| Launcher to'xtasa native model CPU'da qolib ketardi | SIGTERM/SIGINT child jarayoniga uzatiladi, u to'xtatiladi va reap qilinadi | Haqiqiy subprocess regressiyasi |
| RPC URL/IPv6 yoki bir xil worker turli yozilishda qabul qilinardi | Pinned RPC qo'llamaydigan manzillar oldindan rad etiladi; host/port normallashtiriladi | Launcher regressiyalari |
| Worker uzilganda `/health` hali ham `ok` edi | Haqiqiy worker holati tekshiriladi, uzilganda 503 qaytadi | HTTP regressiyasi |
| Buzilgan worker javobi caller xatosi bo'lib ko'rinardi | Upstream protokol/aloqa xatosi 502 va cheklangan diagnostika bilan qaytadi | Partial HTTP, noto'g'ri JSON/tensor javoblari |
| Shared output buffer parallel so'rovda o'zgarib ketardi; aktivatsiyalar RAM'da to'planardi | Javob lock ichida nusxalanadi; oraliq tensor oxirgi iste'molchidan keyin bo'shatiladi | Buffer va weakref regressiyalari |
| Keras nomlangan kirishlar almashib, `10-2` o'rniga `2-10` hisoblanardi | Saqlangan input nomlari/strukturasi asosida bog'lanadi | Haqiqiy Keras dict/list modellari |
| BF16 va boshqa endian tensorlar native runtime'da xato berardi | BF16/float8 natijalar float32 ga, input byte-order native tartibga o'tadi | TorchScript va TensorFlow native testlari |
| SavedModel signature xatosi birinchi so'rovdagina bilinardi; NaN logits javobni buzardi | Signature startup'da tekshiriladi; yaroqsiz logits rad etiladi | Native model va text regressiyalari |
| Eksport yozishda xato yarim model qoldirib, qayta urinishni to'sardi | HF eksport staging katalogida tayyorlanib, to'liq bo'lgach joylanadi | Yozish xatosini kiritish va muvaffaqiyatli retry |
| Upload uzilgan yoki size noto'g'ri bo'lsa ham qabul qilinardi | Hajm/read xatosi tekshiriladi, muvaffaqiyatsiz chunk katalogi tozalanadi | Go upload/download testlari |
| Parallel DB Save bitta `.tmp` faylda to'qnashardi; o'qish/yozish xatolari yashirilardi | Save ketma-ket bajariladi; o'qish xatosi qaytadi; saqlanmagan mutation 500 beradi | Go parallel-save va persistence-failure testlari |
| JSON javobi yuborilayotganda live pointer'lar o'zgarardi | Javob uchun lock ostida snapshot olinadi | `go test -race` |
| Chunk URL'dagi kodlangan `../` katalogdan chiqishi mumkin edi | File/chunk metadata va identifikator tekshiruvi filesystem lookup'dan oldin bajariladi | Traversal va unregistered chunk regressiyasi |
| C++ mijoz chunked JSON'ni dekodlamasdi, qisqa send/read'ni qabul qilardi | HTTP chunk framing yechiladi, qisman send davom ettiriladi, buzilgan/truncated javob rad etiladi | Loopback socket regressiyalari va to'liq Release build |

## Tekshiruv natijalari

Linux x86_64, Python 3.12.14, Go 1.27.3, GCC 14.2.0, CMake 4.4.4.

```bash
# Tegishli optional native runtime'lar o'rnatilgan muhitda:
TF_CPP_MIN_LOG_LEVEL=2 TF_NUM_INTRAOP_THREADS=1 TF_NUM_INTEROP_THREADS=1 OMP_NUM_THREADS=1 python3 -m unittest discover -s inference/tests -v
# 65 test: OK, skip yo'q

go -C backend test -race ./...
go -C backend vet ./...
# Ikkalasi o'tdi; 7 top-level regression test va subcase'lar

cmake -S client -B build/client -DCMAKE_BUILD_TYPE=Release
cmake --build build/client -j 2
ctest --test-dir build/client --output-on-failure
# To'liq client yig'ildi; CTest 1/1 o'tdi
```

Ikki worker'li haqiqiy GGUF smoke testi ham qayta o'tdi. Model fayllari, shaxsiy DB va tayyor binary'lar commitga kiritilmadi. Native modellar kichik lokal fixture'lar edi; fizik kompyuterlar va LAN ishlashi bu sinov bilan o'lchanmagan.

GitHub Actions'dagi [tekshirilgan run](https://github.com/z3wax/cluster/actions/runs/40641239361) job'lari boshlanmagan: GitHub annotatsiyasi hisobdagi to'lov/spending limit muammosini ko'rsatdi. Bu lokal test xatosi emas; CI yashil deb belgilanmaydi. Storage uchun Go race/vet va C++ CTest workflow'i ham qo'shildi.

## Qolgan chegaralar

DB yozish xatosi endi yashirilmaydi, ammo xotiradagi mutation avtomatik rollback qilinmaydi. 500 javobini tranzaksiya bekor bo'lganining kafolati deb qabul qilmang. Inference'ning qo'llab-quvvatlanadigan arxitekturalari va LAN/VPN ishlatish talablari [qo'llanmada](../inference/README.md) saqlangan; bu audit istalgan model yoki internetga ochiq production konfiguratsiyasi uchun umumiy kafolat bermaydi.

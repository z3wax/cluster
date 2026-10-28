# Hissa qo'shish

## Ish tartibi

1. Mavjud issue va hujjatlarni tekshiring. Katta o'zgarishdan oldin maqsad va ta'sirni tushuntiring.
2. Joriy default branchdan alohida branch yarating.
3. Bitta muammoni hal qiladigan kichik, izchil diff tayyorlang.
4. O'zgargan xulqqa tegishli hujjat va zarur tekshiruvlarni yangilang.
5. Pull requestda muammo, natija va tekshirish dalillarini yozing.

## cluster uchun tekshirish

`backend/` ichida `go test ./...` va `go vet ./...` bajaring. Mijoz o'zgarishida operatsion tizim, CMake va kompilyatorni ko'rsating.

```bash
cmake -S client -B build/client -DCMAKE_BUILD_TYPE=Release
cmake --build build/client -j 2
ctest --test-dir build/client --output-on-failure
```

Inference o'zgarishlari uchun mos runtime paketlarini o'rnatib `python -m unittest discover -s inference/tests -v` bajaring; [inference qo'llanmasi](inference/README.md) kerakli bog'liqliklarni ko'rsatadi.

Tekshiruvni bajarmagan bo'lsangiz, buni aniq yozing. Muayyan muhitga ehtiyoj borligini muvaffaqiyatli test deb ko'rsatmang.

## Pull request tarkibi

- O'zgarishning maqsadi va foydalanuvchiga ta'siri.
- Qaysi fayl yoki ichki loyihaga tegishli ekani.
- Tekshirish buyruqlari va real natijalari.
- Zarur bo'lsa UI rasmi yoki minimal API misoli.
- Moslik, ma'lumot formati va migratsiya talablari.

## Ma'lumotlar va mualliflik

Haqiqiy token, parol, session, mijoz ma'lumotlari yoki maxfiy bazalarni yubormang. Log va rasmlardan maxfiy qiymatlarni olib tashlang. Uchinchi tomon litsenziyalari va mualliflik bildirishnomalarini saqlang; yangi materialning kelib chiqishi va foydalanish huquqini ko'rsating.

Mavjud litsenziya ushbu qo'llanma bilan o'zgarmaydi. Alohida litsenziya berilmagan qismlar uchun qayta tarqatish huquqini taxmin qilmang.

**Repozitoriy egasi:** [z3wax](https://github.com/z3wax)

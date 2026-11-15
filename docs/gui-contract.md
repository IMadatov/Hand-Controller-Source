# Hand Controller GUI — kontrakt va parallel ish shartlari

Sana: 2026-11-15
Branch: `hand-controller-gui` (worktree: `../Hand-Controller-GUI`)
Base: `hand-controller-cpp-phase2` @ `81c0d6d`

Bu hujjat GUI sessiyasi uchun **yagona haqiqat manbai**. Engine (C++) tomoni
parallel sessiyada rivojlanmoqda; bu yerdagi hech narsa `cpp/` ichiga tegmaydi.

> **Rollar:** egasining qarori bilan **C++ ishi parallel sessiyada**
> (`Hand-Controller-Source`, branch `hand-controller-cpp-phase2`), **GUI esa
> shu worktree'da** quriladi. Oldingi tahrirdagi aksincha taqsimot bekor.

---

## 1. Stack qarori

**Flutter desktop (Linux birinchi), Dart FFI orqali engine'ga ulanadi.**

Nega: Design mockupiga yaqin natija beradi, Linux/Windows/macOS bitta koddan
chiqadi, va keyinchalik mobilga yo'l ochiq qoladi — bu phase 2 amendmentida
yozilgan maqsadga mos (`extern "C"` shim bitta artefaktdan .NET P/Invoke,
Flutter FFI va mobilni ochadi).

Flutter **3.47.5 stable** shu mashinada mavjud: `~/development/flutter/bin`
(PATH'da emas, qo'lda qo'shiladi). `flutter doctor` Linux desktop
toolchain'ni ✓ deb tasdiqlaydi. `cpp/` uchun amal qiladigan "clean clone +
cmake yetarli" qoidasi Flutter SDK'ga taalluqli emas — u alohida toolchain.

---

## 2. Qat'iy izolyatsiya qoidalari

1. **GUI kodi faqat `gui/` papkada.** Top-level yangi papka.
2. **`cpp/` ichida hech narsa o'zgartirilmaydi.** Bitta fayl ham, hatto
   `CMakeLists.txt` ham. Agar engine tomonidan biror narsa kerak bo'lsa —
   egasiga aytiladi, u phase2 sessiyasida bajariladi.
3. **Root `CMakeLists.txt` ham tegilmaydi** (FFI ulanishi kelgunga qadar).
4. **`main.py` tegilmaydi** — global konstraint.
5. `docs/superpowers/plans/` va `docs/superpowers/specs/` **read-only** —
   ular phase2 sessiyasi egaligida.

Shu 5 qoida buzilmasa, merge og'riqsiz bo'ladi.

---

## 3. Hozir qilinadigan ish (engine kutmaydi)

| # | Ish | Holat |
|---|---|---|
| A1 | Flutter SDK + `gui/` skeleton, Linux desktop target | ochiq |
| A2 | `Design/Hand Controller — Desktop App.html` dan ekranlar, tema, navigatsiya | ochiq |
| A3 | Commands & Bindings editor formasi (vocabulary 4-bo'limdan) | ochiq |
| A4 | Mock engine layer: fake landmarklar, fake `Command` oqimi, fake start/stop | ochiq |

**A4 eng muhim arxitektura qarori:** GUI engine'ga bitta Dart interface
orqali gaplashadi (masalan `HandControllerEngine`), va uning ikki
implementatsiyasi bo'ladi — `MockEngine` (hozir) va `FfiEngine` (keyin).
UI widgetlari FFI haqida bilmasligi kerak.

## 4. Vocabulary — qotgan, o'zgartirilmaydi

Bu nomlar C++ tomonida **published interface**, bindings faylida ishlatiladi.
GUI ularni aynan shu yozilishda ko'rsatishi/yozishi kerak.

**Gesture** (`GestureKindName`, `cpp/control/include/hc/gesture_event.hpp`):
`pinch`, `fist`, `open_palm`, `point_index`, `two_fingers`, `thumb_up`,
`pinky_only`, `four_fingers`, `none`

Har bir nom **pozani** ataydi, harakatni emas. `two_fingers` default
konfiguratsiyada clickka bog'langan, lekin nomi shunga ishora qilmaydi —
chunki har qanday poza har qanday komandaga bog'lanishi mumkin.

**Phase**: `began` | `held` | `ended`
- `began` — bir martalik (click, screenshot)
- `held` — har kadrda (kursor, scroll)
- `ended` — qo'yib yuborish (key_release)

**Command** (`CommandKindName`, `cpp/control/include/hc/command.hpp`):
`none`, `move_cursor`, `click`, `double_click`, `scroll`, `key_press`,
`key_release`

**Button**: `left` | `right` | `middle` — `click`/`double_click` uchun
ixtiyoriy, default `left`.

**Key** (`KeyName`): `arrow_up`, `arrow_down`, `arrow_left`, `arrow_right`,
`print_screen`, `escape`, `enter`, `space`, `tab`
`key_press`/`key_release` uchun **majburiy**.

**Hand selector**: `any` | `left` | `right`
`any` — Left, Right va **Unknown** ni ham qamraydi. Bu muhim: model
handedness ishonchi 0.575 gacha tushishi o'lchangan, ya'ni `Unknown` tez-tez
keladi. GUI'da `any` birinchi/default variant bo'lishi kerak va "unlabelled
qo'lni ham qamraydi" deb tushuntirilishi lozim.

> `Unknown` — bu qo'lning tavsifi, foydalanuvchi yozadigan narsa emas.
> Shuning uchun u bindings UI'da **tanlov sifatida ko'rsatilmaydi**.

### Bindings fayl formati

```
<hand> <gesture> <phase> -> <command> [arg]
set sensitivity 2.0
set smoothing 0.5
```

To'liq namuna va izohlar: `cpp/config/default_bindings.txt`.

- `sensitivity` — strictly positive, default `2.0`
- `smoothing` — `[0, 1]`, default `0.5`
- Ikkisi ham **tanlangan, o'lchanmagan** (build mashinasida kamera yo'q).
  GUI ularni "tune qilinadigan" deb ko'rsatishi kerak, "to'g'ri qiymat" deb
  emas.

### Validatsiya qoidasi — UI buni oldindan ushlashi kerak

> Har bir `key_press` uchun `ended` fazasida mos `key_release` bo'lishi shart,
> aks holda konfiguratsiya **rad etiladi**.

Yopilib qolgan modifikator klavishi — bu loyihadagi eng xavfli nosozlik.
UI `key_press` qo'shilganda mos `key_release` ni avtomatik yaratishi, yoki
yo'qligini xatolik sifatida ko'rsatishi kerak. Saqlashdan keyin C++ parseri
rad etishini kutib o'tirish — yomon UX.

---

## 5. Serializerni GUI'da QAYTA YOZMANG

Phase 2 plani, Amendment Change 2:

> Bindings UI exists to *edit* bindings and save them, so a writer is needed;
> if it is not written here, the UI reimplements the format and the two
> definitions drift.

C++ tomonida `ParseBindings` va `SerializeBindings` mavjud (`81c0d6d` da
landed) va round-trip test bilan qoplangan. GUI oxir-oqibat **shu
funksiyalarni FFI orqali chaqiradi**.

Shuning uchun: A3 (editor formasi) UI-ni va in-memory modelni qursin, lekin
**haqiqiy fayl o'qish/yozishni mock ortida qoldirsin**. Dart'da to'liq
parser/serializer yozish — bu aynan plan ogohlantirgan ikkilanish.

---

## 6. Keyinroq — bloklangan ishlar

Bular **Task 11 (`Controller`) + `extern "C"` shim** kelgandan keyin:

- Live kamera preview, landmark overlay
- Haqiqiy Start / Stop
- Haqiqiy bindings o'qish/yozish
- Per-stage timing / metrics paneli

Task 11 `Controller`ning kutilayotgan shakli (plandan, hali yozilmagan):

```
Start / Stop / PollObservations / PollCommands
```

Va GUI uchun ahamiyatli kafolatlari:

- **Sessiyaga bitta `Controller`; singleton yo'q, global state yo'q.**
- **Sink ulanmagan `Controller` to'liq ishlashi kerak** — `Command`larni
  oddiy data sifatida qaytaradi. GUI uchun bu "preview/sinov rejimi":
  gestureni ko'rsatish, lekin haqiqiy kursorni qimirlatmaslik. Bu qo'llab-
  quvvatlanadigan konfiguratsiya, hack emas.

`MockEngine` interfeysini aynan shu shaklga moslab qursangiz, `FfiEngine`
keyin to'g'ridan-to'g'ri o'rniga tushadi.

> `extern "C"` shim **Task 11 landed bo'lmaguncha yozilmaydi** — plan buni
> ochiq taqiqlaydi: o'rab olinadigan barqaror narsa hali yo'q.

---

## 7. Xavf: X11 jim ishdan chiqishi

GUI'ga ham tegishli. XTest **jim** ishdan chiqadi: success qaytaradi va hech
narsa qilmaydi. Shuning uchun GUI'da "Engine ishlayapti" degan indikator
sink'ning **haqiqatan ishlayotganini** ko'rsatishi kerak, komanda yuborilgan
faktini emas. Sink mavjud emasligini `Create()` xabar qiladi — GUI buni
ko'rinadigan holat sifatida ko'rsatsin ("input injection unavailable"), jim
yutmasin.

---

## 8. Holat, 2026-11-15 (GUI sessiyasi)

Qurilgani (branch `hand-controller-gui`):

| Qism | Fayl | Holat |
|---|---|---|
| Flutter Linux desktop, 1280×800 | `gui/` | ✅ |
| Inter + Space Grotesk bundle | `gui/assets/fonts/` | ✅ |
| Design tokenlari | `gui/lib/theme/` | ✅ |
| Kontrakt ko'zgusi (wire nomlar) | `gui/lib/domain/` | ✅ |
| `HandControllerEngine` + `MockEngine` | `gui/lib/engine/` | ✅ |
| Live View | `gui/lib/screens/live_view.dart` | ✅ |
| Commands & Bindings | `gui/lib/screens/bindings_screen.dart` | ✅ |
| Settings, Diagnostics | `gui/lib/screens/` | ✅ |
| Onboarding, Tray Overlay | — | ⏳ |
| `FfiEngine` | — | ⛔ Task 11 ni kutadi |

29 ta test o'tadi. Ekranlar `gui/test/goldens/*.png` ga render qilinadi
(`flutter test --update-goldens test/screenshots_test.dart`) — bu mashinada
Wayland va screenshot utiliti yo'qligi uchun ko'rish usuli shu.

### Engine sessiyasiga uchta savol

Bular GUI qurilishida chiqdi va **C++ tomonida qaror talab qiladi**:

1. **O'chirilgan (disabled) binding'ni format ifodalay olmaydi.** Mockup'da
   har satrda Enable tugmasi bor, lekin `default_bindings.txt` da "mavjud,
   lekin o'chiq" degan holat yo'q. Hozir GUI uni saqlashda **o'chirib
   yuboradi** va buni oldindan ogohlantiradi. Agar bu noto'g'ri bo'lsa,
   formatga izoh bilan saqlash (`# disabled: ...`) yoki alohida bayroq
   kerak — va u `SerializeBindings` ichida bo'lishi shart.

2. **Settings'dagi hamma narsa `BindingSet` ichida emas.** `sensitivity` va
   `smoothing` — ichida. Lekin resolution, detection threshold, scroll
   sensitivity, panic hotkey — yo'q. Bular engine konfiguratsiyasimi
   (`Controller` qabul qiladimi), yoki sof GUI preference'larimi? Hozir GUI
   ularni o'zida saqlaydi va **hech qayerga yozmaydi**.

3. **Sessiya hamon Wayland.** Phase 2 plani "egasi X11 ga o'tadi" deb
   yozgan; `XDG_SESSION_TYPE=wayland` hali ham shunday. Task 9 ning
   kursorni qaytarib o'qiydigan testi shu holatda **yiqilishi kerak** —
   bu kutilgan natija, tuzatish uchun test emas.

### FfiEngine kelganda o'zgaradigan yagona joy

`gui/lib/main.dart` dagi `buildEngine()`. Boshqa hech narsa.

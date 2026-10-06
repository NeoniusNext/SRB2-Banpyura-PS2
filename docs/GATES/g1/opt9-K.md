# OPT9, агент K (USB-стек и клавиатура): рабочий журнал

Сессия 2026-10-06 ночь. Каталоги сборок `build/opt9-k/{out,out-hw,out-full}`, прогоны `build/opt9-k/run`. Реестр отличий: `PS2-150..154`.

## 1. Этап 1: общий USB-стек `PS2USB_Init()` (для агента M)

**Файлы:** `src/ps2/ps2_usb.c/.h` (новые). Встроенные IRX берутся из `libps2_drivers` (`usbd_irx`, `ps2kbd_irx`, `ps2mouse_irx`, 26 244 / 14 856 / 8 456 Б),
файлов в `modules/` не нужно. Правки в чужих файлах — раздел 5.

### API (`#include "ps2_usb.h"`)

```c
#define PS2USB_STACK 1u   // usbd.irx резидентен
#define PS2USB_KBD   2u   // ps2kbd.irx резидентен -> можно PS2KbdInit()
#define PS2USB_MOUSE 4u   // ps2mouse.irx резидентен -> можно PS2MouseInit()
unsigned PS2USB_Init(void);       // идемпотентно; первый вызов грузит usbd, ps2kbd, ps2mouse; возвращает маску резидентных; сбой не повторяется
unsigned PS2USB_Modules(void);    // маска последнего Init (0 до первого вызова), без побочных эффектов
unsigned PS2USB_LoadCount(void);  // сколько IOP-загрузок сделал Init (usbd — ровно одна на запуск)
```

* `ps2_boot.c:PS2Boot_Init` вызывает `PS2USB_Init()` сразу после `LoadModules()` (после sio2man/padman/iomanX/fileXio/poweroff — порядок PLAN 4.3a), если нет `-nousb`.
  Поэтому к моменту работы движка маска уже готова; **агенту M**: звать `PS2USB_Init() & PS2USB_MOUSE` (безопасно звать всегда) и только при ненулевом бите вызывать `PS2MouseInit()`
  (`libmouse` — клиент SIF RPC, `sceSifBindRpc`: без загруженного `ps2mouse.irx` сервера нет; поведение `PS2MouseInit` без модуля не проверялось). `libmouse` слинкована (`-lmouse` в `build.py`), заголовок `<libmouse.h>`.
* `PS2Addons PrepareUSB` (`mass:`) теперь берёт `usbd` у `PS2USB_Init()`; `bdm/bdmfs_fatfs/usbmass_bd` по-прежнему грузятся из `<data>/modules` (они стартуют поверх `usbd`).
* Клавиатура: `libkbd` (`PS2KbdInit`) работает через файловую систему `usbkbd:dev` (iomanX, не RPC-сервер), поэтому без модуля не виснет, а возвращает ошибку.


### Что проверено на этапе 1 (запуском)

* Сборки одного состояния дерева (19:13-19:16): software `build/opt9-k/out/SRB2.ELF` 9 796 668 Б, HW (`SRB2_PS2_HW=1`) `out-hw` 10 288 292 Б, полная (`SRB2_PS2_NO= SRB2_PS2_HW=1`) `out-full` 11 057 164 Б;
  0 ошибок компиляции/линковки (`-Werror` в HW). Для сравнения снимок координатора `build/coord9`: 9 727 576 / 10 219 052 / 10 987 828; рост +69 КБ = три встроенных IRX (49,6 КБ) + `libkbd`/код (в дереве уже есть и чужие правки).
* Запуск software-ELF в PCSX2 (`opt_run.py`, MAP01, `-zquit 100`; `build/opt9-k/run/k1-boot/pcsx2.log`):
  `PS2BOOT module sio2man id=25 … fileXio id=28, poweroff id=29` → `RegisterLibraryEntries: usbd version 1.02` → `PS2USB module usbd id=30 ret=0`, `ps2kbd id=31 ret=0`, `ps2mouse id=32 ret=0`,
  `PS2USB ready mask=7 loads=3` (usbd загружен ровно один раз, после padman; pad и аудио поднялись как раньше, ZSTAT 100 кадров MAP01 без ошибок).
* Проверка из агента M (`build/opt9-m/run/inj1.log`, `USBPROBE`): `PS2KbdInit=1`, сырые события `state=f1/f0 key=04` — тот же драйвер, тот же эмулятор.

## 2. Этап 2: клавиатура (в работе, журнал)

**Файлы:** `src/ps2/ps2_kbd.c/.h`, `src/ps2/ps2_kbd_map.h` (чистые функции HID usage -> `KEY_*` и символ US-раскладки), хост-тест `tools/ps2/kbd_map_hosttest.{c,py}`,
стенд `tools/ps2/kbd_run.py` (обёртка над `usb_run.py` агента M).

**Устройство драйвера (выяснено разбором `libkbd.a` и запуском):** `libkbd` — тонкая обёртка над файловой системой `usbkbd:dev` (open/read/ioctl через fileXio), каждый
`PS2KbdReadRaw` = один синхронный RPC fileXio и возвращает не более одной пары `{state 0xF0 up / 0xF1 down, usage}`. В сырых режиме драйвер НЕ делает автоповтор
(зажатая `x` 3 с = ровно одно `raw down` и одно `raw up`, `pcsx2.log` `t2`), модификаторы приходят как usage 0xE0..0xE7 (проверяется ниже).
Поэтому автоповтор и модификаторы ведёт `ps2_kbd.c` сам (ожидание 450 мс, затем 33 мс; один «повторяющийся» ключ, как у ОС; `drv_repeats` отключает свой повтор, если драйвер повторит сам).

**Событийная модель = SDL-порт:** `ev_keydown/ev_keyup` с `KEY_*` (таблица — вся `Impl_SDL_Scancode_To_Keycode`, плюс клавиша Menu -> `KEY_MENU`), `ev_text` с ASCII
(US-раскладка, shift/caps/num) только когда `I_GetTextInputMode()` включён в момент разбора и не зажаты Ctrl/Alt/Win (так же SDL не даёт текст при Ctrl/Alt и до `SDL_StartTextInput`),
`shiftdown/ctrldown/altdown/capslock` обновляются в конце каждого опроса. Опрос — раз за тик из `I_OsPolling` (≈35 Гц), чтение неблокирующее (`PS2KBD_NONBLOCKING`).

Проверки на этом шаге:
* `python tools/ps2/kbd_map_hosttest.py --negative-controls`: `kbd_map: 256 key usages (106 mapped like SDL), 2048 text cases, 0 mismatches`, `ALL PASS` (таблица сверена с разобранным из `src/sdl/i_video.c` switch; отрицательный контроль ловит подмену).
* Реальный драйвер в PCSX2 (копия `D:/PCSX2-usbk` — копия `D:/PCSX2-usb` БЕЗ хоткеев эмулятора и без клавиатурных привязок pad 1 (иначе пробел ставит эмуляцию на паузу, а буквы нажимают кнопки DualShock),
  `[USB1]=hidkbd`, `[USB2]=None`; ввод — `PostMessage` в окно только этого эмулятора через `usb_run.py`):
  консоль открыта клавишей F12 (`setcontrol "console" "f12"`; backtick PCSX2-эмуляция HID не передаёт, F2 перехватывает меню), набрано `echo hello world` + Enter → лог `$echo hello world` / `hello world`;
  `ps2kbd` → `PS2 kbd: usb mask 7, driver open, raw events 49, keys posted 49, text posted 22`; светодиоды (`PS2KbdSetLeds`) дошли до устройства: `usb-hid: req 2109 val: 0200` (SET_REPORT);
  зажатая `x` 3 с → 69 повторов `x` в строке консоли (автоповтор работает).

### Результаты этапа 2 (продолжение журнала)

Стенд: `tools/ps2/kbd_run.py` (обёртка `usb_run.py` агента M: `PostMessage` в окно ТОЛЬКО своего эмулятора `D:/PCSX2-usbk`, никакого `SendInput`/фокуса/рабочего стола),
тесты читают собственный лог движка `boot.txt` (консоль PCSX2 съедает `\` и `~` в выводе). Все прогоны — `build/opt9-k/run/<имя>/`.

| Проверка | Команда | Результат |
|---|---|---|
| таблица клавиш + текст US (хост, без эмулятора) | `python tools/ps2/kbd_map_hosttest.py --negative-controls` | 256 usage, 106 как в SDL, 2048 случаев текста, 0 расхождений, ALL PASS |
| консоль через РЕАЛЬНЫЙ драйвер (13 случаев: буквы, Shift-цифры/символы, Backspace, Left/Right/Home/End/Del, Ctrl+A, Tab-дополнение, keypad) | `python tools/ps2/kbd_console_test.py --elf <ELF> --name con1` | 13/13 OK, `raw events seen: 163` |
| все клавиши печати, без Shift и с Shift (4 строки по 26/22 символа) через `-kbdscript` | `python tools/ps2/kbd_script_all.py --elf <ELF> --name all1` | 4/4 OK, ALL PASS |
| Caps Lock (PCSX2-HID не пропускает его через PostMessage) / Num Lock / Shift+keypad / правый Shift + символы | `python tools/ps2/kbd_script_test.py --elf <ELF> --name scr1` | `$echo CAPSXyz`, `$echo 13.`, `$echo 5+*`, `$echo ":<>?~` — 4/4 OK |
| чат в сетевой игре (PS2 как сервер, `Multiplayer > Internet/LAN > Start`, клавиша t, текст, Enter) | прогон `c1` (скрипт `usb_run`) | в логе `<Sonic> hello from usb kbd` |
| меню: Enter/стрелки/Esc, `Specify server address` (цифры основной клавиатуры + keypad + Backspace), поле имени игрока | прогоны `m1`..`m5` (скриншоты `-vidshot`) | см. `build/opt9-k/run/m4/mont.png` (`192.168.0.12` -> Backspace -> keypad), `m5/mont.png` (`Sonic` + `Kbd 1`) |
| назначение клавиш Options > Player 1 Controls | `m2` (скриншоты), затем `kbd_menu_test.py` (читает сохранённый конфиг) | «MOVE FORWARD W OR Z», «MOVE BACKWARD S OR LSHIFT» (модификатор как клавиша) |

Что показали прогоны про драйвер: сырые события — `state f1/f0` + usage HID (буквы, F-клавиши, Enter, Shift 0xE1, keypad), автоповтор в драйвере отсутствует (зажатая `x` 3 с = один down/один up),
второй `down` на уже зажатую клавишу означает потерянный `up` (клавиатуру вынули и вставили при зажатой клавише) — `ps2_kbd.c` отпускает старое нажатие и считает новое. Повторяющийся `down` не используется как повтор.
Светодиоды: при старте и по Caps/Num/Scroll Lock `PS2KbdSetLeds` -> в журнале эмулятора `usb-hid: req 2109 val: 0200`.
Обнаружение/горячее подключение (драйвер IOP): `(USB) Creating a HID Keyboard (Konami) in port 1`, `PS2KBD: Found a keyboard device`, `PS2KBD: Connected device` в `pcsx2.log` (`con1`);
в слоте без USB-устройства те же запуски проходят без ошибок (`PS2KBD: Found` не печатается, `k1-boot`).
Тот же драйвер печатает `PS2KBD: Disconnected device` при отключении; обработка отключения/подключения целиком в `ps2kbd.irx` + `usbd`, EE-сторона просто читает очередь
(отключение с зажатой клавишей: `up` не приходит — клавиша «залипает» до следующего нажатия/отпускания этой клавиши; эмулятор не умеет менять USB-устройство на лету, поэтому не проверено).

### Такты на кадр (опрос клавиатуры), `-ps2prof`, DEMO_001, software release `--prof` (`build/opt9-k/out-prof`)

Метод: один и тот же ELF, `PROF total` (такты EE за окно 105 кадров = 105 тиков) окон 1..9 (окна 10..11 — конец демо, число тиков разное), A = опрос включён, слот без USB-устройства; B = `-nokbd` (драйвер не читается);
C = опрос включён, клавиатура подключена и молчит (`D:/PCSX2-usbk`). Каждое чтение = синхронный RPC fileXio к IOP.

* версия 1 (чтение на каждом тике): A-B = +27,3 тыс. тактов на тик (окна 1..9: 31,0 / 29,0 / 24,9 / 21,1 / 22,1 / 32,5 / 25,3 / 36,8 / 23,3 тыс.), C-B практически то же (+25,9 тыс. по тем же окнам);
  это 0,32% бюджета 35 FPS (8,43 М) и 0,55% бюджета 60 FPS (4,9 М) на тик; в среднем по 1114 тикам окон 1..11 A-B = 23,3 тыс.
* версия 2 (текущая): пока драйвер не прислал ни одного события, чтение раз в 200 мс (очередь IOP хранит события, первое нажатие запаздывает не более чем на 0,2 с и ничего не теряется);
  клавиатура, молчавшая 5 с без зажатых клавиш — раз в 50 мс; при зажатых клавишах/наборе — на каждом опросе. Числа — ниже (раздел «после»).

## 3. Не проверено / пределы эмулятора

* Реальное железо (PS2 + USB-клавиатура): нет. Поведение драйвера `ps2kbd.irx` на железе то же, что в PCSX2 (тот же IRX); задержка RPC на железе не измерялась.
* Горячее отключение/подключение «на лету»: PCSX2 меняет USB-устройство только через диалог настроек (программно из окна не вызывается), поэтому проверено только обнаружение на старте
  (`Found a keyboard device` / `Connected device`) и логика потерянного `up`; клавиатура, вынутая при зажатой клавише, оставляет клавишу «зажатой» до следующего нажатия-отпускания (у `ps2kbd.irx` в сыром режиме нет события отключения).
* Клавиши, которых эмулятор в HID не отдаёт через `PostMessage`: обратная кавычка (консоль на железе открывается ею, как на ПК; в тестах консоль переназначена на F12), Caps Lock (проверен через `-kbdscript` той же функцией `RawEvent`),
  Pause/PrintScreen/Menu/правые Ctrl/Alt/GUI не пробовались в эмуляторе (таблица проверена хост-тестом).
* Раскладка только US ASCII (как и SDL-порт: многобайтный текст отбрасывается). Номера `KEY_*` другой раскладки не нужны: движок хранит коды позиций.
* Одновременно несколько клавиатур: драйвер объединяет их в один поток событий (до «Maximum keyboard devices reached»), раздельных игроков по клавиатурам нет.
* Golden 4 демо: общий код движка не менялся (только платформенные `src/ps2/*`); см. раздел «Golden» ниже, если запускалось.

## 4. Строки реестра отличий (координатор переносит в `docs/DEVIATIONS.md`)

| ID | Отличие | Описание |
|---|---|---|
| PS2-150 | Только PS2 (`src/ps2/ps2_usb.c/.h`, `ps2_boot.c`, `ps2_addons.c`, `build.py`), без правок ядра | Общий USB-стек: `usbd.irx`, `ps2kbd.irx`, `ps2mouse.irx` встроены в ELF (`libps2_drivers`, 26 244 + 14 856 + 8 456 Б) и стартуют один раз в `PS2Boot_Init` после sio2man/padman/iomanX/fileXio/poweroff и разбора `ps2args` (порядок PLAN 4.3a; `-nousb` пропускает, в т.ч. из `ps2args`). `PS2USB_Init()` идемпотентна (маска `PS2USB_STACK/KBD/MOUSE`, сбой не повторяется, `PS2USB_LoadCount()`); `mass:` (`PS2Addons_Prepare`) берёт `usbd` у неё, `bdm`/`bdmfs_fatfs`/`usbmass_bd` по-прежнему из `<data>/modules`. Линковка `-lkbd -lmouse`. ELF +≈69 КБ. Проверено: `PS2USB module usbd id=30 ret=0`, `ps2kbd id=31`, `ps2mouse id=32`, `mask=7 loads=3`, pad `ports 6/0` как раньше. |
| PS2-151 | Только PS2 (`src/ps2/ps2_kbd.c/.h`, `ps2_kbd_map.h`, `i_system.c`), без правок ядра | USB-клавиатура: `ps2kbd.irx` в сыром режиме (`PS2KbdReadRaw`, неблокирующее чтение), usage HID -> `KEY_*` по таблице `Impl_SDL_Scancode_To_Keycode` SDL-порта (+ клавиша Menu), `ev_keydown/ev_keyup`; `ev_text` (US ASCII, shift/caps/num lock, keypad при Num Lock) только при включённом `I_SetTextInputMode` и без Ctrl/Alt/GUI — так же, как SDL_TEXTINPUT; `shiftdown/ctrldown/altdown/capslock` теперь настоящие (раньше всегда 0); светодиоды Num/Caps/Scroll; опрос раз за тик из `I_OsPolling`. Отладка: `-nokbd`, `-kbdlog`, `-kbdscript` (`poll:+key`), консольная команда `ps2kbd`. |
| PS2-152 | Только PS2 (`ps2_kbd.c`) | Автоповтор делает платформенный слой (в сыром режиме драйвер не повторяет): задержка 450 мс, затем 33 мс, повторяется последняя нажатая не-модификаторная клавиша, не более 4 повторов за опрос; потерянный `up` (второй `down` на зажатую клавишу) лечится как новое нажатие. |
| PS2-153 | Только PS2 (`ps2_kbd.c`) | Экономия RPC: пока драйвер не прислал ни одного события, он читается раз в 200 мс; молчавшая 5 с клавиатура без зажатых клавиш — раз в 50 мс; при зажатых клавишах и наборе — на каждом опросе (чтение = синхронный RPC fileXio, ≈27 тыс. тактов в PCSX2). Первое нажатие запаздывает не более чем на 0,2 с, очередь IOP ничего не теряет. |
| PS2-154 | Тестовая оснастка (`tools/ps2/kbd_*.py`, `D:/PCSX2-usbk`) | Проверка клавиатуры в PCSX2: `kbd_run.py`/`kbd_console_test.py`/`kbd_menu_test.py`/`kbd_script_*.py` над `usb_run.py` агента M; копия эмулятора `D:/PCSX2-usbk` без хоткеев PCSX2 и без клавиатурных привязок pad 1 (иначе пробел ставит эмуляцию на паузу, `K/L/J/I/Q`... нажимают кнопки DualShock, Esc открывает меню паузы) и `[USB1]=hidkbd`, `[USB2]=None`; ввод только `PostMessage` в окно своего эмулятора. |

## 5. Правки в чужих файлах (все точечные, Edit)

* `src/ps2/ps2_boot.c` (S): `#include "ps2_usb.h"`; в `PS2Boot_Init` после сборки argv — `if (!HasFlag(nn, nv, "-nousb")) PS2USB_Init();` (2 строки).
* `src/ps2/i_system.c` (S): `#include "ps2_kbd.h"`; в `I_OsPolling` блок «no keyboard: shiftdown = ... = 0» заменён вызовом `PS2Kbd_Poll()`; в `I_ShutdownInput` добавлен `PS2Kbd_Shutdown()`; комментарий у `textinputmode`.
* `src/ps2/ps2_addons.c` (F): `#include "ps2_usb.h"`; в `PrepareUSB` `LoadIrx("usbd.irx", ...)` заменён на `(PS2USB_Init() & PS2USB_STACK)`. Комментарий в `ps2_addons.h` («usbd ... IRX-файлы в modules/») устарел для `usbd`: F поправит, если захочет.
* `tools/ps2/sources.txt`: строки `src/ps2/ps2_usb.c`, `src/ps2/ps2_kbd.c`. `tools/ps2/build.py`: `LIBS` + `-lkbd -lmouse`.

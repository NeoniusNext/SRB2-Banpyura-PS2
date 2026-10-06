# OPT9, агент M (USB-мышь и стенд USB-ввода): рабочий журнал

Сессия 2026-10-06 ночь. Каталоги сборок `build/opt9-m/{out,out-hw,out-full,dev}`, прогоны `build/opt9-m/run`. Реестр отличий: `PS2-155..159`.

## 1. Этап 1: стенд USB-ввода (для K и M)

### Что сделано
* **Эмулятор `D:/PCSX2-usb`** — копия `D:/PCSX2-test` (146 МБ, без logs/snaps/sstates/cache/videos), в `inis/PCSX2.ini` (только в нём):
  `[USB1] Type = hidkbd` (HID-клавиатура), `[USB2] Type = hidmouse` (HID-мышь), кнопки мыши привязаны к клавишам хоста
  (`hidmouse_LeftButton = Keyboard/F13`, `hidmouse_RightButton = Keyboard/F14`, `hidmouse_MiddleButton = Keyboard/F15`), `[InputSources] Mouse = false`
  (никакого raw-input реальной мыши пользователя). У копии свой lock `pcsx2-run-pcsx2-usb.lock`; `D:/PCSX2-test`, `s1..s3`, `net1/2` не тронуты.
  Имена типов и ключей PCSX2 2.6.3 взяты из исходников `pcsx2/USB/usb-hid/usb-hid.cpp` (тип `hidmouse`: привязки `Pointer`, `LeftButton`, `RightButton`,
  `MiddleButton`; ключ в ini `<тип>_<привязка>` → `hidmouse_LeftButton`; `hidkbd` принимает ВСЕ клавиши хоста без настройки).
* **`tools/ps2/usb_run.py`** — запуск эмулятора (окно скрыто, как в `run_pcsx2.py`) + скрипт ввода по времени от строки лога `--ready`.
  Ввод: **только `PostMessageW` в HWND дисплейного окна этого же процесса** (окно ищется по PID: класс `Qt6…QWindowIcon`, заголовок = имя ELF + ` [?]`).
  Нет `SendInput`/`keybd_event`/`mouse_event`/`SetCursorPos`/`SetForegroundWindow`, ни одного сообщения чужим окнам. Команды: `key:NAME[:down|up]`, `type:TEXT`,
  `btn:left|right|middle[:down|up]` (= клавиши F13/F14/F15 → кнопки HID-мыши), `sleep:SEC`, а с `--pointer` ещё `mv:`/`mvto:`/`wheel:`. Опции `--nopad`
  (`[Pad1] Type = None` на время прогона: иначе стрелки/WASD/Return эмулятора одновременно идут как кнопки виртуального DualShock), `--set СЕКЦИЯ.КЛЮЧ=ЗНАЧЕНИЕ`
  (временная правка ini копии; ini восстанавливается до освобождения lock).
* **`tools/ps2/usb_engine.py`** — запуск ELF движка в стенде: готовит каталог прогона как `opt_run.py`, аргументы пишет в `<run>/ps2args` (PCSX2 режет `-gameargs` на ~128
  символах), печатает нужные строки лога. **`tools/ps2/usb_probe.c` + `build_usb_probe.py`** — отдельный ELF-зонд (грузит `usbd/ps2kbd/ps2mouse` с `host:`, печатает отчёты
  мыши/клавиатуры и такты RPC).

### Доказательства (прогоны в `build/opt9-m/run`, лог `exp*.log`)
* Драйверы грузятся, устройство видно: `USBPROBE module host:usbd.irx id=25 ret=0`, `ps2kbd id=26`, `ps2mouse id=27`, `PS2KbdInit=1`, `PS2MouseInit=1`,
  **`mouse count 1`** (с `USB2.Type=None` — `mouse count 0`, с двумя `hidmouse` — `mouse count 2`).
* **Клавиатура** через `PostMessage` в окно эмулятора доходит до `ps2kbd.irx`: `A` → `state=f1 key=04`, `B` → `05`, Enter → `28`, `Shift+h` → `e1`, `0b`; отпускание `f0`.
* **Кнопки мыши** (через привязку к F13..F15): левая → `btn=1`, правая → `btn=2`, средняя → `btn=4`, отпускание → `btn=0`.
* Колесо проходит как `wheel=+1` (вверх) / `wheel=-1` (вниз), но только через привязку `Pointer` (см. ниже: она небезопасна).
* Цена RPC в PCSX2 (EE-такты, cop0 Count, среднее по 200): `PS2MouseRead` 15 110, `PS2MouseEnum` 14 423, `PS2KbdReadRaw` 24 785; без мыши — те же (15 445 / 14 266 / 24 882).

### Предел эмулятора: движение мыши и колесо нельзя подать безопасно
Исходники `DisplayWidget.cpp`/`InputManager.cpp` PCSX2 2.6.3 (прочитаны): HID-мышь получает X/Y/колесо только через привязку `hidmouse_Pointer = Pointer-0`. Наличие такой
привязки включает у окна эмулятора **relative-режим**: при старте ВМ `SetCursorPos` в центр окна (реальный курсор пользователя), а каждое сообщение `WM_MOUSEMOVE` заставляет сам PCSX2
прочитать `GetCursorPos` (настоящий курсор), посчитать дельту относительно центра и снова `SetCursorPos(центр)`. Т.е. «дельта» = положение настоящего курсора, и любое сообщение
двигает рабочий стол пользователя. Это зафиксировано измерением (мои первые прогоны с привязкой `Pointer`: курсор пользователя прыгал в центр скрытого окна, значения X/Y были случайными:
−127, −254…). Поэтому:
* профиль стенда по умолчанию **без `Pointer`**; кнопки мыши и клавиатура проверяются на настоящем пути «окно эмулятора → HID → usbd → ps2mouse/ps2kbd → engine»;
* движение/колесо — режим движка `-usbtest` (HID-отчёты через тот же код преобразования, что и живые) + ручная проверка пользователем (раздел «Ручная проверка»);
* `--pointer` оставлен как явный opt-in для машины, за которой никто не работает (он двигает курсор!).
* Честная оговорка: до того как это выяснилось, я сделал ~8 запусков с привязкой `Pointer` — в эти моменты (старт эмулятора и каждое посланное `WM_MOUSEMOVE`) курсор пользователя мог один раз
  перескакивать в центр скрытого окна. Больше так не делаю.

### Как пользоваться стендом (для K)
```
python tools/ps2/usb_engine.py --name t1 --elf build/optX/out/SRB2.ELF --ready "PS2 kbd: usbkbd driver open" --script "2:key:grave;3:type:echo hi;4:key:enter" --until "hi" --nopad -- -skipintro -warp 1
python tools/ps2/usb_run.py --elf FILE --log LOG --args "..." --script "T:cmd,cmd;T:cmd" [--ready TEXT] [--until TEXT] [--nopad] [--set USB1.Type=hidkbd]
```
`T` — секунды после появления текста `--ready` в логе (по умолчанию 3 с после появления окна). `kbd_run.py` агента K уже вызывает `usb_run.py` и работает как есть.

## 2. Этап 2: мышь `ps2_mouse.c` (PS2-155), хуки, `-usbtest`

### Что сделано
* **`src/ps2/ps2_mouse.c/.h`** (новые): `PS2Mouse_Startup/Poll/Shutdown/GetCursor`. Хуки в `src/ps2/i_system.c` (владелец S; точечные правки, раздел 5): `I_StartupMouse/I_StartupMouse2` (колбэки `use_mouse`/`use_mouse2`),
  `I_GetMouseEvents` → `PS2Mouse_Poll`, вызов `I_GetMouseEvents()` в `I_OsPolling` (как в SDL-порту; один раз за тик в `Local_Maketic`), `I_GetCursorPosition` → виртуальный указатель
  (сумма сдвигов мыши 1, зажатая в размер экрана), `PS2Mouse_Shutdown` в `I_ShutdownInput`. Строка `src/ps2/ps2_mouse.c` в `tools/ps2/sources.txt`.
* Драйвер: `PS2USB_Init()` (K) уже грузит `ps2mouse.irx`; `PS2MouseInit` вызывается лениво при первом опросе, если резидентен бит `PS2USB_MOUSE`, режим `PS2MOUSE_READMODE_DIFF`,
  `PS2MouseSetThres(0)`/`PS2MouseSetAccel(1.0f)` (никакого ускорения драйвера: чувствительность — `mousesens` движка). Без `use_mouse`/`use_mouse2` (оба Off) — ни одного обращения к IOP.
* Преобразование в события — как в SDL (`src/sdl/i_video.c`): один `ev_mouse` за опрос со суммой сдвигов, кнопки `ev_keydown/ev_keyup` с `KEY_MOUSE1 + n` (левая 0, правая 1, средняя 2),
  колесо — `ev_keydown KEY_MOUSEWHEELUP/DOWN` без keyup (до 4 за опрос), `gamekeydown` колеса сбрасывается в начале следующего опроса (как в конце `I_GetEvent` SDL).
  Знак: `dx > 0` вправо, `dy > 0` вниз (HID), колесо `> 0` вверх — как отдаёт драйвер для HID-мыши PCSX2 (измерено: прокрутка вверх → `wheel=+1`).
* Горячее подключение: `PS2MouseEnum` раз в секунду (35 опросов); при `n -> 0` все удерживаемые кнопки отпускаются (нейтральные keyup), при `0 -> n` один холостой `PS2MouseRead` (накопленное до первого
  опроса — не движение игрока); сообщения `PS2 mouse: N mice plugged in`. Консольная команда `ps2mouse` (состояние, средние такты опроса и чтения). `-mouselog` — лог чтений и реакции движка;
  `-nomouse` / `-nousb` — драйвер не открывается.
* **Назначение игроков.** `ps2mouse.irx` сливает все мыши в один поток (измерено, см. раздел 3): поток получает игрок 1, если `use_mouse` On, иначе игрок 2, если `use_mouse2` On
  (одна мышь + пад в сплитскрине). Отдельной второй мыши драйвер не даёт.
* Меню: «Control Setup → Mouse Options» уже есть в движке и работает на PS2 (проверено снимком: Use Mouse, First/Third-Person Mouselook, Mouse Move, Invert Y Axis, Mouse X/Y
  Sensitivity — `build/opt9-m/v4.png`); «Second Mouse Options» для игрока 2 — `v5.png`. Строка «Second Mouse Serial Port» (`cv_mouse2port`, наезжала на значение и на PS2 бессмысленна)
  убрана под `#ifndef PS2` в `src/m_menu.c` (PS2-157). ЛКМ = Enter, ПКМ = Esc в меню — код движка (`M_Responder`), работает без правок.
* **`-usbtest`** (`-usbtest file:NAME` — `<HOME>/NAME`, либо строка со строками через `;`): записанные HID-отчёты (кнопки, x, y, колесо; десятичные знаковые 8 бит) идут через ТУ ЖЕ функцию `Apply`, что и живые данные;
  формат `<опрос> <устройство 0|1> <b> <x> <y> [<w>]`, `C <опрос> <n>` (число мышей, горячее подключение), `S any|level|menu` (когда начинается отсчёт опросов). Отчёты одного опроса
  суммируются как в diff-режиме драйвера, два устройства сливаются в один поток (как реальный драйвер).

### Доказательства (динамическая сборка без LTO, `build/opt9-m/dev`, прогоны `build/opt9-m/run/m*`, лог-фильтр `PS2 mouse`)
* **Движение** (`mtest1.txt`, 50 опросов `dx=+10`, потом 30 `dy=-8`): `localangle` 536870912 → 305004544 (поворот вправо, 4.6° на опрос при `mousesens 20`); контроль без ввода — угол неизменен.
* **Mouselook + колесо** (`mtest2.txt`, `chasemlook On`, `setcontrol "jump" "wheel 1 up"`): `dy<0` → `localaiming` растёт (вверх до ограничения движка), `dy>0` → падает до -897581056; колесо вверх →
  `z` игрока растёт 9→32→0 (прыжок), колесо вниз прыжка не даёт; повторное колесо вверх — второй прыжок.
* **Кнопки**: живой путь через PCSX2 (окно эмулятора → HID → usbd → ps2mouse → `Apply` → движок): `buttons 1/2/4` ↔ флаги `L1/R1/M1` в `gamekeydown`, отпускание → `L0/R0/M0`, аккорд ЛКМ+ПКМ (`buttons 3`).
* **Мышь 2** (`mtest3.txt`, `-splitscreen`, `use_mouse Off`, `use_mouse2 On`): меняется только `angle2` (498138136…), `angle` неизменен; левая кнопка → `gamekeydown[KEY_2MOUSE1]` = 1.
* **Горячее подключение** (`mtest4.txt`): «0 mice plugged in» → удерживаемая ЛКМ отпущена (`L1 → L0`), «1 mice plugged in» → снова работает.
* Цена опроса: см. раздел 3 (пока: чтение `PS2MouseRead` ≈ 15 000 EE-тактов за тик в PCSX2).

## 3. Этап 3: цена опроса (такты на кадр), golden, второй мышь

### Такты на кадр `-ps2prof` (релиз `--prof`, `build/opt9-m/out-prof`, DEMO_001 `-timedemo`, software, стенд `D:/PCSX2-usb`; окна 1..9 по 105 кадров, `tools`: `build/opt9-m/profsum.py`)
| Запуск | такты/кадр | к `-nousb` |
|---|---|---|
| `-nousb` (нет USB-стека, нет клавиатуры/мыши) | 9 901 563 | 0 |
| `-nomouse` (USB-стек + опрос клавиатуры K, мышь выключена) | 9 927 140 | +0.26 % (клавиатура K: ~25 К тактов на опрос) |
| мышь есть в движке, **устройства нет** (`USB2.Type=None`) | 9 929 093 | +0.28 % (= +0.02 % к `-nomouse`: `PS2MouseEnum` раз в секунду) |
| мышь есть и **подключена** (простаивает), `use_mouse On` | 9 950 752 | +0.50 % (= **+0.24 % к `-nomouse`**, ≈ 23.6 К тактов на тик: один блокирующий RPC `PS2MouseRead`) |
Бюджет кадра 60 FPS = 4.9 М тактов: опрос подключённой мыши ≈ 0.5 % этого бюджета, без мыши — 0.04 %. Движение мыши цену не меняет (один и тот же RPC).
Микро-замер RPC в зонде (`usb_probe.c`, cop0 Count, среднее по 200): `PS2MouseRead` 15 110, `PS2MouseEnum` 14 423, `PS2KbdReadRaw` 24 785 тактов.

### Golden (DEMO_001, `--ps2ref`, `build/opt9-m/out-ref`, `ftest_golden.py`)
`golden DEMO_001: 32 files, 3 differ: frame-000560.idx, frame-000665.idx, frames.csv` — **те же три файла, что у базовой сборки F без моих правок** (см. `opt9-F.md` §3: известные 1-байтные отличия PS2-81;
`ref-base`/`ref-full` дают ту же тройку). Прямое сравнение моего `refout` с `build/opt9-f/run/g-base2-DEMO_001/refout`: все `tics.csv`/`frames.csv`/`frame-*.idx` совпадают, отличаются только
`memory-end.csv`/`memory-level.csv` (разные бинарники). Мои правки в ядре — только строка меню `#ifndef PS2` (PS2-157) и PS2-файлы: картинку и тики они не затрагивают.

### Вторая мышь для игрока 2: упор в `ps2mouse.irx`
Стенд с двумя `hidmouse` (`USB1.Type=hidmouse`, кнопки первой на F13..F15, второй на F16..F18; `build/opt9-m/run/exp7.log`): `PS2MouseEnum = 2`, но `PS2MouseRead` отдаёт ОДИН поток — кнопки обеих мышей
складываются по OR (F13 и F16 вместе → `btn=1`, отпускание F13 при зажатой F16 → `btn=1`), движение суммируется. Драйвер ps2sdk не различает устройства (в RPC нет номера мыши). Поэтому в `ps2_mouse.c` один поток
направляется игроку 1 (`use_mouse` On) либо, если он выключен, игроку 2 (`use_mouse2` On): в сплитскрине «мышь + пад» работает с любой из сторон, две независимые мыши — нет. Разделение потребовало бы
собственного IRX-драйвера (usbd LDD для HID-мыши: пер-устройство состояние + RPC) — не делал: на реальной консоли его не проверить (эмулятор отдаёт только «идеальный» HID), а ошибка IOP-кода вешает консоль.
`-usbtest` умеет два устройства (строки `<опрос> 1 …`), но сливает их в один поток так же, как драйвер.

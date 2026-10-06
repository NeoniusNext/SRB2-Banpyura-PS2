# OPT5, агент H: GS Hardware Renderer (отчёт, ведётся инкрементально)

Сессия 2026-10-05. Каталоги: `build/opt5-h/{out,out2,run}`; сборка `bash build/opt5-h/mk.sh` / `env2.sh`+`bh.py` (HW, релиз), запуск `python build/opt5-h/run.py NAME --elf ELF --until TXT -- <engine args>`.
Эмулятор: PCSX2 2.6.3 из `D:\PCSX2-test`, **GS-рендерер Vulkan (Renderer=14)** — картинка реально растеризуется, окно скрыто.
Новые отладочные средства (i_video.c, только HW-часть): `-hwstats N` теперь печатает строки `HW flip` (счётчики показа: `gs_frames`, `vblanks`, `flips`, `dropped`, `flipwait`, `forced`, `wd`) и `HW state` (+`titlemap`, `menu`); `-vidkeys` (уже был) нажимает клавиши по номеру кадра.

## 0. Состояние на старте
* `tools/ps2/build.py` строка 276 была сломана (реальный перевод строки внутри `'\n'.join`, след OPT4): исправлено (одна строка, файл принадлежит C — изменён только этот литерал).
* `src/ps2/i_video.c` `Impl_HWStats`: тот же дефект (`"...totalframes=%d<LF>"`) — исправлен.
* Оба варианта собираются (HW релиз: 122 файла, ELF ~8.74 МБ).

## 1. Застывание картинки в GS HW (главный баг)

### 1.1 Воспроизведение и причина (измерено)
Сначала проверена гипотеза «меню поверх 3D» (`-warp 1 -vidkeys 300:esc`, 4000 кадров): счётчик кадров растёт, зависания драйвера нет.
Но счётчик кадров не доказывает, что картинка *показывается*: драйвер отдаёт кадр на экран только в обработчике vblank. Добавлены счётчики показа (`flips`).
Прогон «интро пропущено (`-vidkeys 150:enter`) → титульный экран с 3D-картой», старый драйвер, `-hwstats 50`:

```
HW flip frame  200: gs_frames=200  vblanks=700   flips=150  dropped=49
HW flip frame  250: gs_frames=250  vblanks=855   flips=171  dropped=78
HW flip frame  300: gs_frames=300  vblanks=1084  flips=172  dropped=127
HW flip frame  600: gs_frames=600  vblanks=3306  flips=176  dropped=423
HW flip frame 1000: gs_frames=1000 vblanks=6644  flips=183  dropped=816
HW flip frame 1500: gs_frames=1500 vblanks=10802 flips=191  dropped=1308
```
Как только на экране тяжёлая 3D-сцена (кадр ≈ 250), **flips перестаёт расти (191 показ на 1550 кадров, dropped 1357)**: картинка застыла, хотя счётчик кадров идёт, исключений/таймаутов нет.

Причина (`ps2_hw_gs.inc`): `vblank_handler` показывал кадр только если в момент vblank **бит FINISH в GS_CSR ещё взведён**. Но `frame_begin` следующего кадра сам ждёт FINISH
(`gs_wait_finished`) и тут же его сбрасывает (`*GS_CSR = 2`). Если GS рисует кадр дольше, чем EE готовит следующий (тяжёлая 3D-сцена, меню/оверлей с множеством мелких полигонов — любой кадр, где ЕЕ ждёт GS),
FINISH приходит в произвольный момент и снимается в пределах десятков микросекунд — окно, в которое должен попасть vblank, исчезающе мало. Каждый кадр объявлялся «заменённым до показа» (`dropped++`), `DISPFB2` не менялся: экран показывал последний кадр, нарисованный до начала тяжёлой сцены.
Лёгкие кадры (2D-интро, где EE медленнее GS) флипались нормально — поэтому дефект проявлялся именно при появлении 3D.

### 1.2 Исправление (PS2-HW-21) и watchdog
* `H.pend_done`: кадр «полностью нарисован» запоминается тем, кто первым увидел FINISH (`gs_wait_finished` у EE или vblank-обработчик). Обработчик показывает кадр при `pend && (pend_done || FINISH)`.
* `frame_begin`: если завершённый кадр ещё не показан, EE ждёт (опрос `DelayThread(100)`, до 125 мс) его показа в ближайший vblank и только потом рисует в этот буфер; если vblank-прерывание не пришло — показывает кадр сам (`flip_forced`). Кадры больше не теряются тихо.
* Watchdog (`wd_diag`/`wd_recover`): таймаут `ring_wait` (GIF DMA) и `gs_wait_finished` (FINISH) раньше делал `SleepThread(); abort()` — **тихое вечное зависание**. Теперь печатается строка `HWD WATCHDOG: <причина> | frame_no, frame_open, finish_pending, pend, pend_done, disp, target | vbl, flips, flip_vbl, irq_seen | pending, busy, q_n, cur, wr, D2_CHCR/MADR/QWC, GIF_STAT, GS_CSR`,
  канал 2 DMA останавливается, очередь буферов сбрасывается, игра продолжает со следующего кадра; после 6 сбросов — `I_Error` с диагностикой. Счётчики `timeouts`, `wd_recoveries` в `HW flip`.

### 1.3 Проверка после исправления (титул с 3D-картой, `e4.ELF`, 1500 кадров)
```
HW flip frame  100: gs_frames=100  vblanks=516   flips=99   dropped=0 forced=0 wd=0
HW flip frame  600: gs_frames=600  vblanks=3915  flips=599  dropped=0 forced=0 wd=0
HW flip frame 1000: gs_frames=1000 vblanks=7110  flips=999  dropped=0 forced=0 wd=0
HW flip frame 1500: gs_frames=1500 vblanks=11683 flips=1499 dropped=0 forced=0 wd=0
```
flips = кадры − 1, dropped = 0, watchdog не срабатывал. (Доказательство ≥ 3000 кадров с меню — п. 1.4.)


### 1.4 Длинный прогон: 3D + 2D-меню (≥ 3000 кадров, ≥ 500 кадров с меню)
Команда: `python build/opt5-h/run.py proof2 --elf build/opt5-h/e5.ELF --until "VIDSHOT COMPLETE" -- -renderer Hardware -warp 1 -hwstats 250 -vidkeys 400:esc,1100:esc,1500:esc,2200:esc -vidshot f300,f800,f1300,f1800,f2500,f3300 -hwexit 3500`
(обычный запуск, не `-timedemo`: GFZ1, ESC открывает меню паузы поверх 3D-сцены: кадры 400-1100 и 1500-2200 = **1400 кадров с открытым меню**, `menu=1` в логе). Лог `build/opt5-h/run/proof2/boot.txt`, строки `HW flip` (каждые 250 кадров):
```
frame  250 gs_frames=250  vblanks=1427  flips=249  dropped=0 forced=0 wd=0 menu=0
frame  500 gs_frames=500  vblanks=2182  flips=499  dropped=0 forced=0 wd=0 menu=1   (leveltime заморожен на 879: игра на паузе)
frame 1000 gs_frames=1000 vblanks=3687  flips=999  dropped=0 forced=0 wd=0 menu=1
frame 1250 gs_frames=1250 vblanks=4438  flips=1249 dropped=0 forced=0 wd=0 menu=0
frame 2000 gs_frames=2000 vblanks=6700  flips=1999 dropped=0 forced=0 wd=0 menu=1
frame 3000 gs_frames=3000 vblanks=9710  flips=2999 dropped=0 forced=0 wd=0 menu=0
frame 3250 gs_frames=3250 vblanks=10461 flips=3249 dropped=0 forced=0 wd=0 menu=0
```
3300 кадров, показано каждый (`flips = кадры - 1`), 0 потерь, 0 срабатываний watchdog, `VIDSHOT COMPLETE 6` (шесть снимков через readback). Меню поверх 3D нарисовано (снимок кадра 800: «CONTINUE/RETRY/OPTIONS...» поверх GFZ1; кадр 300 — чистая 3D-сцена).
Этот же прогон со **старым** драйвером для титульной 3D-карты показал 191 показ на 1550 кадров (п. 1.1). Скорость показа: 3.0 vblank на кадр в GFZ1 (≈ 20 кадров/с в эмулированном времени) с меню и без него.

### 1.5 Попутные исправления
* `PS2HWD_ReadScreenRGB` (скриншот/`-vidshot`/`-hwdump`) падал `screen readback RAM allocation failed`: нужен выровненный блок 327680 Б, а при 23.5 МБ в куче его нет (фрагментация). Теперь при отказе `memalign` картинка читается полосами по 8 строк через статический буфер 32 КиБ (результат тот же).

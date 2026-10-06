# OPT9, агент S (память/стабильность полной сборки, сплитскрин, software FPS, звук): рабочий журнал

Сессия 2026-10-06 (ночь). Каталоги: `build/opt9-s/{out,out-hw,out-full,out-zdbg,run,sweep}`; паки — `build/opt6-s/pak` (жёсткие ссылки). Эмулятор — только `opt_run.py`/`map_sweep.py`/`ftest_golden.py` поверх `run_pcsx2.py` (≤ 2 прогона одновременно).
Реестр: память/звук `PS2-70..79` (70..78 заняты в OPT4/7; в OPT9 — PS2-79 и далее, см. раздел 4), software `PS2-80..89` (80..86 заняты; свободны 87..89), сплитскрин/стабильность `PS2-140..149`.

## 0. Старт (проверено запуском)

* Полная сборка `SRB2_PS2_NO= SRB2_PS2_HW=1` (`build/opt9-s/out-full`, LTO-шаг 358 с при нагрузке от остальных агентов): ELF 10 987 828 Б; загружаемый образ text 4 744 864 + data 582 788 + bss 2 725 696 = 8.05 МБ; арена **22 261 760 Б** (software-only: 24.59 МБ).
* **MAP11 (CEZ2) в полной сборке: OOM** `PU_LEVEL 408 B` на загрузке (`build/opt9-s/run/m11-sw/boot.txt`: `[zck] slopes used=20 296 720 free=1 965 040`, затем арена заполнена целиком: used 22 261 728, свободно 32 Б, кэши 10 КБ). Software-only сборка на этой карте проходила (`gpeak 24.57 / arena 24.59 МБ`, свободно 3 МБ после загрузки): полная сборка не проходит самую тяжёлую карту.
* Причины (сравнение `[zck]` software-only OPT7 `build/opt7-s/run/m11-base` и полной на одних и тех же стадиях, `-zsizes`):
  * `sizeof(side_t)` = **96** вместо 36 (в HW-сборке определён `SIDE_UDMF`: все UDMF-поля в каждой стороне) × 43 170 сторон MAP11 = **+2.6 МБ**;
  * `sizeof(seg_t)` = 64 вместо 48 (поля HW: `pv1/pv2/flength/lightmaps`) × 49 592 = +0.79 МБ (оставлено: их читает HW-рендерер);
  * BSS: `latlnglookup[256][256][3]` float (таблица нормалей MD3, `hw_md3load.c`) = **786 432 Б** постоянно, ради моделей нескольких аддонов;
  * остальное: код Lua/UDMF/сеть/HW (text +1.3 МБ), lwIP-пулы 196 КБ, `linkdrawlist` 172 КБ и т.д.

## 1. Этап 1: память полной сборки (в работе; цифры — non-LTO сборки `build/opt9-s/out-nolto`, MAP11, 32 МБ)

Инструмент: `PCSX2 -gameargs` обрезает строку аргументов на ~128 символах (хвост терялся молча: `-renderer` без `Hardware`, `-zreserve` не виден) — `opt_run.py` теперь пишет все аргументы кроме `-logfile boot.txt` в `<run>/ps2args` (по аргументу на строку, `ps2_boot.c` читает).
Сборка без LTO для быстрых проверок: `build/opt9-s/bnl.sh` (`-Wno-format-truncation` для `ps2_curl.c`/`d_netfil.c`: без LTO `-Werror` HW-сборки валит чужие snprintf; в LTO-сборке этого нет).

Сделано (каждая правка — строка реестра раздела 4):
* `side_t` в HW-сборке 96 → 36 Б (`r_defs.h`: `SIDE_UDMF` только с `-DPS2_HW_SIDE_FULL`; `hw_main.c` — 49 чтений через `SIDE_*` тулом `tools/ps2/hw_side_accessors.py`): MAP11 −2.59 МБ.
* `latlnglookup[256][256][3]` (hw_md3load.c, 786 432 Б BSS) убран: нормаль считается из той же формулы при чтении вершины (побитно).
* PS2-79 `ps2_spill.c`: всё, что не вмещает куча libc (резерв ≈ 0.8 МБ над ареной), берётся из арены зоны (`--wrap=_malloc_r/_calloc_r/_realloc_r/_free_r/_memalign_r`, только поток игры). Без этого первый же HW-запуск полной сборки умирал `HWR_LoadMapTextures: ran out of memory for OpenGL textures` (HW-стадия libc 2.0 МБ при резерве 1.5). Находка при отладке: `_calloc_r` ньюлиба читает размер из заголовка чанка перед указателем (для блока арены — мусор) → `memset` улетал за конец ОЗУ (шторм `cpuTlbMiss`, адрес 0x1f000000...): calloc теперь свой.
  Резерв libc 1.5 МБ → 768 КБ (`z_zone.c`); `ps2_mem: C heap blocks taken from the arena` и `spillpeak/spillnow` в ZSTAT; `-zspill` — строка на каждый блок ≥ 16 КБ.
* PS2-87 `vissprite_t`: клип-массивы `clipbot/cliptop[MAXVIDWIDTH=640]` (2 560 Б на спрайт, `sizeof` 2764) заменены указателями в хвост чанка (по `vid.width`): 212 Б + 1 280 Б; `R_ResetVisSprites` из `R_ExecuteSetViewSize`; копия при разрезе linkdraw сохраняет собственные массивы нового спрайта.
* PS2-87 `seg_t`: `side/dontrenderme/glseg` — три `UINT8` в одном слове (−8 Б на сегмент, MAP11: −0.4 МБ).
* PS2-88 `polyblocklinks` (4 Б на клетку блокмапа, 283 КБ на MAP11) выделяется только первым полиобъектом уровня (`POLYBLOCKLINK()`).
* `tools/ps2/opt_units.txt`: 102 холодных юнита (меню, сейвы, Lua, сеть, загрузчики) с `-Os`: text non-LTO 4 476 960 → 4 165 776 (−311 КБ).

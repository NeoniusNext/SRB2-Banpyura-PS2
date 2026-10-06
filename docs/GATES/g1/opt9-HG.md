# OPT9, агент HG: GS Hardware Renderer — геометрия и CPU-путь (отчёт, ведётся инкрементально)

Сессия 2026-10-06 (ночь). Каталоги: `build/opt9-hg/` — скрипты (копии opt8-h с правкой путей: `mk.sh` HW релиз, `mkp.sh` HW `--prof` -> `outp`, `mks.sh` HW `--sample` -> `outs`,
`mkswp.sh` software `--prof` -> `out-swp`, `run.py`, `bench.sh NAME ELF DEMO args`, `benchsum.py`, `hwphase.py` (HWPROF/HWPROF2 по окнам), `pcshot.py`, `mont3.py`).
Единицы: такты COP0 EE на кадр, PCSX2 2.6.3 (32 МБ), релиз (`SRB2_PS2_RELEASE=1`, LTO), `-timedemo DEMO_00n.lmp -ps2prof`, FPS = 294.912 М / такты.
Эмулятор — только через `tools/ps2/run_pcsx2.py` (обёртка `build/opt9-hg/run.py`, 4 слота автоматически). **HW-прогоны требуют `-zreserve 3072`** (резерв C-кучи 3 МБ; без него `HWR_LoadMapTextures: ran out of memory`).
Номера реестра: PS2-HW-40..59 (HG).

## 0. Состояние на старте (проверено запуском)

* Дерево собирается: HW `--prof` (`outp`, 10.2 МБ), software `--prof` (`out-swp`, 9.8 МБ), HW `--sample` (`outs`, 13.9 МБ). LTO-шаг ≈ 250 с: один цикл сборки ≈ 5 мин.
* Базовые прогоны (4 демо, окна по 105 кадров, окно 0 = загрузка карты, считается 1..N):

| демо | software (среднее окон 1..) | HW baseline `wall` (окна 1..9, с децимацией текстур PS2-HW-24) |
|---|---:|---:|
| DEMO_001 | 9.79 М (30.1 FPS) | 19.1 М (mean `wall`), «работа» (без flipwait) 16.9 М |
| DEMO_002 | 10.15 М (29.1 FPS) | окна 8–9 = смена/загрузка: 36 М (шум); окна 1–7: 15.1 М |
| DEMO_003 | 17.24 М (17.1 FPS) | **OOM** (`PU_HWRBATCH`/`PU_HWRCACHE` 256–368 КБ) на кадрах 304–585: HW не доходит до конца демо |
| DEMO_004 | 19.47 М (15.1 FPS) | `wall` 30–108 М/кадр (`bsp` 15–86 М) |

(Целевая планка «≈9.9 М у software» верна только для DEMO_001; на DEMO_003/004 software тоже 17–19 М.)

## 1. Измерение: где уходят такты (sampler `build.py --sample`, `-ps2sample`, `tools/ps2/sample_report.py`)

DEMO_001 (все 9 окон, ≈22 М тактов/кадр в выборке):
* `Z_Evictable` 3.05 М + `Z_MallocInternal` 2.94 М + `ZA_Alloc` 0.11 М = **6.1 М/кадр (28 %)** — обход арены зоны (`z_zone.c:541/143/146`, `Z_MoveFrontier`).
* драйвер: `emit_poly` 1.63 М, `put_vertex` 0.63, `begin_draw` 0.50, `clip_poly` 0.43, `xv_get` 0.41, `emit_tris` 0.24, `state_flush` 0.22, `pass_setup` 0.22 (≈4.3 М).
* движок BSP: `HWR_Lighting` 0.46, `HWR_ProcessSeg` 0.45, `HWR_Subsector` 0.41, `R_PointToAngle64` 0.25, `CompareVisSprites` 0.22 (qsort), `HWR_CalcWallLight` 0.22, `HWR_RenderPlane` 0.20, `HWR_AddLine` 0.20, `HWR_ProcessPolygon` 0.18, `HWR_RadixSort32` 0.23 …

DEMO_004: `Z_Evictable` 7.64 М + `Z_MallocInternal` 7.06 М = **14.7 М/кадр = 61 % всех тактов**; остальное — драйвер/движок как выше.

Причина (`-zreport 105`): `[zfront] ... zone-less fallbacks` растёт на **~10 (DEMO_001) и ~55 (DEMO_004) за кадр**: долгоживущая половина арены (`ZA_TOP`: PU_STATIC, PU_LEVEL во время игры) забита (свободно 1.3 КБ), каждый
запрос к ней не помещается, `Z_MoveFrontier` проходит всю арену (по блокам, `Z_Evictable` на каждом), не находит места и возвращает «запасной» вариант. Источники таких запросов в кадре (`-zcaller 1`, `zcaller_report.py`):
`HWR_CreateDrawNodes` — 2 × `Z_Calloc(PU_STATIC)` на кадр (≈2.2 КБ), `R_Prep3DFloors` (r_bsp.c) — до 150–220 `Z_Free`+`Z_Calloc(PU_LEVEL)` на кадр (двигающиеся FOF-сектора: `R_CheckSectorLightLists` обнуляет `numlights`, значит блок
пересоздаётся всякий раз), `HWR_MakePatch`/`Patch_CreateFromDoomPatch`/`Picture_PatchConvert` (HT/F).


## 2. Этап 1: зона, плоские прогоны, инструментарий (PS2-HW-40, 41) — результат измерения

Правки: `HWR_CreateDrawNodes` (hw_main.c) — постоянные массивы вместо двух `Z_Calloc(PU_STATIC)` на кадр (PS2-HW-40); `R_CheckSectorLightLists` (r_bsp.c) не обнуляет `numlights`, `R_Prep3DFloors` переиспользует список
(PS2-HW-41, под `#ifndef PS2_PROFILE`); таймеры `HWPROF2` (ps2_hw_prof.h, hw_main.c, i_video.c): `setup sky seg plane addspr subsec light sprsort sprdraw nodesort nodedraw`.
Прогон `v1` (та же сборка плюс правки других агентов, `-zreserve 3072`, окна 1..): DEMO_002 `wall` 10.86 М (окна 1..10; было 15.8 М в окнах 1..7 на базе, у которой bsp в окнах 8–9 = 110 М),
DEMO_003 18.75 М (раньше `bsp` 31..68 М, теперь 2.7 М), DEMO_001 (`-hwtexcap 512`) 12.78 М (было 19.1 М), DEMO_004 всё ещё падает из-за памяти текстур (HT).
Разбивка DEMO_002 после правок (средние по окнам, М тактов/кадр): `wall` 10.86 = `sky` 1.6 + `bsp` 1.95 (`seg` 0.67, `plane` 0.59, `addspr` 0.29) + `batch` 2.42 + `sprites` 1.15 + `nodes` 0.26 + остальное (логика тика, HUD, звук) ≈ 3.5;
драйвер `draw` 4.35 (из них небо 1.6, остальное — полигоны через `cut_and_emit` ≈ 1400 тактов на полигон).

## 3. Этап 2: небо, лит-полигон, ошибка VU0 (PS2-HW-42, 43, 44a)

* **PS2-HW-42** `ps2_hw_sky.inc`: купол (1168 вершин, 1148 треугольников) раньше копировался в staging, строился индексный список и каждый треугольник трансформировался/клипался/упаковывался (1.6 М/кадр).
  Теперь сетка конвертируется один раз, все вершины идут через VU0 (outcode + целые GS), четверти ленты без пересечения плоскости клиппинга уходят одной лентой `TRISTRIP`, четверти за плоскостью отбрасываются,
  пересекающие камеру/guard band — через прежний клиппер по треугольникам; шапка вне кадра отбрасывается целиком. Сетки с |s|,|t| > 4 (старый путь отбрасывает повторы текстуры по треугольникам) идут старым путём. `-hwdbg 4096` — старый путь.
* **PS2-HW-43** `ps2_hw_fast.inc` + хуки в `ps2_hw_draw.inc` (`P.litfast`, `lit_fast_plan`, ветка в `emit_poly`): полигон плана `LM_BANDS` без клиппинга, без резки повторов, одного класса темноты (класс считается теми же операциями, что
  `doom_class`, без `floorf`) упаковывается с целыми GS от VU0 (`emit_reglist_fan_pv` / `put_vertex_pv`), состояние GS ставится по `P.serial`, а не для каждого полигона. Остальное — прежний путь. `-hwdbg 8192` — прежний путь.
* **PS2-HW-44a** ошибка в `vu0_xform` (`ps2_hw_vu0.inc`): выход `f` (`cfc2`) не был «early clobber» — компилятор мог дать ему тот же регистр, что указателю `o`, который читает последний `sqc2` (в LTO-сборке с новым циклом так и вышло:
  запись в NULL/мусор). Исправлено `"=&r"`.

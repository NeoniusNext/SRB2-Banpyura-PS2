# OPT10, агент HG: GS Hardware Renderer — геометрия и CPU-путь (отчёт, ведётся инкрементально)

Сессия OPT10 (Linux-контейнер, свой worktree `worktree-agent-a5f093907a411cb86`). Единицы: такты COP0 EE на кадр, PCSX2 2.6.3 (32 МБ, программный GS), релиз с LTO, `-timedemo DEMO_00n.lmp -ps2prof`,
окна по 105 кадров (окно 0 = загрузка карты, считаются окна 1..9), FPS = 294.912 М / такты. HW-прогоны: `-renderer Hardware -zreserve 3072`.
Скрипты (в `build/`, не в git): `mkp.sh [каталог]` — полная HW-сборка `--prof` (`EXTRA=--sample` — сэмплер), `bench.sh NAME ELF DEMO args` — прогон, `hwsum.py NAME... [-w]` — свод окон HWPROF/HWPROF2.
Номера реестра: PS2-HW-40..59 (HG). Внимание: **`-hwdbg` принимает десятичное число** (`atoi`): `0x8000000` даёт 0 (так «не сработал» первый NOUP-прогон).

## 0. База (HEAD 43b9a7e, HW `--prof`, DEMO_001, окна 1..9)

| величина | М тактов/кадр |
|---|---:|
| `wall` среднее | **73.9** (4.0 FPS), максимум окна 86.4; «1051 gametics in 9163 realtics» |
| `clear` / `bsp` / `batch` / `sprites` / `nodes` | 24.8 / 3.3 / 19.8 / 22.1 / 1.3 |
| `drv draw` / `tex` (копирование текселей) | 4.9 / 18.5 |
| `HWPROF2`: setup / seg / plane / subsec / sprdraw / nodedraw | 24.8 / 1.3 / 2.4 / 3.2 / 21.8 / 4.6 |

Сэмплер (`--sample`, `-ps2sample`, `tools/ps2/sample_report.py`; в HW-режиме `PROF total` — переполненный 32-бит COP0, отчёт теперь берёт такты из HWPROF) — DEMO_001, группы функций (М/кадр, сборка сэмплера ≈ 89 М/кадр):
текстуры (`HWR_GenerateTexture` 20.8, `fill_ap88` 8.2, `tex_upload` 7.2, `HWR_Draw*ColumnInCache` 11.3, `MakeBlock` 3.2, `ASTBlendPaletteIndexes` 2.7, `vram_alloc_evicting` 2.0, `dec_level` 1.5 …) ≈ **60**, memcpy/memset 7, звук (vorbis) 6.3,
**геометрия CPU (драйвер + движок HW) ≈ 11** (`emit_poly` 1.7, `begin_draw` 0.73, `cut_and_emit`+`clip_attr` 1.3, `emit_reglist_fan(_pv)` 1.1, `clip_poly` 0.5, `HWR_ProcessSeg` 0.46, `HWR_Subsector` 0.44 …), логика тика ≈ 1.
Вывод: **почти всё время базы — текстуры (HT)**; чистая геометрия измеряется отдельно (раздел 1).

## 1. Чистая стоимость геометрии: флаг `-hwdbg 134217728` (PS2-HW-40a, только измерение)

`HWDBG_NOUP` (`ps2_hw_vif.inc`, хук в начале `hw_SetTexture`): все текстуры = один резидентный дамми 256x256, загрузок нет, `tex`=0, `regen`=0; картинка неверна, но число полигонов, батчей, вызовов драйвера прежнее.

DEMO_001 (`n1`, база без VU1/без правок геометрии), среднее окон 1..9:

| `wall` | `clear` | `bsp` | `batch` | `sprites` | `nodes` | `drv draw` | HWPROF2: seg / plane / subsec / sprdraw / nodedraw |
|---:|---:|---:|---:|---:|---:|---:|---|
| **12.75 М (23.1 FPS)** | 2.42 | 3.22 | 2.73 | 2.08 | 0.82 | 4.94 | 1.25 / 2.36 / 3.16 / 1.82 / 2.01 |

Остаток `wall` − сумма фаз ≈ 1.5 М (логика тика, HUD, звук вне фаз). Полигонов в кадре ≈ 3600 (3.2 вершины на полигон: треугольники/четырёхугольники), т.е. ~1350 тактов драйвера и ~900 тактов движка на полигон.
Цель ≤ 8.4 М требует убрать ~4.4 М из 11.3 М рендера, цель 4.9 М — ~7.9 М.

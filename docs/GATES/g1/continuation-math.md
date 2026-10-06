# G1 continuation: fixed math / hot double — 2026-10-02

## Итог

**Fixed math проверена и включена по умолчанию; замена slope/seg double
не прошла строгую эквивалентность и выключена по умолчанию. Hot-double пункт
G1 этим отчётом не закрыт.** Все изменения незакоммичены. Reference для
standalone-тестов: `HEAD=2653b4c285cb5903330cdf7d3ae180c239454714`;
исходники извлекаются через `git archive/show`, golden не пересоздавался.

Изучены `AGENT_BRIEF.md`, G1 и CPU/A2 пункты PLAN, исходный незакоммиченный
diff восьми renderer/fixed файлов и существующие тесты. Предыдущий default
автоматически включал `MATH`, `SLOPE`, `SEGS` для `PS2_PROFILE`.

## Что изменено

- `src/m_fixed.h,c`: сохранены существующие 32-bit `mult/mfhi`, нормализованное
  деление и sqrt estimate + integer fix-up. Default `PS2_OPT_MATH` только
  при `PS2 || PS2_PROFILE`. `PS2_NOOPT` / `PS2_NOOPT_MATH` выключают его,
  включая конфликт с явно заданным `PS2_OPT_MATH`.
- `PS2_OPT_SLOPE` / `PS2_OPT_SEGS` теперь **только явный opt-in**. Общий и
  групповые NOOPT имеют приоритет. Без PS2/profile все math opt-флаги отключены.
  `PS2_Clz` доступен независимому slope/seg experiment даже с NOOPT_MATH.
- `FixedSqrt` ускоряет только исходную integer-sqrt ветвь: при `HAVE_SQRT`
  исходная libm-ветвь сохраняется. Negative inputs в integer-ветви по-прежнему
  трактуются как UINT32 исходным циклом.
- `math_hosttest.c,py`: независимый unsigned-magnitude oracle для определённой
  saturation `FixedDiv`, включая `INT_MIN`, нулевой делитель и оба знака;
  отдельная compile/run матрица девяти macro-конфигураций; HEAD snapshot
  перенесён в собственный output теста.
- `math_slope_hosttest.c,py`: явное включение experiment, режим
  `--strict-equivalence`, без исключения near-parallel rays; сравнение LSB
  без переполнения signed subtraction. Numerical PASS больше не выдаётся
  за точную эквивалентность.
- Новые `math_draw_hosttest.c,py`: настоящие 14 textured tilted drawers
  извлекаются из HEAD/working tree; PO2/NPO2, water, translucent, splat,
  floor-sprite, границы span 1/2/15/16/17/31/32/33/319/320, координатный wrap,
  row guards, byte-for-byte сравнение и испорченный drawer как negative control.
  Это synthetic span test, **не** frame-A2 измерение.
- `build_math_bench.py,math_bench.c`: собственный snapshot, команды/build log,
  edge saturation checks, корректный счётчик проверок, JSON с ELF SHA256;
  wrapper-success принимается лишь вместе с `failures=0` в selftest.
- `math_inventory.py`: явный каталог объектов, disassembly + SHA256 + flags
  и JSON; отсутствие объектов теперь ошибка, а не ложный нулевой inventory.
- `build_host_b.ps1`: изолированные default / fixed-reference / experimental
  режимы через CMake flags. `host_frames_compare.py`: строгий exit status,
  проверка полного набора и размера кадров, непустого golden overlap,
  exe SHA256 и фактических replay-команд; свежий output обязателен.
- `r_slopeq.h,r_segs.c`: комментарии к experiment уточняют отличие точного
  rational intersection от результата последовательных double округлений.

`PS2_OPT_DRAW` остаётся существующим определением; в принадлежащих renderer
файлах нет использующей его альтернативной ветви. Это не заявка на проверенную
новую drawer-оптимизацию.

## Выполненные команды и результаты

Все пути ниже относительно workspace. Артефакты — только `build/continue-math*`.

### Fixed / guards / controls

```powershell
python tools/ps2/math_hosttest.py --out build/continue-math-fixed-final --pairs 100000000 --sqrt 100000000 --negative-controls
python tools/ps2/math_hosttest.py --out build/continue-math-guards --guards-only
```

`continue-math-fixed-final/test.log`:

```text
edge matrix: 453 x 453 pairs
exhaustive |a|,|b|<=2048: 16785409 pairs
random pairs: 100000000 (each: FixedMul, FixedDiv, FixedDiv2)
FixedSqrt checks: 167371040
FixedHypot/FV2/FV3 checks: 60000000
TOTAL checks=763855708 failures=0
```

Sqrt не исчерпывающе перебран по всем 2^32 inputs: проверены первые 2^26,
границы powers of two, 100 млн generated inputs, 2 млн fix-up estimates
с отклонением до ±64. `FixedDiv2` проверяется лишь для b != 0, как требует
контракт исходной функции; проверенное деление на ноль — saturating `FixedDiv`.

Все семь negative controls вернули exit 1; ошибки записаны в
`continue-math-fixed-final/neg-*/test.log`:

| Control | failures |
|---|---:|
| divlu-no-correction-1 | 466106 |
| divlu-no-correction-2 | 9045528 |
| div2-no-sign | 17165399 |
| mul-wrong-shift | 13944649 |
| sqrt-no-fixup-up | 975026 |
| div-saturate-sign | 853483 |
| divmag-skip-reduce | 344320 |

`continue-math-guards/guards/test.log`: **9 modes PASS**: PC, PS2-only,
PS2_PROFILE, NOOPT, NOOPT_MATH, experiment без fixed, общий/групповой
NOOPT против explicit opt, explicit opt без платформенного guard.

### Slopes: numerical PASS, strict FAIL

```powershell
python tools/ps2/math_slope_hosttest.py --out build/continue-math-slope-final --cases 2000000 --negative-controls
python tools/ps2/math_slope_hosttest.py --out build/continue-math-slope-strict --cases 2000000 --strict-equivalence
```

- 2 млн angles, ANG2RAD mismatches **0**, max sin/cos error **1.11e-16**.
- 4 млн rotations, **0** различий.
- Strict: 4 млн seg/ray intersections, **3999999 equal, 1 off by 1 LSB**,
  **0 skipped**. Numerical mode исключает 2545 near-parallel cases.
- 20 млн thick-side yscale cases, **0** mismatches в проверенном in-range
  unscaled случае. Это не проверка всех scaled-wall corner conversions.
- 2 млн planes (499602 scaled), 24 млн components: **4147** не равны
  даже `(float)reference`. Worst vector normalized error **1.19e-7**;
  zeroheight worst error **3.49e-10** map units;
  lightscale worst relative error **1.94e-7**.
- Numerical mode `RESULT PASS`; все **6** negative controls дают
  exit 1 / `RESULT FAIL` (`neg-*/test.log`). Strict mode **exit 1 / RESULT FAIL**.

Эти component tolerances не являются разрешением A2 на пиксели.

### Standalone pixels

```powershell
python tools/ps2/math_draw_hosttest.py --out build/continue-math-pixels-default --cases 10000 --negative-controls
python tools/ps2/math_draw_hosttest.py --out build/continue-math-pixels-experimental --cases 10000 --experimental
```

14 drawers × 10000 inputs: **140000 spans / 44800000 bytes**.
Default **0 differing spans / 0 px**, SHA256 обеих копий
`ad4b0cc14afd5d208a8e1d01f3f94f59bde3ef8723ee94d7f650c373bc04fbce`.
Negative control: **9487 differing spans / 1418823 px**, обнаружен.
Explicit float slope: **42355 differing spans / 67193 px**, **exit 1**.
JSON, extracted-body hashes, generated code, build/test logs — в этих каталогах.
Последние standalone drawer builds не имеют предупреждений.

### Свежие host builds и все demo frames

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/ps2/build_host_b.ps1 -Out build/continue-math-host -ExeName srb2-continue-math -MathMode default
powershell -NoProfile -ExecutionPolicy Bypass -File tools/ps2/build_host_b.ps1 -Out build/continue-math-host-ref -ExeName srb2-continue-math-ref -MathMode fixed-reference
powershell -NoProfile -ExecutionPolicy Bypass -File tools/ps2/build_host_b.ps1 -Out build/continue-math-host-experimental -ExeName srb2-continue-math-experimental -MathMode experimental
python tools/ps2/host_frames_compare.py --ref build/continue-math-host-ref/bin/Release/srb2-continue-math-ref.exe --cand build/continue-math-host/bin/Release/srb2-continue-math.exe --out build/continue-math-frames-default --every 1
python tools/ps2/run_host_profile.py --exe build/continue-math-host/bin/Release/srb2-continue-math.exe --output build/continue-math-golden-default
python tools/ps2/host_frames_compare.py --ref build/continue-math-host-ref/bin/Release/srb2-continue-math-ref.exe --cand build/continue-math-host-experimental/bin/Release/srb2-continue-math-experimental.exe --out build/continue-math-frames-experimental --every 1
```

Initial обе сборки остановились на чужом `m_menu.c:14643 C1004`; логи
`build-20261002-091616.log` сохранены. После изменения общего дерева повторные
сборки прошли, этот blocker больше не воспроизводится. `m_menu.c` мной не правился.
Default final build log: `continue-math-host/build-20261002-092810.log`.

Default против NOOPT_MATH: **4196 frames (1049/demo), 0 differing frames,
0 px**, все четыре `tics.csv` идентичны, ref-vs-golden **0 px / 120 frames**.
Golden run: **4200 тиков / 120 кадров / 200 файлов / 0 differences**,
`passed=true`, `inputs_changed=[]`. Проверены также SOC и SFX/PCM.

Explicit SLOPE+SEGS против того же reference, strict **exit 1**:

| Demo | Frames | Differing frames | Pixels | Worst % |
|---|---:|---:|---:|---:|
| DEMO_001 | 1049 | 94 | 123 | 0.00625 |
| DEMO_002 | 1049 | 41 | 55 | 0.00625 |
| DEMO_003 | 1049 | 90 | 193 | 0.0375 |
| DEMO_004 | 1049 | 159 | 2863 | 0.1296875 |
| Total | 4196 | 384 | 3234 | 0.1296875 |

Тики experiment также совпали. Пиксели не классифицированы маской slopes;
общий A2-допуск нельзя применять к seg/rotation/lighting отличиям.
Требование пользователя — **0 px**; experiment отклонён независимо от малого
среднего процента (**0.00120427%**).

### Standalone EE bench и actual-object disassembly

```powershell
$env:SRB2_PS2_OUT = 'D:/Ai-Project3/SRB2-PS2-Port/build/continue-math-ee'
python tools/ps2/build_math_bench.py --run --timeout 240
python tools/ps2/math_inventory.py --objects build/continue-math-ee/math-bench --files math_bench,m_fixed,ref_wrap --out build/continue-math-inventory-bench
python tools/ps2/math_inventory.py --objects build/agent-b-opt/obj --out build/continue-math-inventory-candidate
python tools/ps2/math_inventory.py --objects build/g1-final-build/obj --out build/continue-math-inventory-reference
```

Запущен только отдельный math ELF, **не integrated EE build**.
PCSX2 вызван только `run_pcsx2.py` с общей блокировкой. ELF **1261080 Б**,
selftest **1000000 pairs / 3492413 checks / failures=0**, wrapper exit **0**.
`continue-math-ee/math-bench/{commands.json,build.log,run.log,report.json}`.
Все 5 EE compile units exit 0, без предупреждений (`-Wall -Wextra`).

COP0 Count, cycles/call после вычета overhead 15.0; minimum из 32 repeats:

| Distribution | FixedMul ref→opt | FixedDiv ref→opt | FixedDiv2 ref→opt | FixedHypot ref→opt |
|---|---|---|---|---|
| game | 10.0→5.0 | 134.0→108.3 | 124.0→104.6 | 499.6→195.8 |
| subunit | 10.0→5.0 | 129.0→36.9 | 119.5→33.5 | 494.4→123.9 |
| uniform | 10.0→5.0 | 134.0→108.6 | 124.0→104.7 | 499.2→196.1 |

`FixedSqrt`: **336.2→56.7**. Это microbench PCSX2, не измерение whole-frame
производительности и не модель cache PS2.

Fresh `m_fixed.o` inventory: **0 helper relocations**; `math_bench.o:new_mul`
содержит `mult v0,a0,a1` + `mfhi a0`; `m_fixed.o` содержит `plzcw` и `sqrt.s`.
Reference `ref_wrap.o`: по **1 `__divdi3`** в FixedDiv/FixedDiv2;
`ref_m_fixed.o`: **27 `__divdi3`** call sites.

Два engine inventory получены из **реальных ранее собранных объектов**,
по 16 объектов. Они не выданы за rebuild текущего source state; SHA256,
mtime, flags и полные `objdump -dr` сохранены рядом с `inventory.json`.
Stored candidate `r_draw.o`: **0 soft-double call sites**;
`r_plane.o`: **0 soft-double call sites**;
`r_segs.o:R_RenderThickSideRange`: `__adddf3`×2, `__divdf3`×2,
`__muldf3`×5, `__floatsidf`×4, `__fixdfsi`×1 — scaled-wall fallback остаётся.
NPO2 drawers сохраняют `__udivdi3`×2 и `__clzdi2`×2 на routine (libdivide setup).

Попытка `--objects build/ps2/obj --out build/continue-math-inventory-current`
дала **no matching EE objects / exit 1**. Общий каталог не заполнялся мной.

## Что остаётся / передача координатору

В **нынешнем default source profile** hot double всё ещё выполняется:

- `r_draw.c` / `r_draw8*.c`: slope lighting и все 14 textured tilted spans.
- `r_plane.c`: slope plane/light setup, cross products и sin/cos rotations
  offset/polyobject; arithmetic в вызываемом `m_vector.c` также исходная.
- `r_segs.c`: thick-side yscale (включая unscaled default) и обе seg/ray
  intersections. Experiment не устраняет scaled-wall fallback.
- Вне владения: `d_main.c:D_SRB2Loop`, `screen.c:SCR_CalculateFPS` /
  `SCR_DisplayTicRate`, `r_fps.c:R_ApplyLevelInterpolators` содержат double
  helpers в inspected objects. Inventory также показывает boot/optional
  palette/sprite/HUD пути; их частота этим тестом не профилирована.
- `p_slopes.c` soft-double сохранён, как разрешено заданием.

**Host profile math сейчас default ON** (`PS2_PROFILE`); EE (`PS2` и
`PS2_PROFILE`) также default ON для exact fixed группы. SLOPE/SEGS default OFF.
Обычный PC без guards — исходная fixed/renderer ветвь.

Новых обязательных build flags/init вне владения не требуется. Координатору
нужно пересобрать integrated EE после этих macro-изменений: старые объекты
с default float slopes не соответствуют нынешнему default. Full EE frame
replay, target slope pixels и аппаратный PS2 timing здесь **не проверялись**.
После host evidence fixed часть готова к интеграции; без exact замены hot
double строгий CPU/G1 пункт остаётся открыт.

## Предлагаемая строка реестра (PS2-16 candidate)

`docs/DEVIATIONS.md` вне владения и не редактировался мной. Предложение
координатору после проверки свободного ID:

| ID | Область | Отличие и проверка |
|---|---|---|
| PS2-16 | Только `PS2 || PS2_PROFILE`, `m_fixed.c,h`; experimental `r_draw*`, `r_plane`, `r_segs`, `r_slopeq.h` | Exact FixedMul/Div/Div2/Int и integer FixedSqrt на 32-bit EE instructions, defined unsigned-magnitude saturation для INT_MIN/zero. Default fixed ON; host 763855708 checks/0 failures, 7 negative controls; EE 3492413 checks/0 failures; 4196 host frames/0 px и golden 200 files/0 differences. SLOPE/SEGS только explicit opt-in, не приняты: 4147 vector components, 1 seg LSB и 3234 px/384 demo frames отличаются (worst 0.1296875%); hot double default остаётся. Полный отчёт `docs/GATES/g1/continuation-math.md`. |

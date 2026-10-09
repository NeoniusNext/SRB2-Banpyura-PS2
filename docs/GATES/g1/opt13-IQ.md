# OPT13 IQ: быстрые побитные правки (готовые патчи исследования, по одному коммиту)

Ветка `opt13i-iq`, worktree `/home/user/wt/iq`, база `ea3d0fa` (= OPT12 + документы OPT13). Сборка: `SRB2_PS2_OUT=... SRB2_PS2_NO= SRB2_PS2_HW=1 python3 tools/ps2/build.py --jobs 2 [--ps2ref|--prof]`.
Эталонные ELF (чистый `git archive HEAD` в `build/basesrc`, чтобы правки в рабочем дереве не попадали в базовую сборку): `build/base/ref.ELF` (--ps2ref), `build/base/prof.ELF` (--prof, LTO).
Хост-эталон (x86, PS2_PROFILE): `build/host-base/out` (после пункта 3); `tics.csv` на 4 демо побайтно равны `golden/phase0-v2/run1`.

## Сводка

| # | пункт | коммит | статус |
|---|---|---|---|
| 3 | host_build_fix_main (хост-профиль снова собирается) | см. git log | принят |
| 0 | `tickmax=` в строке `TICK` профиля (самый длинный `TryRunTics` окна) | см. git log | принят (прибор) |
| 1 | O(1)-удаление из списка интерполяции | см. git log | принят |

## 3. host_build_fix_main

Патч `docs/research/rtick/patches/host_build_fix_main.patch` применился без правок (`ps2_loadprof.h`, `tools/ps2/build_host_profile_wad.c`).
Проверка: `JOBS=2 tools/ps2/host_variant.sh base "" same` собирает хост-профиль (116 целей) и прогоняет 4 демо: 1050 хэшей кадров, 1052 тика на каждом;
`cmp build/host-base/out/DEMO_00n/tics.csv golden/phase0-v2/run1/DEMO_00n/tics.csv` равно для n = 1..4. Код EE не меняется (правки только под `!PS2` и в хост-заглушках).

## 0. Прибор: tickmax

Для пункта 1 нужен худший тик окна, а не сумма (всплеск в один тик не виден в сумме окна из 105 кадров). `src/d_main.c`, `src/ps2/ps2_prof.{c,h}`: в строку `TICK` (сборки `--prof`/`PS2_PROF_DIRECT`) добавлено `tickmax=` (циклы самого длинного вызова `TryRunTics` в окне). Старые разборщики (`core_tick.py`) читают хвост строки как есть. Базовая ELF для A/B собрана с тем же прибором (чистый HEAD + только этот коммит): `build/base/prof.ELF`.

## 1. interp_remove_o1 (RTICK D-в)

Патч `interp_remove_o1.patch` лёг без правок. `mobj_t.interpidx` (+4 Б, шаг пула мобжей 416 Б не меняется), `R_RemoveMobjInterpolator` берёт индекс из мобжа, если он ещё указывает на этот мобж; иначе прежний линейный поиск.
**Доказательство тождественности списка.** Временный прибор (не в коммите) в хост-сборке сравнивал на каждом удалении индекс из подсказки с результатом линейного поиска и останавливал игру при расхождении: 4 демо, расхождений нет (D4: > 4096 попаданий; промахов подсказки — ноль). Хост `host_variant.sh o1 "" same`: `tics.csv` и 1050 хэшей кадров всех 4 демо равны базе.
**Golden software (EE, `--ps2ref` ELF, `tools/ps2/golden_full.sh`, `SRB2_PAK=build/pak2`)**: D1..D4 «tics identical over 1050 rows», «frames: 30 reference, 0 differ» (`golden/ps2-head`), и `tics.csv` = `golden/phase0-v2/run1` (RESULT OK ×8).
**Числа (`--prof` LTO, одна пара ELF база/патч, D4 `-timedemo`, окна 1..9, М тактов):**

| | tick/тик (среднее окон) | tickmax окна 4 | tickmax окна 5 (всплеск искр) | кадр, среднее |
|---|---:|---:|---:|---:|
| software D4 база | 1.328 | 4.21 | 7.86 | 11.733 |
| software D4 O(1) | 1.266 (−4.7 %) | 2.80 | 3.16 | 11.668 (−0.6 %) |
| HW D4 база | 1.337 | 4.40 | 7.67 | 8.741 |
| HW D4 O(1) | 1.275 (−4.6 %) | 2.82 | 3.08 | 8.712 (−0.3 %) |

Худший всплеск D4 7.7–7.9 → 3.1–3.2 М (в исследовании 7.57 → 2.55 М с прибором `RT_EEPROF`; здесь измерена вся `TryRunTics`, остаток — создание 258 искр). Окно 1 (25 М) одинаково на базе и в патче: первый вызов после загрузки уровня, к пункту не относится.
Файлы: `src/p_mobj.h`, `src/r_fps.c`.

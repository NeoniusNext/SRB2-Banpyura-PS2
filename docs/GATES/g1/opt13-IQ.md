# OPT13 IQ: быстрые побитные правки (готовые патчи исследования, по одному коммиту)

Ветка `opt13i-iq`, worktree `/home/user/wt/iq`, база `ea3d0fa` (= OPT12 + документы OPT13). Сборка: `SRB2_PS2_OUT=... SRB2_PS2_NO= SRB2_PS2_HW=1 python3 tools/ps2/build.py --jobs 2 [--ps2ref|--prof]`.
Эталонные ELF (чистый `git archive HEAD` в `build/basesrc`, чтобы правки в рабочем дереве не попадали в базовую сборку): `build/base/ref.ELF` (--ps2ref), `build/base/prof.ELF` (--prof, LTO).
Хост-эталон (x86, PS2_PROFILE): `build/host-base/out` (после пункта 3); `tics.csv` на 4 демо побайтно равны `golden/phase0-v2/run1`.

## Сводка

| # | пункт | коммит | статус |
|---|---|---|---|
| 3 | host_build_fix_main (хост-профиль снова собирается) | см. git log | принят |

## 3. host_build_fix_main

Патч `docs/research/rtick/patches/host_build_fix_main.patch` применился без правок (`ps2_loadprof.h`, `tools/ps2/build_host_profile_wad.c`).
Проверка: `JOBS=2 tools/ps2/host_variant.sh base "" same` собирает хост-профиль (116 целей) и прогоняет 4 демо: 1050 хэшей кадров, 1052 тика на каждом;
`cmp build/host-base/out/DEMO_00n/tics.csv golden/phase0-v2/run1/DEMO_00n/tics.csv` равно для n = 1..4. Код EE не меняется (правки только под `!PS2` и в хост-заглушках).

# OPT12, агент CORE: объекты/тик, RAM, software, стабильность (отчёт, реестр PS2-500..549)

Ветка `opt12-core`, worktree `/home/user/wt/core`, PCSX2 2.8.2 (программный GS, 32 МБ), релизная LTO-сборка полной конфигурации
(`SRB2_PS2_NO= SRB2_PS2_HW=1 python3 tools/ps2/build.py --jobs 2 [--prof|--ps2ref|--sample]`). Единицы: такты COP0 EE (294.912 М/с), среднее окон 1..9 (945 кадров)
таймдемо `-timedemo DEMO_00n.lmp -ps2prof`; «тик» = `tick/tic` из `tools/ps2/core_tick.py` (TryRunTics на один тик, звуковые потоки вытесняют основной поток внутри него).
Разброс повторов одной ELF ≈ 0.01..0.03 М. Любое сравнение «до/после» — на ОДНОЙ ELF через `-ps2noquick MASK` (бит n выключает быстрый путь n), если не сказано иное.

## 0. База на старте (HEAD 455b669, проверено запуском)

| демо | тик/тик, М | диспетчер/кадр, М | всего/кадр, М | FPS | объектов (tics.csv) |
|---|---:|---:|---:|---:|---:|
| DEMO_001 | 0.69 | 7.57 | 8.28 | 35.6 | 1063 |
| DEMO_002 | 2.06 (без звука `-nosound -nomusic`: 1.93) | 6.34 | 8.42 | 35.0 | 2021..2091 |
| DEMO_003 | 5.28 | 5.84 | 11.14 | 26.5 | 4135..4249 |
| DEMO_004 | 1.33 | 10.44 | 11.79 | 25.0 | 2341..2977 |

* Golden на HEAD (PS2REF-ELF, `build/gold.sh`): DEMO_001, DEMO_003 — `tics identical over 1050 rows` против PC golden (`golden/phase0-v2/run1`), 30 кадров против `golden/ps2-head` — 0 differ, `ALLHASH identical (1050 frames)`.
* Хост-профиль (x86) на HEAD НЕ линковался (`ps2_fxfrac` из OPT11-FX2 живёт только в `src/ps2/i_video.c`): добавлена заглушка в `tools/ps2/build_host_profile_wad.c`. После этого
  `tools/ps2/host_variant.sh base "" same` собирается и даёт `tics.csv`, **побайтно равный PC golden на всех четырёх демо**, плюс FNV каждого кадра (`allhash.csv`) — быстрый (≈ 3 мин на 4 демо) A/B-стенд для правок тика.
* MAP11 (CEZ2) software, `-warp MAP11 -zquit 35`: арена 23 556 096 Б, после 35 кадров `free=80 768 B largest=19 776 B evicted=3834 (190 МБ) failed=1036` — кэш текстур перегружен каждый кадр.
* bss 2.26 МБ (`netcmds` 295 КБ, lwIP `memp_memory_PBUF_POOL_base` 197 КБ, `ffloor` 104 КБ, `pts` 82 КБ, `H` 70 КБ, `eng` 68 КБ, `emblemlocations` 68 КБ, `ssmemo`+`sismemo` 98 КБ, ...), text 4.89 МБ, data 0.63 МБ.

(отчёт дописывается по этапам)

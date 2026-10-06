# OPT10, агент HT: HW — текстуры, VRAM, загрузка, GIF/DMA (отчёт, ведётся инкрементально)

Сессия 2026-10-06/07, Linux-контейнер, ветка `worktree-agent-a942e589eb2640e9b`. Продолжение `opt9-HT.md` (PS2-HW-30..39).
Каталоги: сборка `build/out` (HW-ELF `SRB2_PS2_OUT=$PWD/build/out SRB2_PS2_NO= SRB2_PS2_HW=1 python3 tools/ps2/build.py --jobs 2 --prof`), прогоны `build/runs/NAME`.
Инструмент: `python3 tools/ps2/hwsum.py NAME...` — таблица окон HWPROF (среднее/макс по окнам, FPS = 294.912 М / wall) + строки HWTEX.

## 0. База (HEAD 43b9a7e, релиз+LTO+`--prof`, `-renderer Hardware -zreserve 3072 -ps2prof`, окна по 105 кадров, такты EE на кадр)

Команда: `python3 tools/ps2/opt_run.py --name base1 --elf build/out/SRB2.ELF --pak /home/user/SRB2-Banpyura-PS2/build/pak --out build/runs --demo DEMO_001 --no-ref --timeout 900 --until "gametics in" -- -renderer Hardware -zreserve 3072 -ps2prof`

| демо | окон | wall среднее | max окна | clear | batch | sprites | tex | uploads/окно | ws (блоков/текстур) | пул |
|---|---|---|---|---|---|---|---|---|---|---|
| DEMO_001 | 10 | **72.13 М (4.09 FPS)** | 86.52 М | 23.1 | 18.3 | 20.5 | 17.3 | ≈10 200 | 25–29 тыс. / 99–140 | 12 032 |

«1051 gametics in 9183 realtics» (OPT10 бриф: 9129) — база воспроизведена (в брифе ≈70 М, 4 FPS).

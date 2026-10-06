# OPT10, агент SW (software-рендерер, скорость кадра): рабочий журнал

Сессия 2026-10-06/07, Linux-контейнер, ветка `worktree-agent-ac96535946e7398b3` (worktree). Каталоги: `build/out-*` (сборки), `build/runs/*` (прогоны).
Единица — такты COP0 EE на кадр (`-timedemo`, 1 кадр = 1 тик), окна 1..9 по 105 кадров (945 кадров), FPS = 294.912 М / такты. PCSX2 2.6.3, 32 МБ.
Реестр: `PS2-80..89` (80..88 заняты ранее; здесь — PS2-89 и далее по согласованию, см. раздел 4).

## 0. Инструменты (новые)

* `tools/ps2/prof_summary.py` — среднее/максимум по окнам `PROF` (М тактов/кадр, FPS).
* `tools/ps2/sample_report.py` — переносим на Linux (пути тулчейна), добавлена сводка по категориям исходников (`category (by source file)`).
* `tools/ps2/softcalls.py` — какие функции движка вызывают soft-double/64-битные помощники libgcc (дизассемблирование объектов non-LTO сборки).
* Эталон «каждый кадр»: PS2REF `-ps2ref-hashall` пишет FNV-1a **каждого** кадра демо (`allhash.csv`, 1050 строк). Сохранены базовые хеши HEAD (`tools/ps2/golden_allhash/DEMO_00n.csv`),
  `tools/ps2/sw_gold.sh ELF TAG DEMO_001 ...` гоняет PS2REF-ELF, сравнивает 30 кадров с `golden/ps2-head` (побитно) и **все 1050 кадров** с базой.
* `tools/ps2/sw_base.sh ELF TAG DEMO_001 ...` — замер скорости на `--prof`-ELF.

## 1. База (дерево на старте, HEAD 43b9a7e; `--prof`, LTO, программная конфигурация `SRB2_PS2_NO=lua,udmf,addons,limits`)

Команда: `tools/ps2/sw_base.sh build/out-prof/SRB2.ELF b1 DEMO_001 DEMO_002 DEMO_003 DEMO_004` (`opt_run.py ... --demo DEMO_00n --no-ref -- -ps2prof`).

| демо | среднее окон 1..9, М/кадр | FPS | максимум окна | минимум окна | «1051 gametics in N realtics» |
|---|---:|---:|---:|---:|---:|
| DEMO_001 | 9.89 | 29.8 | 15.72 | 5.23 | 1197 |
| DEMO_002 | 10.02 | 29.4 | 14.96 | 5.52 | 1240 |
| DEMO_003 | 16.02 | 18.4 | 19.60 | 13.16 | 1991 |
| DEMO_004 | 14.89 | 19.8 | 19.66 | 9.77 | 1844 |

Эталон скорости цели: 8.4 М = 35 FPS (4 демо). Нужно: DEMO_001 −1.5, DEMO_002 −1.6, DEMO_003 −7.6, DEMO_004 −6.5 М/кадр.

Регрессионный эталон: PS2REF-ELF HEAD (`SRB2_PS2_NO= python3 tools/ps2/build.py --ps2ref`): `golden_check.py --pixels` на `golden/ps2-head/DEMO_00n` — для всех четырёх **tics identical (1050), 30 frames 0 differ**;
`-ps2ref-hashall` базы сохранены (см. выше).

## 2. Профиль (statistical sampler `--sample`, `-ps2sample`; нормировано на PROF total)

(заполняется)

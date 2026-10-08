# OPT12, агент HWDRV: GS Hardware Renderer — драйвер GS/VU1/VU0, темп кадров, полнота и точность картинки, VRAM (отчёт, дописывается по этапам)

Ветка `opt12-hwdrv`, worktree `/home/user/wt/hwdrv`. Реестр отличий PS2-HW-440..479. Единицы: такты COP0 EE на кадр (`HWPROF wall`), PCSX2 **2.8.2** (программный GS, 32 МБ), `--prof` + LTO,
`-renderer Hardware -zreserve 1536`, окна по 105 кадров (окно 0 = загрузка карты, среднее и максимум по окнам 1..9), FPS = 294.912 М / такты.

## 0. Как мерить (скрипты не в git: `build/run.sh`; в git: `tools/ps2/hwsum.py`, `tools/ps2/hwdrv_sum.py`)
* Сборка: `export PS2DEV=/opt/ps2dev-x/ps2dev; SRB2_PS2_OUT=$PWD/build/out SRB2_PS2_NO= SRB2_PS2_HW=1 python3 tools/ps2/build.py --jobs 2 --prof` (LTO, ≈ 5 мин при нагрузке 10+).
* Прогон: `python3 tools/ps2/opt_run.py --name N --elf ELF --pak build/pak --out build/runs --demo DEMO_00n --no-ref --until "gametics in" --timeout 900 -- -ps2prof -renderer Hardware -zreserve 1536 [арг.]`
  (≈ 50 с на DEMO_001, 1.5 мин на остальные; `build/run.sh NAME ELF DEMO_00n [арг.]`). Таблица: `python3 tools/ps2/hwsum.py --skip 1 N`, стоимость драйвера: `python3 tools/ps2/hwdrv_sum.py N`.
* Реальное время с интерполяцией: `opt_run.py ... --playdemo --cfg 'fpscap "Match refresh rate"' --until "HWPROF win=12 "` (тестовый `reference.cfg` ставит `fpscap "35"`: без `--cfg` интерполяции нет).

## 1. Исходные числа (ELF HEAD `455b669` + `--prof`, `build/base.ELF`, прогон `b_d1..b_d4`)
| демо | wall среднее / максимум (окна 1..9), М | FPS по среднему | `timed ... realtics` | `drv draw` | драйвер всего + планировщик (HWPROF23), М |
|---|---:|---:|---:|---:|---:|
| DEMO_001 | 6.73 / 8.99 | 43.8 | 1051 тик за 831 realtics (44.2 FPS) | 0.55 | 1.76 (макс 2.24) |
| DEMO_002 | 10.30 / 23.71 | 28.6 | 1352 (27.2 FPS) | 0.24 | 4.10 (макс 16.76: аномалия) |
| DEMO_003 | 9.72 / 14.15 | 30.3 | 1206 (30.5 FPS) | 0.30 | 1.67 (макс 2.92) |
| DEMO_004 | 9.74 / 12.70 | 30.3 | 1211 (30.4 FPS) | 0.43 | 2.54 (макс 3.36) |

## 2. Этап 1: темп кадров, vsync, flip (задача 1)
### 2.1 Что оказалось (измерено)
* **Режим вывода в этой среде — PAL, 50 Гц** (BIOS «Europe v02.00», `pcsx2.log`: `Set GS CRTC configuration. PAL 640x512 @ 50.000`). Один vblank = 5.898 М тактов EE, а не 4.92 М. Поэтому `flipwait` ≈ 2.7 М из 5.95 М
  в лёгких окнах D1 — это не простой «из-за квантования 60/30», а ровно запас до следующего vblank: окно 3 D1: 105 кадров за 106 vblank (`vbl=106`), работа EE ≈ 3.2 М из 5.9 М. «60 FPS = 4.9 М» верно только для NTSC;
  на PAL-консоли предел показа 50 кадров/с (`I_GetRefreshRate` = 50: интерполяция идёт до 50, а не до 60).
* **Схема flip («gate», `ps2_hw_gs.inc`)**: два буфера цвета + один Z. `frame_end` кадра N ждёт flip кадра N-1 (`flip_wait`), потом ставит `pend`; DMA кадра N+1 удерживается (`dma_start_locked`) до flip кадра N
  (кадр N+1 рисуется в буфер, который показывается до этого flip). Следствие: пока работа EE `w` меньше периода vblank `P`, период кадра ровно `P` (EE ждёт); при `w > P` EE не ждёт совсем (`flipwait = 0`) и период кадра = `w`,
  а не `2P`. То есть **квантования 60/30 нет** (в окнах с `w` чуть больше `P`: wall 6.45 при `flipwait` 0.17 и `vbl` = 114/105 кадров).
* Тройная буферизация не даёт выигрыша по тактам EE при таких работе и GS: GS (и в PCSX2, и по оценке для железа: 320x224 с перекрытием 3–4, 3 МБ текстур, 3 К полигонов — единицы мс) успевает за vblank.
  Она дала бы только сглаживание выбросов (кадр 9 М после кадров по 4 М) и стоила бы 280 КБ VRAM (+1 буфер 320x224 CT32) из 3.2 МБ пула текстур.

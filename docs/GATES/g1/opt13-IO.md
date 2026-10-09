# OPT13-IO: ввод-вывод — читатель паков, предзагрузка, повторы чтения, безопасная запись, keepalive сети, сон без будильников

Роль IO волны реализации OPT13 (2026-10-09). Worktree `/home/user/wt/io`, ветка `opt13i-io`, база — HEAD основной ветки с OPT12 (`ea3d0fa`). PCSX2 2.8.2 в Linux-контейнере, 32 МБ,
`host:` (диск Linux). Основа — `docs/research/rsys/OPT13_RSYS.md` (RS-01…RS-16, S-00…S-16), прототипы в `docs/research/rsys/patches`.
Отчёт дописывается и коммитится после каждого пункта; ниже «Сделано» — только то, что запущено, «Не проверено» — всё остальное.

**Главное ограничение, повторённое в каждой цифре ниже:** эмулятор отдаёт `host:` со скоростью диска Linux, носителей консоли в нём нет. Число и размер команд устройству (`IO R …` в журнале) —
**измерены** (то, что движок реально просит у IOP); время DVD/USB/MX4SIO/HDD — **модель** `tools/ps2/io_log_summary.py` (допущения `OPT13_RSYS.md` раздел 1: DVD 4 МБ/с seek 100/25 мс;
USB 1.1 1 МБ/с и 3 мс на команду; MX4SIO 3 МБ/с, 1 мс; HDD 15 МБ/с, seek 12 мс). Консоль не использовалась: всё, что зависит от железа, помечено «не проверено на консоли».

## 0. Стенд и команды

```
export PS2DEV=/opt/ps2dev-x/ps2dev
# ELF с журналом устройства (только для замеров; в релизную сборку не входит)
SRB2_PS2_OUT=$PWD/build/out-io3 SRB2_PS2_NO= SRB2_PS2_HW=1 SRB2_PS2_IOLOG=1 python3 tools/ps2/build.py --jobs 2
python3 tools/ps2/opt_run.py --name N --elf ELF --pak build/pak2 --out build/runs --map MAP01 -- -zquit 10 -loadprof -loadhash          # MAP01 software
python3 tools/ps2/opt_run.py --name N --elf ELF --pak build/pak2 --out build/runs --demo DEMO_002 --no-ref -- -renderer Hardware -zquit 700   # D2 (карта 4) HW, 700 кадров; D4 = DEMO_004 (карта 50)
python3 tools/ps2/io_log_summary.py build/runs/N/pcsx2.log          # команды устройству, байты, по файлам; модель носителей; фазы: «старт» и «после Entering main game loop»
python3 -B tools/ps2/test_pack_reader.py --pak build/pak2 --src /opt/srb2-assets --out build/hosttest/packtest2   # хост-тест читателя (v2); --pak build/pak: пак v1
python3 tools/ps2/verify_pack.py --src /opt/srb2-assets --pak build/pak2                                          # SHA-256 каждой лампы против pk3
```
Паки: `build/pak2` (SRP2 v2, без порядка хранения, `MODELS.PAK` собран), `build/pak` (SRP2 v1). Опции движка этой роли (все в `<datadir>/ps2args` тоже): `-pkmedium dvd|usb|sd|hdd|auto`,
`-pkwin cap,minreq,ahead,gap`, `-pkioerr at[,count]`, `-iostat`, `-pkprefetch [КБ]`, `-iobench`, `-amixhang N`.

## 1. База (до правок), журнал устройства

Базовая ELF `build/out-base` = HEAD + зонд журнала (читатель не менялся), паки `pak2`, три сценария, журнал `IO R` PCSX2:

| сценарий | старт до «Entering main game loop» | после (карта + игра) |
|---|---|---|
| MAP01, software, 10 кадров | 127 команд, 7.36 МБ | 113 команд, 7.24 МБ |
| D2 (карта 4) HW, 700 кадров | 130 команд, 7.50 МБ | **274 команды, 17.46 МБ** |
| D4 (карта 50) HW, 700 кадров | 130 команд, 7.50 МБ | **268 команд, 17.18 МБ** |

(в исследовании RSYS на `pak3` с порядком хранения: D2 14.2, D4 21.5 МБ; у `pak2` старт больше — нет порядка.)

# OPT12, агент NET: сеть PS2 <-> ПК-сервер, зависание подключения в Hardware, пинг (реестр PS2-NET-1..49)

Сессия 2026-10-08/09, ветка `opt12-net`, worktree `/home/user/wt/net`. PCSX2 2.8.2, Linux-контейнер (4 ядра на 5 агентов + эмуляторы: нагрузка 4..11).
Каталоги стенда (под `build/`, не в git): `build/out` (PS2-сборка, полная, `SRB2_PS2_NO= SRB2_PS2_HW=1`, LTO), `build/pc-net` (ПК-сборка с `-DNETSYNC_DIAG`),
`build/opt12-net/{specs,run,pc,pc-home1,pc-home2}`, `build/logs`.

## 0. Стенд (проверено запуском)

* Эмуляторы `/opt/pcsx2/net1`, `/opt/pcsx2/net2` — копии `/opt/pcsx2/slot0` (BIOS внутри слота, в репозиторий не копировался) с блоком
  `[DEV9/Eth] EthEnable=true EthApi=Sockets EthDevice=eth0 InterceptDHCP=true` и `extrathreads=0` в `[EmuCore/GS]`; рядом `usr/bin/PCSX2.ini.good` (восстановление пустого ini, см. `net_session.py:emu_path`).
* **Сеть PS2 внутри PCSX2 в контейнере работает**: DEV9 Sockets поверх единственного интерфейса контейнера `eth0` (192.0.2.2/24, шлюз 192.0.2.1); гость получает по DHCP (встроенный перехватчик PCSX2)
  `192.0.2.100/24`, шлюз `192.0.2.1`, DNS `8.8.4.4`. Другой схемы (loopback/второй адаптер) не понадобилось. Проверено: `pcsrv-ps2cli` (ПК dedicated + PS2-клиент software): 73 с,
  NETSYNC gametic=1400 на обеих сторонах; `hw-net-coop` (PS2-клиент `-renderer Hardware`): `VIDSHOT COMPLETE`, NETSYNC идёт.
* ПК-сторона: `cmake -S . -B build/pc-net -G Ninja -DCMAKE_BUILD_TYPE=Release -DSRB2_CONFIG_STATIC_STDLIB=OFF -DSRB2_CONFIG_USE_GME=OFF -DSRB2_CONFIG_HWRENDER=OFF -DCMAKE_C_FLAGS=-DNETSYNC_DIAG && ninja -C build/pc-net -j2 SRB2SDL2`
  -> `build/pc-net/bin/lsdlsrb2_opt12-net`. PS2: `SRB2_PS2_OUT=$PWD/build/out SRB2_PS2_NO= SRB2_PS2_HW=1 python3 tools/ps2/build.py --jobs 2` (LTO, ≈ 4 мин при нагрузке).
* Сценарии: `tools/ps2/net_specs.py`, `net_specs9.py` (прежние), `net_specs12.py` (новые: `lat-sw`, `lat-hw`, `hw-osk-connect`, `hw-netcmd-connect`, `netup-so`, `netup-ha`), запуск
  `python3 tools/ps2/net_session.py build/opt12-net/specs/NAME.json`. `tools/ps2/net_env.py`: `BASE = build/opt12-net`.
* Новое в стенде: `net_session.py` пишет рядом с логом каждого узла `boot.txt.ts` / `out.txt.ts` — каждая строка с **временем хоста** (секунды с эпохи); логи неудачной попытки при
  `--retries` не затираются, а сохраняются как `<имя>.failN` (в OPT10/OPT11 «зависший эмулятор» молча перезапускался — см. раздел 1: это был настоящий зависон игры).

## 1. Зависание при подключении к сети в Hardware (PS2-NET-2/3)

**Воспроизведено.** Сценарий `netup-ha` (PS2 `-renderer Hardware -connect HOSTIP`, сервера нет; ждём строку `PS2 net: address`, т.е. подъём сети: модули, линк, DHCP) на ELF до правок
(`build/elf-base.ELF`, 8 запусков, `build/loop.sh`): **3 из 8 зависли** (rc 2, лог движка замолкает, эмулятор жив). С нитью-сторожем (раздел ниже) ещё 14 запусков: 4 зависания.
Итого на 22 запусках HW-подъёма сети: **7 зависаний (32 %)**; в Software таких в сессии не было (но 2 сессии из 3 — единичные). `lat-hw` в первом же запуске завис на ожидании линка. То есть
«PS2-клиент HW ↔ ПК-сервер даёт таймаут» из OPT11 — не «не воспроизводится / зависший эмулятор» (так это помечено в OPT10-X раздел 0 и OPT11-STAB 10.3 и **молча перезапускалось** `--retries`),
а настоящий зависон гостя, который случается примерно в каждом третьем подъёме сети в HW.

**Что видно.** Гость стоит в BIOS-цикле простоя (`pc = 0x81fc0`, `ra = 0` по gdb: `tools/ps2/hang_probe.gdb`), эмулятор здоров (CPU thread в Throttle, GS thread ждёт работы, 76 % CPU).
Лог движка замолкает всегда в одном месте: ≈ 5 с шага «ждём DHCP» (или ожидание линка), после кадра HWFX ≈ 299.

**Нить-сторож (`-netwd`, `ps2_net.c`, PS2-NET-3).** Отдельная нить приоритета 1 раз в секунду смотрит счётчик `ps2net_beat` (растёт при каждом чтении `I_GetPreciseTime`); если он стоит 3 с,
выводит прямой `write(1, ...)` (без блокировки stdio) состояние всех нитей EE (`ReferThreadStatus`) и семафоров. Результат на 3 зависших запусках одинаков:

* игровая нить (tid 1, приоритет 8): `status=WAIT, last syscall WaitSema, waitId=42` — это **собственный семафор `DelayThread`** (`DelayThread` в ps2sdk = `CreateSema` + `SetTimerAlarm(..., DelayThreadWakeup_callback, sema)` + `WaitSema` + `DeleteSema`;
  игровая нить спит `I_Sleep(1)` в цикле ожидания Tick() NetUI тысячу раз в секунду);
* семафор 42 существует, `count=0, waiting=1`; ни одного будильника библиотеки таймера на него нет; таймер при этом жив — нить-сторож спит `DelayThread(1 с)` и просыпается, аудионити (приоритеты 5 и 7) тоже ходят по `DelayThread`;
* дамп памяти гостя через gdb (`eeMem + 0x695a80`, состояние библиотеки таймера ps2sdk): 4 активных будильника (4 мс, 10 мс, 100 мс — аудио и lwIP, 1 с — сторож), узел будильника игровой нити (1 мс) лежит в **списке свободных**:
  будильник «отработан и освобождён», а `iSignalSema(42)` до ожидающей нити не дошёл (потерянное пробуждение).
* остальные нити: netman (Tx/Rx/RPC, приоритеты 86/87/89) в `SleepThread`, tcpip_thread lwIP (88) в `WaitSema` на своём ящике — всё в штатных состояниях; взаимной блокировки по семафорам сети нет.

**Причина (установлена по состоянию гостя + подтверждена исчезновением зависаний при замене `DelayThread`).** Библиотека будильников ps2sdk (`SetTimerAlarm`, таймер 2, программный список будильников, общий с `WaitSemaEx` lwIP и со всеми `DelayThread`) теряет пробуждение
при плотной нагрузке на неё (netman/lwIP + 1000 `DelayThread(1 мс)` в секунду от игровой нити + короткие `DelayThread(30..100 мкс)` драйвера HW в ожидании GS/DMA + прерывания VBLANK/GIF/VIF1 в HW).
Нить, чей будильник потерян, спит навсегда. В HW окно шире (прерывания драйвера), поэтому там и видно. Точный механизм гонки внутри библиотеки не разбирался (дизассемблирован `TimerHandler_callback`/`SetNextComp`/`iSetTimerAlarm`,
вызовы коллбэков идут с `ei` внутри обработчика); доказательство — состояние гостя при зависании и исчезновение зависаний после исправления (ниже).

**Исправление (PS2-NET-3).** Игровая нить больше не использует `DelayThread`: `PS2_SleepUs(us)` (`src/ps2/i_system.c`) временно опускает приоритет нити до 120 и читает часы до срока —
все остальные нити (аудио, netman, lwIP, ядро) выше и выполняются первыми, а будить нить нечему (будильников нет). `I_Sleep`, `I_SleepDuration` и четыре «опросных сна» драйвера HW
(`ps2_hw_gs.inc`: ожидание FINISH/DMA/vblank, 30..100 мкс) переведены на неё. 

**Проверка (запуском).** `netup-ha` на `build/elf-e1.ELF` (только это исправление): **14 запусков из 14 дошли до `PS2 net: address`** (31..37 с), ни одного зависания; до исправления зависало 7 из 22 (32 %);
вероятность 14 удач подряд при прежних 32 % — 0.5 %. Команда: `build/loop.sh build/opt12-net/specs/netup-ha.json 14 e1` (цикл с `--retries 0`, каждый запуск сохраняется как `build/opt12-net/run/netup-ha.e1.N`).

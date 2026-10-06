# OPT10, агент X (контент и сеть на Linux): рабочий журнал

Сессия 2026-10-06/07, Linux-контейнер, ветка `worktree-agent-af7475cde787ad754`. Каталоги (все под `build/`, не в git): `build/out` (полная сборка), `build/pc-net` (ПК-сборка для сети),
`build/opt10-x/{specs,run,addons,pc-home1,pc-home2}`, `build/runs`, `build/logs`.
Продолжение `opt9-F.md`, `opt9-N.md`. Реестр: `PS2-100..139` (продолжение).

## 0. Стенд (проверено запуском)

* Полная сборка: `SRB2_PS2_OUT=$PWD/build/out SRB2_PS2_NO= SRB2_PS2_HW=1 python3 tools/ps2/build.py --jobs 2` — 182/182 файлов, LTO 112 с, **ELF 10 649 392 Б**.
  Загрузка: `opt_run.py --name smoke0 --map MAP01 -- -zquit 120` -> `ZQUIT DONE`, 0 ошибок, арена 24.4 МБ, свободно 7.7 МБ (`build/runs/smoke0`).
* Эмуляторы сети: `/opt/pcsx2/net1`, `net2`: в `usr/bin/inis/PCSX2.ini` `[DEV9/Eth] EthEnable=true EthApi=Sockets EthDevice=eth0` (единственный интерфейс контейнера, 192.0.2.2). Гость получает по DHCP 192.0.2.100, шлюз 192.0.2.1.
* ПК-сторона: `build/pc-ref` (PS2REF) НЕ годится для сети — в нём нет хука `-netsync` (`#if PS2_PROFILE || NETSYNC_DIAG`). Собран `build/pc-net` из этого дерева:
  `cmake -S . -B build/pc-net -G Ninja -DCMAKE_BUILD_TYPE=Release -DSRB2_CONFIG_STATIC_STDLIB=OFF -DSRB2_CONFIG_USE_GME=OFF -DSRB2_CONFIG_HWRENDER=OFF -DCMAKE_C_FLAGS=-DNETSYNC_DIAG && ninja -C build/pc-net -j2 SRB2SDL2`.
* `tools/ps2/net_session.py` портирован (Linux): PS2-узлы — `xvfb-run` + AppImage-копия (`net1`/`net2`), аргументы движка в `ps2args` (`-gameargs` режется на ~128 символах), ПК-узлы — Linux-бинарь под своим Xvfb (dedicated — без него),
  `SRB2WADDIR=/opt/srb2-assets`, процесс-группы (SIGINT, потом SIGKILL), собственная блокировка `NETLOCK`, `--retries N` при падении эмулятора/DEV9. `net_specs.py`, `net_specs9.py` — те же сценарии, пути из `tools/ps2/net_env.py`.
  Новые инструменты: `udp_probe.py` (UDP-зонд), `udp_sniff.py` (AF_PACKET-сниффер, в контейнере работает от root).

## 1. Сеть PS2 <-> ПК (проверено запуском)

Находки по пути:
1. DEV9 Sockets на Linux работает в обе стороны (зонд `udp_probe.py`: ПК -> PS2 `rx` растёт, PS2 `punch` -> ПК доходит; сниффер на `lo` показал пакеты 192.0.2.2:5029 <-> :5030).
2. ПК-клиент не заходил: он стоит на экране «информация о сервере» (`CL_VIEWSERVER`, ENTER) — на Windows в OPT9 эту клавишу кто-то нажимал, под Xvfb нажать некому. **Правка в чужом файле** (`src/netcode/client_connection.c`, только `#ifdef NETSYNC_DIAG`, макрос
   `NETSYNC_AUTOENTER`): ПК-клиент с `-netsync` сам «нажимает ENTER» на экранах информации о сервере и подтверждения аддонов. В обычной ПК-сборке (без `-DNETSYNC_DIAG`) код не существует.
3. Каталог `-home DIR/.srb2` движок не создаёт (`I_Error: Can't create file .../$$$.sav`) — `net_session.py` создаёт его.

| Сценарий | Команда | Результат |
|---|---|---|
| PS2-сервер (MAP01) + ПК-клиент | `net_session.py build/opt10-x/specs/ps2srv-pccli.json` | клиент вошёл (`players=2`), 68.5 с; `netsync_compare` srv/cli: 37 общих тиков (210..1470), **0 отличий** |
| ПК dedicated + PS2-клиент | `.../pcsrv-ps2cli.json` | 75.8 с; 43 общих тика (35..1505), **0 отличий** (`players=1`: dedicated не игрок) |

# OPT10, агент X (контент и сеть на Linux): рабочий журнал

Сессия 2026-10-06/07, Linux-контейнер, ветка `worktree-agent-af7475cde787ad754`. Каталоги (все под `build/`, не в git): `build/out` (полная сборка), `build/pc-net` (ПК-сборка для сети),
`build/opt10-x/{specs,run,addons,pc-home1,pc-home2}`, `build/runs`, `build/logs`.
Продолжение `opt9-F.md`, `opt9-N.md`. Реестр: `PS2-100..139` (продолжение).

## !!! Инцидент: регистрация на НАСТОЯЩЕМ мастер-сервере (нарушение запрета брифа)

Сценарий `menu-browse` (первая версия `net_specs9.py`, прогон 2026-10-06 ~23:56 UTC) запускал ПК-dedicated сервер с `-room 1 +masterserver http://HOSTIP:8090/MS/0 +servername "PC test server"`. Движок регистрирует сервер при старте
(`D_CheckNetGame` -> `RegisterServer()` при `masterserver_room_id > 0`), а `+`-параметры применяются позже, поэтому он **дважды отправил `POST https://ds.ms.srb2.org/MS/0/rooms/1/register`** (строки `HMS: connecting 'https://ds.ms.srb2.org/MS/0/rooms/1/register'`
в `build/opt10-x/run/menu-browse/srv/out.txt`, ~23:56:07 UTC), только потом URL сменился на mock. Процесс убит через ~2 мин (без `unlist`: токена регистрации у меня нет).
Последствие, **проверенное чтением списка** (HTTP GET, разрешён): в настоящем списке (секция комнаты `1`) в 00:12 UTC, через ~16 мин, висят `160.79.106.128 5029 SRB2%20server 2.2.15` и `160.79.106.141 5029 SRB2%20server 2.2.15`
(адреса egress-прокси контейнера, порт по умолчанию 5029, имя по умолчанию «SRB2 server») — это мои записи. Обновлений они не получают, убрать их без токена я не могу (удалить может администрация мастер-сервера, либо они истекут сами).
Других обращений к `ds.ms.srb2.org` в логах моих запусков нет (grep по `run/*/*/out.txt`, `boot.txt`, `pc/*/pc.out`: единственный файл — этот). Версия-запросы (`GET .../versions/18`) ПК-движок при старте тоже слал на настоящий адрес.
Пользователю/координатору отправлено срочное сообщение сразу после обнаружения (00:13 UTC).

Исправления стенда (`tools/ps2/net_session.py`, `net_specs9.py`):
1. каждому ПК-движку перед стартом пишется `<home>/.srb2/config.cfg` с `masterserver "<mock | relay | http://127.0.0.1:9/MS/0>"` (читается ДО старта сервера); комната и имя для регистрации на mock — тоже из этого файла, а не `-room`/`+`-параметрами;
2. у ПК-движков снимаются `https_proxy`/`http_proxy`/`all_proxy`: наружу из контейнера они выйти не могут;
3. каждому PS2-узлу в `reference.cfg` пишется `masterserver` (по умолчанию мёртвый локальный порт; у сценариев с mock/relay — их URL);
4. после каждой сессии логи всех узлов проверяются на `ds.ms.srb2.org` (`MASTER SERVER AUDIT FAILED`, код возврата 4).
Проверка по требованию координатора «dedicated без явного masterserver не регистрируется нигде»: (а) по коду `RegisterServer()` вызывается только при `masterserver_room_id > 0` (`d_clisrv.c:814`, `mserv.c:456/562/592`), умолчание `-1` (`mserv.c:71`);
комнату задаёт только `-room N` (`d_main.c:1737`) или выбор комнаты в меню; (б) все ~25 прогонов PC-dedicated/PC-клиентов и PS2-серверов до и после инцидента без `-room` не содержат ни `Registering this server`, ни `HMS:` (grep по всем `out.txt`/`boot.txt`);
(в) `python3 tools/ps2/dedicated_noreg_test.py`: настоящий бинарь `-dedicated -server -warp MAP01` как в соак-сценариях, 25 с: `server started: True; master-server lines: 0`, `OK`. Единственный путь к регистрации — явный `-room`/`masterserver_room_id`, он теперь есть
только у сценариев с mock (в `config.cfg` вместе с URL mock).
Чтение настоящего списка теперь только через `tools/ps2/ms_relay.py` (пропускает GET `rooms`/`servers`/`rooms/N/servers`/`versions/N`, остальное — 405/403; журнал `relay.jsonl`).

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
| PS2-клиент -> ПК dedicated, аддон NSK.pk3 (скин+Lua+SOC, 408 КиБ) по UDP игрового соединения (`addons-udp`) | `net_batch.py addons-udp` | скачан (`Downloading addon "NSK.pk3" from the server`), `Added file host:/.srb2/DOWNLOAD/NSK.pk3 (514 lumps)`, `Added skin 'ztest'`, Lua `FTLUA skin ... nsk running`, `$$$.sav` принят; 88 с; 43 общих тика (35..1505), **0 отличий** |
| то же по HTTP-источнику (`addons-http`: NSK.pk3+ZT.pk3, `+http_source http://HOSTIP:8091`, `tools/ps2/http_static.py`) | `addons-http` | 94 с; 51 тик (35..1785), **0 отличий**; запросы GET в `build/opt10-x/run/addons-http/http/http.jsonl` |
| HTTP-источник всегда 404 -> откат на UDP (`addons-http-404`) | `addons-http-404` | 96 с; 51 тик, **0 отличий** |

| **PS2-сервер <-> PS2-клиент, соак** (`soak-ps2srv-ps2cli`, два PCSX2 `net1`/`net2`, оба игрока ходят и прыгают по `-padscript`, `resynchattempts 0`, `blamecfail On`) | `net_batch.py soak-ps2srv-ps2cli`, `netsync_compare.py srv/boot.txt cli/boot.txt --min-players 2` | сервер дошёл до gametic 13 335, клиент до 8 470: **216 общих отсчётов NETSYNC, gametic 945..8470 (7 525 тиков с двумя игроками), 0 отличий** (`state`, `cons`, `rnd` совпадают); ошибок/ресинхронизаций/таймаутов нет (`build/opt10-x/run/soak-ps2srv-ps2cli/compare.json`). Сборка с BACKUPTICS 1024 (см. раздел 3). |

## 2. PS2-сервер выбрасывал присоединяющегося PS2-клиента: причина и исправление (PS2-139)

Симптом (воспроизводился на нагруженной машине, load average 12-15 при шести агентах с эмуляторами и LTO-сборками): `soak-ps2srv-ps2cli`: клиент входит, грузит сейв, затем сервер печатает `*Soni left the game (Connection timeout)`,
клиент через `nettimeout` тиков своего времени — `PS2 net: server timeout (no packet from the server for N tics), back to the title screen`; поток сервер->клиент превращается в одни `punch` (сниффер `udp_sniff.py`, `udp_sniff_sum.py`: пакеты по секундам).

Поиск (запуском): сначала считал причиной `nettimeout`/`jointimeout` — подняты до максимума 2100 в `reference.cfg` обоих узлов: **не помогло** (сервер выбрасывал клиента через те же ~220 тиков после входа).
Диагностическая строка в `Net_ConnectionTimeout` (`PS2 net: timeout node N: now .. lastrecv .. freeze .. connectiontimeout .. jointimeout ..`, PS2-139) показала: `now 1959 lastrecv 1957 freeze 3863 connectiontimeout 2100` — ни один из часов не истёк.
Вызывал `d_clisrv.c:TryRunTics`: `if (maketic + realtics >= netnodes[i].tic + BACKUPTICS - TICRATE) Net_ConnectionTimeout(i)`: узел, чей подтверждённый тик отстаёт от `maketic` на `BACKUPTICS - TICRATE`, выбрасывается (иначе кольцо тиков переполнилось бы).
На PS2 `BACKUPTICS = 256` (PS2-123) => предел **221 игровой тик** (6.3 с игрового времени; PS2-сервер работает медленнее 35 тиков/с, `I_GetTime` у него убегал вперёд ~1.7x: `now 1959` при gametic 1140). Клиенту на загрузку уровня после входа этого не хватает.
Совпало точно: вход на gametic ~920, выброс на ~1140.

Исправление: `src/netcode/protocol.h`: `BACKUPTICS = 1024` (как на ПК; терпит 28 с отставания). Цена: `netcmds` 74 КБ -> 295 КБ (+221 КБ статической памяти); измерено `opt_run.py --map MAP01 -zquit 120`: арена `24379392 -> 24158208` (-216 КБ), свободно на MAP01 7 677 536 -> 7 456 352 Б, `used` тот же (16 701 856).
Координатору/S: если памяти не хватает на самых больших картах — 512 (терпит 13.6 с) — промежуточный вариант; с 256 PS2-сервер выбрасывает клиентов, которым нужно больше 6 с на загрузку.
После исправления `soak-ps2srv-ps2cli`: таймаутов нет (см. строку таблицы выше). Прежние прогоны `nettimeout 2100` в тестах оставлены (`CFG_SYNC`, `pcsrv(longto=True)`); сценарии таймаутов (`server-kill`, `client-kill`, `reconnect`) на умолчаниях.
В копиях `/opt/pcsx2/net1`, `net2` `extrathreads = 0` (программный GS без своих потоков: меньше CPU при двух эмуляторах).

## 3. Контент в полной конфигурации (проверено запуском)

Сборка `SRB2_PS2_NO= SRB2_PS2_HW=1` (ELF 10 649 648 Б), все прогоны через `ftest_run.py`/`addon_compare.py`/`opt_run.py`. Тестовые аддоны — `tools/ps2/make_addons.py` (`zip lua lim skin nsk sum demo hud`) и `tools/ps2/make_udmf_map.py`;
настоящие аддоны пользователя с `D:\games\SRB2\addons` и ZombieEscape2 на Linux недоступны, а чужие публичные аддоны с GitHub скачать не удалось (репозитории вне доступа сессии: `Access denied: repository ... is not configured for this session`) — их проверка из OPT9 (Windows, 10 аддонов) не повторялась.

| Что | Команда | Результат |
|---|---|---|
| ZIP/PNG (`ZT.pk3`, 34 записи, 6 PNG) | `ftest_run.py ... -- -file ZT.pk3 -ftest-lumps 4 -ftest-patches ...`, `ftest_check.py zip` | `lumps: 34 expected, 35 found, 0 problems` |
| Lua (`ZL.pk3`: математика, строки, таблицы, pcall, хуки, freeslot, SOC с выражениями) | `addon_compare.py --name zl --files ZL.pk3 --until "FTLUA think36"` (ПК-сборка `build/pc-net` и PS2 с одним списком аддонов + `ZSUM.pk3`) | 37 строк FTLUA на PS2 = 37 на ПК, **0 отличий**, предупреждений нет |
| Лимиты (`ZF.pk3`: 300 состояний, 40 типов, 300 звуков, 40 цветов, 20 sprite2, `G_AddGametype`, длинный спрайт, userdata до роста таблиц) | `addon_compare.py --name zf --until "FTLUA done"` | 41 строка = 41, **0 отличий**; 130 одинаковых предупреждений на обеих сторонах |
| Скин+звук+музыка+Lua+SOC (`ZS.pk3`) | `addon_compare.py --name zs` | 28 = 28, 0 отличий, 0 предупреждений |
| База без аддонов (`ZSUM.pk3` печатает через Lua-API скины/типы/состояния/цвета/звуки/карту) | `addon_compare.py --name sum-base --files ""` | 17 = 17, 0 отличий |
| **UDMF-карта** (`make_udmf_map.py`: 12 комнат 4x3, 31 линия, 20 вершин, XGL3-узлы, построенные самим генератором с проверкой обхода дерева) | `ftest_run.py -- -file UM.pk3 -ftest-level -warp 99 -zquit 100`, `ftest_check.py udmf` | счётчики `[20, 12, 31, 48, 15]` и CRC вершин/секторов/линий/сторон/вещей = независимый Python-парсер `udmf_ref.py` (`1 maps expected, 1 levels loaded, 0 problems`); `addon_compare.py --name um --warp 99`: 17 строк PS2 = ПК (хэши секторов/линий/вещей) |
| **Lua-HUD** (`ZH.pk3`: `v.drawFill/drawString/drawScaled/drawNum/cachePatch`, `hud.add(..., "game")`) в **software** и в **HW** | `ftest_run.py -- -file ZH.pk3 -skipintro -warp 1 [-renderer Hardware -zreserve 3072] -vidshot l90 -zquit 120` | `FTLUA hud first call 320x200 true`, `hud calls 40` в обоих режимах; снимки `docs/GATES/g1/opt10-X/hud-software.png`, `hud-hardware.png`: рамка, строки, масштабированное кольцо, счётчики на месте в обоих. `addon_compare --name zh`: единственное отличие — `v.width()`: 320 на PS2, 1280 на ПК (размер окна ПК) |
| **Меню Add-ons** с пада | `ftest_run.py --files ...ZH.pk3=.srb2/addons/ZH.pk3,... -- -padscript file:pad.txt -vidshot ...` (`padseq.py`: start, down x2, cross, cross, cross) | меню открывается, папка `.srb2/addons` показывает `ZH.pk3`, `ZL.pk3`; Cross на файле: `Added file host:/.srb2/addons/ZH.pk3 (1 lumps)`, `FTLUA hud loaded`, значок «загружен» (`docs/GATES/g1/opt10-X/menu-addons.jpg`) |
| **Автозагрузка** `<data>/autoload` | `ftest_run.py --files ZH.pk3=autoload/ZH.pk3,NSK.pk3=autoload/NSK.pk3 -- -skipintro -warp 1 -zquit 100` | `Loading Lua script from host:/autoload/ZH.pk3|Lua/ZHUD.lua`, скин `ztest`, `FTLUA nsk running`, HUD вызывается |
| **mc0:** (карта памяти) из каталога дистрибутива | `-ftest-mc ZF.pk3 -ftest-mcformat` из `dist/SRB2-PS2` | `module mcman.irx id=35 ret=0`, `mcserv.irx`, `FT_MC wrote mc0:/SRB2/ZF.pk3 4590`, `readback 4590 1daf12c4 SAME`, каталог `. .. ZF.pk3` |
| **mass:** (USB-накопитель) | то же с `-ftest-mcdev mass` | модули `bdm.irx`, `bdmfs_fatfs.irx`, `usbmass_bd.irx` загружены из `<data>/modules`, `FT_MC prepare mass: 0` — **накопителя в PCSX2 нет** (USB mass storage не эмулируется), чтение/запись `mass:` **не проверены** |
| **Ваниль не меняется** (все четыре демо, полная конфигурация без аддонов, `--ps2ref`, `tools/ps2/golden_full.sh`) | `SRB2_PS2_OUT=build/out-ref SRB2_PS2_NO= build.py --ps2ref`; `golden_check.py --run ... --ref golden/ps2-head/DEMO_00n --pixels` | **DEMO_001..004: тики идентичны (1050 строк), 30 кадров каждого — 0 differ (побитно)**; к ПК-golden тики DEMO_001/002/004 идентичны, DEMO_003: `state_hash` с тика 45 (известно, задача S) |


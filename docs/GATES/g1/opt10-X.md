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
1. каждому ПК-движку перед стартом пишутся `<home>/.srb2/config.cfg` **и `dconfig.cfg`** (dedicated-сервер читает именно `dconfig.cfg`: «Executing .../dconfig.cfg» в логе; первая версия защиты писала только `config.cfg` — это обнаружено следующим же прогоном `menu-browse`
   и исправлено; в тот промежуток защищали пп. 2 и 4, обращений к `ds.ms.srb2.org` в логах нет) с `masterserver "<mock | relay | http://127.0.0.1:9/MS/0>"` (читаются ДО старта сервера); комната и имя для регистрации на mock — тоже из этих файлов, а не `-room`/`+`-параметрами;
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
* **Итоговая сборка** (HEAD на 02:30 UTC, `build/out`, `SRB2_PS2_NO= SRB2_PS2_HW=1 build.py --jobs 2`, 182/182 файлов, `NOMD5` не определён): **ELF 10 657 324 Б**, `opt_run.py --name smoke-final --map MAP01 -- -zquit 120` -> `done=True errors=0`, arena 24 150 016, used 16 701 856, free 7 448 160.
  Все сетевые прогоны до 02:30 шли на предыдущей сборке `build/out4` (ELF 10 657 196 Б; `size`: text 4 325 688 против 4 325 712 — +24 Б кода; исходники `src` между сборками отличаются только переименованием номеров реестра в комментариях (`git diff 12192b8 b0bc9c8 -- src/w_wad.c src/netcode/d_net.c`) и меткой коммита/даты в строке версии;
  логика та же); ключевые сценарии перепрогнаны на итоговой сборке — таблица «итоговая сборка» в разделе 5.
* **Сбой стенда: PCSX2.ini копии `net1` стал пустым** (0 Б, 01:57 UTC): эмулятор убит SIGKILL в момент записи настроек при выходе (мастер-копия выходит дольше 8 с под нагрузкой). Пустой ini -> следующий запуск зависает на первом мастере настройки
  (`pcsx2.log` кончается строкой `Loading config from ...`, 0 % CPU, 10 минут простоя — потерян один прогон `reconnect`). Исправление в `net_session.py`: ожидание после SIGINT 25 с вместо 8, перед стартом пустой ini (< 1000 Б) восстанавливается из
  `<копия>/usr/bin/PCSX2.ini.good` (рабочие копии сделаны для `net1`, `net2`, `usbk`), сторож зависшего старта (нет `boot.txt` 300 с и `pcsx2.log` < 1500 Б -> код 3 и повтор по `--retries`).
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

## 2. PS2-сервер выбрасывал присоединяющегося PS2-клиента: причина и исправление (PS2-113)

Симптом (воспроизводился на нагруженной машине, load average 12-15 при шести агентах с эмуляторами и LTO-сборками): `soak-ps2srv-ps2cli`: клиент входит, грузит сейв, затем сервер печатает `*Soni left the game (Connection timeout)`,
клиент через `nettimeout` тиков своего времени — `PS2 net: server timeout (no packet from the server for N tics), back to the title screen`; поток сервер->клиент превращается в одни `punch` (сниффер `udp_sniff.py`, `udp_sniff_sum.py`: пакеты по секундам).

Поиск (запуском): сначала считал причиной `nettimeout`/`jointimeout` — подняты до максимума 2100 в `reference.cfg` обоих узлов: **не помогло** (сервер выбрасывал клиента через те же ~220 тиков после входа).
Диагностическая строка в `Net_ConnectionTimeout` (`PS2 net: timeout node N: now .. lastrecv .. freeze .. connectiontimeout .. jointimeout ..`, PS2-112) показала: `now 1959 lastrecv 1957 freeze 3863 connectiontimeout 2100` — ни один из часов не истёк.
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


## 4. Демо и MD5 аддонов (PS2-111), память ванили, клавиатура

* **MD5 на PS2 был выключен целиком** (`-DNOMD5` в `build.py`): все дайджесты нулевые. Следствия, найденные запуском (`tools/ps2/demo_addon_test.py`): демо, записанное с аддонами, не воспроизводится
  (`ERROR: Required files for this demo are loaded out of order`: у всех записей файлов дайджест 0, поэтому каждая совпадает с первым «важным» файлом и порядок «неверный»); PS2-сервер отдаёт клиентам список файлов с нулевыми дайджестами;
  удалённое администрирование (`password`/`login`) отключено; контрольная сумма демо — случайные байты; MD5 карты в демо нулевой.
* Исправление (PS2-111): `-DNOMD5` только в урезанном профиле (`SRB2_PS2_NO` содержит `addons`); `w_wad.c:W_InitFile` считает MD5 **только для файлов аддонов** (pk3/wad/soc/lua), cooked-пак (100 МБ `MUSIC.PAK`) не хешируется — его дайджест остаётся нулевым;
  проверка «уже загружен» и `W_VerifyFileMD5` пропускают нулевой дайджест. Старт не замедлился (`t_ms` MAP01: 15095 / 15105 / 15106 мс до/после), арена -8 КБ (`24158208 -> 24150016`: код MD5).
* Проверка: `demo_addon_test.py --elf build/out4/SRB2.ELF --addons NSK.pk3,ZP.pk3`: запись 1100 тиков с аддонами (скин+Lua+SOC и Lua-логгер состояния, `ZP.pk3`), воспроизведение с теми же аддонами, Lua печатает позицию/кольца/счёт/скин/состояние ГСЧ раз в 35 тиков:
  **запись 32 строки, воспроизведение 29, общих 29 (leveltime 35..1015), различий 0** (`build/logs/demo3.txt`). До правки (`demo2`, нулевые дайджесты) воспроизведение отказывалось стартовать.
* **Память: ваниль не растёт от возвращённого контента.** MAP01, 120 кадров, `opt_run.py --map MAP01 -zquit 120`, software, PCSX2 32 МБ: урезанный профиль `SRB2_PS2_NO=lua,udmf,addons,limits` (`build/out-base`): арена 25 542 656, `used` 16 766 400, свободно 8 776 256;
  полная конфигурация (`build/out4`): арена 24 150 016, `used` 16 701 872, свободно 7 448 144. То есть `used` у полной даже меньше (таблицы лимитов стартуют малыми и растут только по требованию аддона, `PS2Limits_Grow`),
  а цена — статическая: арена на 1.39 МБ меньше (код Lua/UDMF/аддонов в ELF) и ELF 10.66 МБ против 9.54 МБ. Golden четырёх демо в полной конфигурации побитно равен `golden/ps2-head` (раздел 3).
* **USB-клавиатура на Linux** (`tools/ps2/kbd_x11.py`, копия эмулятора `/opt/pcsx2/usbk`: `[USB1] Type = hidkbd`, `[Pad1] Type = None`, хоткеи пусты, свой Xvfb, ввод `xdotool`): консоль с клавиатуры
  (`setcontrol "console" "f12"`: обратная кавычка эмулятором в HID не отдаётся — как в OPT9-K) — `$echo zqxkw` -> `zqxkw` (`build/runs/kbd4`); `-kbdlog` показывает сырые события (`raw down usage 0x04` для `a`). Ввод адреса сервера с клавиатуры — раздел 5, строка `kbd-addr`.
* **Переподключение (`reconnect`) — артефакт сценария, не движка.** Первый вариант падал: после `exitgame` и `connect` PS2 через ~250 тиков делал `I_Error: Tried to transmit to another node`. Диагностика PS2-112 (`D_QuitNetGame`/`CL_Reset`/`SV_StartSinglePlayerServer`/`Net_ConnectionTimeout` печатают `caller`;
  `tools/ps2/addr_sym.py` переводит адрес в функцию по `nm`): `SV_StartSinglePlayerServer netgame 1 server 0 client 1 caller G_DeferedInitNew+0x60` — паду-скрипт нажимал Cross каждые 100 тиков и на титульном экране после `exitgame` открыл меню и выбрал «1 Player»
  ПОД только что установленным соединением; игра перестала быть сетевой, а узел 1 остался `ingame` -> тайм-аут -> отправка пакета при `netgame=0`. Человек до меню во время сетевой игры не доберётся. Сценарий исправлен: Cross только в окнах присоединения (200..1100, 1780..2600).
  **Результат после исправления (запуском):** `reconnect` rc 0 за 1123 с: сервер печатает `Sonic has joined the game` -> `Sonic left the game` (после `exitgame` клиента, gametic 4928) -> `Sonic has rejoined the game` (клиент снова скачал `$$$.sav`, загрузил карту);
  `netsync_compare`: 160 общих отсчётов, gametic 35..6090, **0 отличий**. Через ~60 с после повторного входа тот же артефакт сценария выстрелил ещё раз: в логе `music O__CHSEL` (музыка выбора персонажа) и `SV_StartSinglePlayerServer netgame 1 server 0 client 1 caller G_DeferedInitNew`,
  потом `I_Error: Tried to transmit to another node` — поздние Cross сценария (кадры 1880+) выбрали «1 Player» в главном меню, которое, судя по логу, осталось открытым под экраном подключения после команды `connect` из консоли.
  Причина установлена по логу и адресам вызова (`addr_sym.py`), на ПК-сборке НЕ проверялась (возможно, так же в апстриме); человек, нажимающий Cross в игре, этого не вызовет. Сценарий зачтён по `rejoined`+0 отличий; переделка
  (Cross только в окне 1780..1900) не прогонялась.

## 5. Сводка сетевых прогонов (все — запуском, `net_batch.py` -> `net_session.py` + `netsync_compare.py`)

«Отсчётов» — общие строки `NETSYNC` (раз в 35 тиков: хэш состояния `state`, `cons`, `rnd`) двух узлов, «тики» — диапазон gametic. Во всех строках с отсчётами **0 отличий**. rc: 0 условие выполнено, 2 таймаут/условие не наступило,
3 эмулятор умер/DEV9, 5 `abort_on` (клиента выбросило по тайм-ауту), 6 лог-шторм. Все прогоны шли на машине с load average 12-34 на 4 ядрах (5-6 агентов с эмуляторами и сборками): медленный PS2-узел отстаёт, и сервер его выбрасывает
(`Connection timeout`) — поэтому часть сценариев пришлось повторять (отмечено). Прогнано на `build/out4`, если не сказано иное.

| Группа | Сценарий (имя в `net_specs*.py`) | rc | с | отсчётов / тики | Что проверено |
|---|---|---|---|---|---|
| PS2 <-> ПК | `ps2srv-pccli`, `pcsrv-ps2cli` | 0 | 68 / 76 | 37 / 43 (210..1470, 35..1505) | вход клиента, одинаковое состояние |
| | `soak-pcsrv-ps2cli` (ПК dedicated, PS2-клиент ходит/прыгает по `-padscript`) | 0 | 226 | **190 (35..6650)** | соак >= 5000 тиков |
| | `soak-ps2srv-pccli` (PS2-сервер, ПК-клиент) | 0 | 249 | **183 (280..6650)** | соак >= 5000 тиков, ПК-клиент -> PS2-сервер |
| | `addons-udp`, `addons-http`, `addons-http-404`, `addons-http-chunked` | 0 | 88 / 94 / 96 / 98 | 43 / 51 / 51 / 50 | аддон NSK.pk3 (скин+Lua+SOC) качается с ПК-сервера по UDP; по HTTP-источнику (`http_static.py`); при 404 — откат на UDP; chunked-ответ |
| | `mode-{match,ctf,race,tag,coop,teammatch}-pcsrv-ps2cli` | 0 (tag: rc 5, повтор rc 0) | 102-114 | 61-64 (35..~2200) | режимы Match/CTF/Race/Tag/Co-op/Team Match, ПК-сервер -> PS2-клиент |
| PS2 <-> PS2 | `soak-ps2srv-ps2cli` (два PCSX2) | 0* | 418 | **216 (945..8470, двое игроков)** | соак; *rc 2 было из-за неверного `until` (NETSYNC 6500 не кратен 35), состояние совпало |
| | `mode-{match,ctf,race,tag,coop}-ps2srv-ps2cli` | 0 | 107-124 | 36-43 (700..2170) | те же режимы, PS2-сервер <-> PS2-клиент |
| | `mode-teammatch-ps2srv-ps2cli` | 2, 5 (нагрузка) | 336, 133 | 36 / 0 | клиента выбросили: клиент не успел загрузить карту (на load 33 за 132 с он ещё грузил сейв, сервер ушёл на 988 тиков вперёд = правило `BACKUPTICS - TICRATE`); перепрогон — в таблице итоговой сборки |
| | `addons-ps2srv-ps2cli` / `addons-ps2srv-pccli` | см. итоговую сборку | | 101 (2380..5880) | аддон с PS2-хоста (`-file` в host:) качает PS2-/ПК-клиент |
| | `quit-match-2p`, `quit-coop-2p` (PS2-хост выходит командой `quit`) | 0 / 0 | 222 / 207 | 57 (770..2730) / 41 (665..2065) | клиент через 350 тиков печатает `server timeout ... back to the title screen`, без `I_Error` |
| | `quit-match`, `quit-coop` (хост один) | 0 / 0 | 104 / 111 | — | выход без `I_Error` (`D_QuitNetGame`, `end of logstream`) |
| Сплит-экран по сети | `split-net` (PS2-клиент в сетевой игре, консоль `splitscreen 1` на кадре 800, оба пада ходят) | 2 (остановлен вручную) | 439 | **380 (35..13300)** | **Оригинальный движок не поддерживает сплитскрин в сетевой игре** (`SplitScreen_OnChange`: `Splitscreen not supported in netplay, sorry!`, если не `cv_debug` — по исходнику `src/r_main.c:196`; самой строки в логе PS2 не нашлось): второй игрок не присоединяется, сервер видит `players=1` всё время; проверено, что отказ не ломает соединение и состояние (0 отличий на 13 300 тиках). Первый прогон (команда на кадре 1500 не наступила: на нагруженной машине кадров ~1 на 3 тика) — 85 отсчётов, 0 отличий. Сетевой сплитскрин как возможность — не цель порта |
| Мастер-сервер | `ps2host-mock`, `ps2host-menu` (PS2-хост регистрируется на mock, ПК-клиент заходит из списка; во втором игра создана из меню Host) | 0 / 0 | 74 / 549 | — / 365 (1575..14315) | `POST /rooms/1/register` принят mock (`ps2host-mock/mock.jsonl`), запись видна в списке, клиент вошёл |
| | `menu-browse` (PS2: Multiplayer -> комната -> список mock -> сервер -> Enter) | 0 | 200 | 44 (35..1540) | **ПК-сервер зарегистрировался на mock** (`POST` из `dconfig.cfg`), PS2-меню показало его строкой «SRB2 server», окно информации (28 мс, GREENFLOWER ZONE 1, Co-op, Dedicated), вход; `docs/GATES/g1/opt10-X/menu-browse-mock.jpg` |
| | `ms-blackhole`, `ms-refused` (мастер недоступен: адрес без маршрута / порт отказал) | 0 / 0 | 101 / 77 | — | `Registering this server...` -> `ERROR: There was a problem contacting the master server...`, игра идёт дальше (NETSYNC 700 достигнут) |
| | `real-ms-read` (ЧТЕНИЕ настоящего списка через `ms_relay.py`: GET `versions/18`, `rooms`, `servers`; остальное relay отвергает) | 0 | 69 | — | список настоящего мастера показан в PS2-меню (снимок не приложен: чужие имена серверов); **единственное** обращение к `ds.ms.srb2.org` этого отчёта, кроме инцидента; повторно не запускалось |
| Ввод адреса | `osk-connect` (экранная клавиатура, Triangle в меню ввода адреса), `osk-shot` | 0 / 0 | 111 / 67 | 43 (35..1505) | адрес набран кнопками, соединение с ПК-сервером, одинаковое состояние; `osk-address.jpg` |
| | `kbd-addr` (`kbd_x11.py --pc-server`: PCSX2 `usbk` с USB-клавиатурой, `xdotool`: Return, Down, Return, Down, набор `192.0.2.2`, Return, Return...) | 0 | ~60 | — (`NETSYNC` до 420) | меню Multiplayer -> адрес с клавиатуры -> `Contacting the server` -> `Sonic has joined the game` -> `MAP01` (`build/logs/kbd-addr.txt`) |
| Отказы | `server-kill` (сервер убит) | 0 | 75 | 20 | клиент: `server timeout`, возврат на титул, без `I_Error` |
| | `client-kill` (клиент убит) | 0 (после правки текста условия) | 77 | 15 | сервер: `left the game (Connection timeout)`, продолжает считать тики (`NETSYNC` идёт дальше; за окно grace видно 3 отсчёта, счётчик `players` в них ещё 2 — снятие слота дольше окна не прослежено) |
| | `reconnect` (клиент `exitgame`, `connect`) | 0 | 1123 | **160 (35..6090)** | `joined` -> `left` -> `rejoined`, см. раздел 4 |
| HW + сеть | `sw-net-coop` (контроль: PS2-клиент software) | 0 | 191 | 136 (35..4760) | — |
| | `hw-net-coop` (PS2-клиент `-renderer Hardware`, ПК-сервер) | 2 | 900 | 30 (35..1050) | HW-клиент падает (`Z_CheckHeap`), см. «не закрыто» |

## 6. Дистрибутив и запуск из него (проверено запуском)

`python3 tools/ps2/make_dist.py` -> `dist/SRB2-PS2/` (192.9 МиБ, жёсткие ссылки на паки, каталог в `.gitignore`): `SRB2.ELF` (итоговая сборка, 10 657 324 Б), `SRB2.PAK`, `ZONES.PAK`, `CHARS.PAK`, `MUSIC.PAK`, `FINEACON.DAT`, `modules/{mcman,mcserv,bdm,bdmfs_fatfs,usbmass_bd}.irx`,
`autoload/README.txt`, `ps2args.example`, `README.txt` (русский + английский: запуск на консоли — uLaunchELF/OPL/FreeMcBoot; запуск в PCSX2 — Boot ELF, HostFs, 32 МБ, сеть Sockets/PCAP, `punch` для сервера на PS2; аргументы `ps2args`).
Запуск из каталога дистрибутива в эмуляторе (`run_pcsx2.py --elf dist/SRB2-PS2/SRB2.ELF --args "-logfile boot.txt -skipintro -warp 1 -zquit 120"`): все четыре пака подключены (`Added file host:/SRB2.PAK (12614 lumps)`, `ZONES.PAK`, `CHARS.PAK`, `MUSIC.PAK`),
MAP01 загружена, `ZQUIT DONE`, 0 ошибок, арена 24 158 208, free 7 521 920 (`build/logs/dist-launch-boot.txt`). Артефакты запуска (`boot.txt`, `.srb2`) из `dist/` убраны.

## 7. Реестр отличий (диапазон X: `PS2-100..139`; номера 111-116 новые)

| ID | Что | Файлы | Чем проверено |
|---|---|---|---|
| PS2-111 | MD5 для файлов аддонов на PS2 (раньше `-DNOMD5` занулял все дайджесты: демо с аддонами не воспроизводились, сервер отдавал клиентам нулевые дайджесты); cooked-паки не хешируются, нулевой дайджест ничего не идентифицирует | `src/w_wad.c`, `tools/ps2/build.py` | `demo_addon_test.py`: запись/воспроизведение 29 общих строк, 0 различий; golden 4 демо побитно; старт MAP01 не медленнее |
| PS2-112 | диагностика сети PS2 (`PS2_PROFILE`): поля тайм-аута узла в `Net_ConnectionTimeout`, строки `CL_Reset`/`D_QuitNetGame`/`SV_StartSinglePlayerServer` с адресом вызова | `src/netcode/d_net.c`, `d_clisrv.c`, `tools/ps2/addr_sym.py` | нашли причину PS2-113 и артефакт `reconnect` |
| PS2-113 | `BACKUPTICS` на PS2 256 -> 1024 (PS2-сервер выбрасывал присоединяющегося клиента через 221 тик отставания; +216 КБ статической памяти) | `src/netcode/protocol.h` | `soak-ps2srv-ps2cli`: 216 отсчётов, 0 отличий, без тайм-аутов; арена -216 КБ |
| PS2-114 | `SRB2_PS2_NO` по умолчанию пуст: полная конфигурация (Lua, UDMF, аддоны, лимиты); `NOMD5` только в урезанном профиле | `tools/ps2/build.py` | полная сборка 182/182, smoke MAP01, все сценарии этого отчёта |
| PS2-115 | `NETSYNC_DIAG`: ПК-клиент с `-netsync` сам нажимает ENTER на экранах информации о сервере/подтверждения аддонов (под Xvfb нажать некому); в обычной ПК-сборке кода нет | `src/netcode/client_connection.c` | PS2-сервер <-> ПК-клиент, 183 отсчёта |
| PS2-116 | Стенд на Linux: `net_session.py` (ограждение мастер-сервера, аудит логов, сторож лог-шторма и зависшего старта, восстановление ini), `net_batch.py`, `net_specs*.py`, `net_env.py`, `kbd_x11.py`, `ms_relay.py`, `make_dist.py`, `udp_probe.py`, `udp_sniff*.py`, `padseq.py`, `make_udmf_map.py`, `demo_addon_test.py`, `dedicated_noreg_test.py`, `golden_full.sh` | `tools/ps2/*` | см. разделы 0-6 |

## 8. Правки в чужих файлах (по месту, ради стенда или найденной ошибки)

* `src/netcode/client_connection.c` — `NETSYNC_AUTOENTER` (только `#ifdef NETSYNC_DIAG`, PS2-115). `src/netcode/protocol.h` — `BACKUPTICS` (PS2-113: **затрагивает память: +216 КБ статически**; агенту S/координатору: если не хватает на больших картах — 512 даёт 13.6 с терпимого отставания).
* `src/netcode/d_net.c`, `d_clisrv.c` — диагностические строки под `PS2_PROFILE` (PS2-112). `src/w_wad.c` — MD5 только для аддонов (PS2-111).
* `tools/ps2/build.py` — умолчание `SRB2_PS2_NO` и `NOMD5` (PS2-114). `tools/ps2/opt_run.py` — `stage` подключает `FINEACON.DAT` к пакам (без него полная сборка не стартует). `tools/ps2/ftest_run.py`, `pc_run.py`, `addon_compare.py`, `make_addons.py` —
  перенос на Linux (`ps2args`, нормализация 64-битных хэшей, новые тестовые аддоны). `tools/ps2/run_pcsx2.py` — взят целиком из ветки `claude/determined-knuth-8c4nbi` (сторож лог-шторма 300 МБ, код 5; указание координатора). `.gitignore` — `/dist/`.

## 9. Не закрыто

* **`mass:` (USB-накопитель)** не проверен: PCSX2 не эмулирует USB mass storage; модули `bdm/bdmfs_fatfs/usbmass_bd` грузятся (`FT_MC prepare mass: 0`), чтение/запись нет. Проверка — на железе.
* **Настоящие сторонние аддоны** (десять из OPT9 на Windows, ZombieEscape2) на Linux недоступны, репозитории GitHub вне доступа сессии; проверены официальные ассеты и синтетические аддоны (ZIP/PNG, Lua, лимиты, скин, UDMF, HUD, демо с аддонами, сеть). Сохранения с аддонами не проверялись (демо — да).
* **HW + сеть/смена уровня**: PS2-клиент `-renderer Hardware` в сети падает (`Z_CheckHeap`, `hw-net-coop`; `hw-net-match` не прогонялся); смена уровня в HW (`-skipintro -warp 1 -renderer Hardware -zreserve 3072 -netcmd file:cmd2.txt`, `600:map MAP02`): на самой первой сборке (`43b9a7e`, `hw-map-out`) — `I_Error: Out of memory allocating 1048576 bytes`; на более поздних (`ec7fae3` `hw-map-2`, `12192b8` `hw-map-4`/`hw-map-out4`) OOM нет, уровень сменился (музыка `O_GFZ2`),
  кадры идут (`HWFX frame` 6299 / 5399 / 1199; `hw-map-2/4` крутились ~88 минут из-за моей ошибки в условии остановки: движок в одиночной игре строки `Map is now` не печатает — ни при первом, ни при втором уровне, проверено на software). Причину исчезновения OOM я не искал (память в HW зависит от состояния кэша текстур на момент смены) — **единичные успехи не доказывают отсутствие проблемы**; прогон итоговой сборки — ниже. Домен HT/HF/S.
* **Нагрузка стенда.** Сетевые сценарии зависят от загрузки процессора: при load > 30 на 4 ядрах медленный PS2-узел отстаёт, и сервер его выбрасывает по правилу `BACKUPTICS - TICRATE` (28 с при 1024). Повторы отмечены в таблицах.
* **Две записи на настоящем мастере** (`160.79.106.128:5029`, `160.79.106.141:5029`, «SRB2 server» — из инцидента, начало отчёта): убрать не могу (нет токена), по указанию координатора больше никаких обращений к `ds.ms.srb2.org` кроме разового чтения списка; 209.145.63.242 и чужие серверы не трогал.
* USB-клавиатура проверена только в PCSX2 (эмулируемый HID, `xdotool`); на настоящей консоли и с настоящими клавиатурами не проверялась. `mc0:` проверен записью/чтением файла на эмулируемой карте памяти.

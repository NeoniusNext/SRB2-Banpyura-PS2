# OPT9, агент N (сеть и мастер-сервер): рабочий журнал

Сессия 2026-10-06 (ночь). Каталог: `build/opt9-n/{out-full,specs,run,...}`; пакеты — `build/opt6-s/pak`; ПК-сборка SRB2 для сетевых тестов — `build/opt7-s/pc/srb2-s7pc.exe`.
Реестр: `PS2-120..139` (сеть/мастер-сервер). Эмулятор — только `net_session.py` (копии `D:/PCSX2-net1`, `D:/PCSX2-net2`, собственная блокировка) и `run_pcsx2.py`/`opt_run.py` для одиночных запусков.
Продолжение `opt6-S.md`/`opt7-S.md`/`opt8-S.md` (сетевая часть).

## 0. Старт (проверено запуском)

* Сборка полной конфигурации `SRB2_PS2_OUT=build/opt9-n/out-full SRB2_PS2_NO= SRB2_PS2_HW=1 python tools/ps2/build.py`: 178/178 файлов, ELF 10 987 828 Б (LTO-шаг 356 с при пяти параллельных сборках). Копия — `build/opt9-n/full0.ELF`.
* Запуск сессий: `bash build/opt9-n/netrun.sh specs NAME` (спеки `SRB2_NET_BASE=build/opt9-n python tools/ps2/net_specs.py ELF` → `build/opt9-n/specs`).

## 1. Базовые прогоны на полной сборке `full0.ELF` (проверено запуском)

Инструменты: `tools/ps2/net_specs9.py` (новые сценарии OPT9: `--elf`, `--base build/opt9-n`, имена), `bash build/opt9-n/netrun.sh specs NAME`, `build/opt9-n/montage.py` (PPM -> PNG).
Правка `net_session.py` (мой файл): перед стартом сценария удаляются старые логи узлов (иначе старый `boot.txt` удовлетворял условию `until` до старта узла).
Побочное открытие: **в аргументах Bash-инструмента двойная обратная косая (`\\n`) схлопывается в одинарную (`\n`)**: в heredoc с Python-кодом литерал `\\n` внутри строки C/Python превращался в реальный перевод строки. Все правки, где нужна обратная косая, делались инструментом Edit.

| Сценарий | Результат |
|---|---|
| `ps2srv-ps2cli` (два PCSX2, `net1`/`net2`): PS2-сервер MAP01 + PS2-клиент `-connect` | **работает**: клиент вошёл (`players=2`), `NETSYNC` до gametic 1785; `netsync_compare.py` srv/cli: 21 общий тик (1085..1785), **0 отличий** (`build/opt9-n/ps2ps2-compare.json`); 90.7 с. (В OPT8 сервер-эмулятор «падал на старте» — сбой запуска PCSX2, не движка.) |
| `ps2host-mock`: PS2-хост регистрируется на mock-мастере | `POST /MS/0/rooms/1/register` (peer 172.18.0.1, port 5029, title `PS2 mock test`, version 2.2.15) -> токен, затем `listserv` печатает `172.18.0.1 5029 PS2 mock test 2.2.15` (`build/opt9-n/run/ps2host-mock/mock.jsonl`) |
| `menu-browse` (меню пада: Multiplayer > Server browser > комната > строка сервера > ENTER-Join) | **PS2 -> ПК-сервер прямо из списка**: mock отдал комнаты (Standard/Casual/Custom) и сервер, зарегистрированный на mock ПК-сервером (`PC test server`, ping 28 мс, Co-op, GFZ1, Dedicated), экран информации о сервере, вход, уровень на экране PS2, `NETSYNC` идёт (кадры: `build/opt9-n/mb4.png`) |

Найдено и исправлено в mock (`tools/ps2/mock_masterserver.py`): `versions/<id>` отвечал `0 none` -> клиент показывал «доступно обновление» и не шёл дальше в список комнат; настоящий сервер отвечает `56 v2.2.15` (проверено GET). Теперь `--modversion` (56). Регистрация с тем же адресом и портом заменяет старую запись (как у настоящего). `--fixture DIR` (`tools/ps2/ms_fixture.py` снимает настоящие комнаты/серверы GET-запросом и **анонимизирует адреса** в 192.0.2.x/2001:db8::x) — формат реального списка (IPv4+IPv6, чанки) без обращения клиента к чужим серверам.

## 2. Мастер-сервер на PS2, добавления и наблюдения (этап 1, продолжение)

* **Настоящий мастер-сервер, только чтение (GET)** — сценарий `real-ms-read` (`build/opt9-n/run/real-ms-read/cli/boot.txt`): DNS в эмуляторе (`PS2 net: dns 1.1.1.1`, DHCP DEV9), `GET /MS/0/versions/18` (ответ `56 v2.2.15` = совпадает с MODVERSION), `GET /MS/0/rooms` — меню комнат показывает Standard / Casual / Custom Gametypes / All (`build/opt9-n/rm1.png`), `listserv` печатает весь настоящий список (IPv4 и IPv6 строки, разделы по комнатам, ответ chunked). Регистрация не выполнялась, ни один сервер из списка не опрашивался (комната не выбиралась; для проверки разбора списка — mock с `--fixture`, адреса 198.51.100.x/2001:db8::x).
* **Файлы с сервера по UDP** (`addons-udp`, `addons-http`, `full2.ELF`): аддон с ПК-сервера (`NSK.pk3`, 408 КиБ; скин+Lua+SOC; и `ZT.pk3`, 1.94 МиБ) скачан по игровому соединению (≈4 с на 408 КиБ), загружен (`Added file host:/.srb2/DOWNLOAD/NSK.pk3 (514 lumps)`, `Added skin 'ztest'`, Lua `NETLUA spawn 1`), затем `$$$.sav` (24.5 КиБ) принят, игра идёт, `netsync_compare` ПК-сервер/PS2-клиент: 42 общих тика, 0 отличий. (Первая попытка `ZS.pk3` из набора F не годится как сетевой аддон: его Lua в кадре 14 вызывает `quit` на сервере; поэтому сделан `NSK.pk3` = тот же скин без `quit`.)
* **HTTP-источник аддонов (PS2-137)**: libcurl на PS2 нет, поток запросов `CURLPrepareFile`/`CURLGetFile` был заглушкой (всегда «HTTP недоступен» — откат на UDP). Теперь `ps2_curl.c` содержит потоковый GET-автомат `PS2HttpGet_Open/Step/Close` (не блокирует: на каждый проход цикла подключения делает то, что позволяют сокеты; тело пишется в файл по мере прихода; Content-Length, chunked с расширениями и трейлером, чтение до закрытия, перенаправления 301/302/303/307/308 с относительным `Location`, отказ 4xx/5xx, обрыв, таймаут «нет данных 20 с»). Хост-тест `python tools/ps2/ps2_http_hosttest.py` (MSVC): **ALL PASS** — три режима мастер-сервера и 10 случаев GET (3 МиБ plain/chunked/redirect, noclen, empty, slowhead, trunc, 404, loop, stall; побайтное сравнение с эталоном). В `d_netfil.c` PS2-ветка `CURLPrepareFile/CURLGetFile/CURLAbortFile` с теми же переходами состояний, что поток libcurl (`FS_DOWNLOADING` → `FS_FOUND`/`FS_FALLBACK`, `http_failed`, MD5-проверка), шаг делает `client_connection.c` в состоянии `CL_DOWNLOADHTTPFILES`. Ошибка при первом запуске (видна в журнале PS2): `va()` с одним статическим буфером перезаписал адрес именем User-Agent -> «Protocol not supported»; исправлено локальными буферами; имя файла в пути кодируется процентами.
* **PS2↔PS2, 5250 тиков** (`soak-ps2srv-ps2cli`, `full2.ELF`): оба эмулятора, сервер и клиент ходят/прыгают по `-padscript`, `resynchattempts 0` + `blamecfail On`; 260.9 с; `netsync_compare`: 133 общих тика (gametic 980..5600, players=2), **0 отличий** (`build/opt9-n/soak-ps2ps2-compare.json`); ошибок/ресинхронизаций/таймаутов в журналах нет.

## 3. HTTP-источник аддонов в эмуляторе, режимы игры (`full4.ELF`, 11 117 628 Б; все прогоны — по `net_session.py`, логи в `build/opt9-n/run/<имя>/`)

| Сценарий | Результат |
|---|---|
| `addons-http` (ПК-сервер `-file NSK.pk3 ZT.pk3` + `+http_source http://HOSTIP:8091`, статический сервер `tools/ps2/http_static.py`) | PS2-клиент скачал оба файла **по HTTP** (журнал `http.jsonl`: `GET /NSK.pk3?md5=cf82...` 418 370 Б -> 200, `GET /ZT.pk3?md5=4c63...` 2 031 196 Б -> 200; журнал PS2: `Downloading addon "NSK.pk3" from http://172.18.0.1:8091` / `Finished download`), загрузил (`Added file host:/.srb2/DOWNLOAD/NSK.pk3 (514 lumps)`, `ZT.pk3 (35 lumps)`), вошёл, принял `$$$.sav`, Lua аддона отработал (`NETLUA spawn 1`); `netsync_compare` 41 тик, **0 отличий** |
| `addons-http-chunked` (источник отвечает `Transfer-Encoding: chunked`) | то же, 43 тика, 0 отличий |
| `addons-http-404` (источник всегда 404) | `ERROR: Failed to download addon "NSK.pk3" (The requested URL returned error: 404)` -> `Falling back to direct downloader.` -> скачано по игровому соединению, 43 тика, 0 отличий |

# OPT14, агент GIF: «при подключении к серверам включается запись GIF»

Сессия 2026-10-10, ветка `opt14-gif`, worktree `/home/user/wt/gif`, PCSX2 2.8.2. Стенд: `build/out` (PS2 ELF, без LTO для функциональных проверок),
`build/pc-net` (ПК-сборка с `-DNETSYNC_DIAG`: dedicated-сервер), `build/opt14-gif/{specs,run}`; сценарии — `tools/ps2/net_specs14_gif.py`
(`python3 tools/ps2/net_specs14_gif.py --base build/opt14-gif [имя ...]`, запуск `python3 tools/ps2/net_session.py build/opt14-gif/specs/ИМЯ.json`).

## 0. Происшествие со стендом (честно, до остального)

Сценарий `gif-mash1-sw` (мой, через `pcsrv(ms=True)` из `net_specs9.py`: ПК-сервер регистрируется на MOCK мастер-сервере контейнера) запустил ПК-сервер,
который **попытался зарегистрироваться на настоящем мастер-сервере**: в `srv/out.txt` строки `HMS: connecting 'https://ds.ms.srb2.org/MS/0/rooms/1/register'...`,
`ERROR: ... Could not resolve host: ds.ms.srb2.org` (аудит `net_session.py` пометил `real_master_server_contact`).
Запрос **не ушёл из контейнера**: имя не разрешилось (у процесса движка `net_session.py` вырезает переменные прокси, DNS наружу нет); ничего на `ds.ms.srb2.org`
не отправлено. Тем не менее это нарушение правила «регистрировать там запрещено» по намерению, и стенд надо чинить:

* причина — гонка в самом движке: `masterserver "<mock>"` из `config.cfg` вызывает `MasterServer_OnChange` -> `Set_api()` (`src/netcode/mserv.c`), а тот
  в многопоточной сборке ставит `hms_api` **из отдельной нити** (`change-masterserver`); следующая строка конфига `masterserver_room_id "1"` вызывает
  `RoomId_OnChange` -> `RegisterServer()` сразу же, и `HMS_connect` берёт ещё прежний (настоящий, по умолчанию) адрес. Результат зависит от нагрузки машины
  (раньше у NET/OPT12 это не сработало, сегодня нагрузка 4..11 — сработало);
* что сделано: сценарии этого агента **не** используют `pcsrv(ms=True)`: сервер читает из конфига только `masterserver "<mock>"`, а `masterserver_room_id 1` ему
  подаётся через stdin через 12 с (нить давно отработала); все остальные сценарии (`-connect`, OSK) идут с `masterserver "http://127.0.0.1:9/MS/0"` (мёртвый порт);
* `tools/ps2/net_specs9.py: pcsrv(ms=True)` (чужой файл) остаётся с этой гонкой — записано в раздел «правки в чужих файлах / замечания»: опасность реальна
  для любого, кто запустит `menu-browse`/`ps2host-menu` в среде с DNS наружу.

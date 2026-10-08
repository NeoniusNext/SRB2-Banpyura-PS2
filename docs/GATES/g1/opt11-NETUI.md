# OPT11, агент NETUI: экран подключения к сети (DHCP) на Hardware renderer и в стиле меню

Сессия 2026-10-08, ветка `worktree-agent-ac241994e31ac836c` (база `claude/lucid-mayer-1izlqe`, слита). Реестр: `PS2-330..349`.
Файлы стенда (не в git): `build/out0` (базовая сборка HEAD, `SRB2_PS2_LTO=0`), `build/runs/*` (прогоны), эмулятор `/opt/pcsx2/netui1` (копия slot0 с DEV9 Ethernet).

## 1. Диагноз (проверен запуском на базовой сборке `build/out0`)

* `src/ps2/ps2_net.c`: `Splash()` рисует только при `rendermode == render_soft` (заливка + строки «NETWORK» и фаза), при Hardware — только `CONS_Printf`.
  Снимки окна эмулятора во время подъёма сети (`tools/ps2/netui_run.py`, сценарий `-skipintro -netcmd "60:connect 192.0.2.2"`):
  * Software (`docs/GATES/g1/opt11-NETUI/before-sw-link.jpg`): чёрный экран, две строки шрифтом по умолчанию «NETWORK» / «WAITING FOR THE ETHERNET LINK...»; затем (`before-sw-connect.jpg`) обычный экран подключения к серверу (небо, логотип, красная полоса).
  * **Hardware** (`before-hw-link.jpg`): картинка **застыла** — последний кадр титульного экрана остаётся на мониторе весь подъём сети (≈ до 20–30 с), никакой индикации; затем экран подключения к серверу (`before-hw-connect.jpg`) рисуется нормально.
* Библиотека `libps2_drivers.a` (`ps2_eeip_driver.c`, дизассемблирована в этой сессии): `configure_eeip_network()` вызывает `Progress` только на границах фаз, ожидание — циклы `usleep(1000000)` до `timeout_seconds`
  (по умолчанию 10; движок ставит 15, `-nettimeout N`): ожидание линка (`NetManIoctl(0x3000)` == 1), затем ожидание DHCP (`libcglue_ps2ip_getconfig("sm0")`: `dhcp_status == 10` и адрес не 0). Максимум ≈ 2·15 с = 30 с без единого вызова `Progress`.
  Прервать цикл снаружи нельзя; перерисовка между вызовами `Progress` невозможна. => подъём сети переписан на опрос тех же публичных функций библиотеки с шагом ≈ 100 мс и перерисовкой каждый тик (раздел 3).

## 2. Стенд (проверено запуском)

* `/opt/pcsx2/netui1` = копия `/opt/pcsx2/slot0` + `[DEV9/Eth] EthEnable=true, EthApi=Sockets, EthDevice=eth0` и `[SPU2/Output] Backend=Null` (без неё PCSX2 2.6.3 выводит модальное окно «cubeb_stream_init() failed», закрывающее снимок). Общие слоты 0..3 не менялись.
  Гость получает от встроенного DHCP эмулятора `192.0.2.100/24`, шлюз `192.0.2.1`, DNS `8.8.4.4`; базовая сборка: «PS2 net: address 192.0.2.100 ... (stack heap 6288 B)».
* `tools/ps2/netui_run.py` (новый): один прогон PCSX2 в собственном Xvfb (`:70..:79`) с запретом на двойной запуск копии, правка ключей ini на время прогона (`--ini DEV9/Eth.EthEnable=false`), снимки окна эмулятора по появлению текста в `boot.txt` (`--grab 'TEXT@ЗАДЕРЖКА=имя'`) и периодические (`--periodic`).
  `tools/ps2/netui_pics.py` (новый): обрезка снимка окна до картинки консоли и сохранение в JPEG для `docs/GATES/g1/opt11-NETUI/`.

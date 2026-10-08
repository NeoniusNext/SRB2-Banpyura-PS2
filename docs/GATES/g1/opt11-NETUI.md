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

## 3. Экран сети (PS2-330, PS2-331) — проверено запуском

**Что сделано.** `src/ps2/ps2_net.c`: подъём сети переписан как цикл опроса тех же функций библиотеки (`ps2ipInit`, `libcglue_ps2ip_getconfig/_setconfig("sm0")`, `NetManIoctl(GET_LINK_STATUS)`;
порядок вызовов и тексты `PS2 net: ...` в логе прежние), шаг ожидания 100 мс; между шагами рисуется кадр и читается пад. `src/ps2/ps2_netui.c/.h` (новые): модальный экран.
* **Рисуется в обоих рендерерах.** Тем же приёмом, что `CL_ConnectToServer` (`F_TitleScreenDrawer` для `GS_WAITINGPLAYERS`, `V_DrawFadeScreen(0xFF00,16)`, `M_DrawTextBox`, бегущая 16-клеточная полоса `palstart+((animtime-i)&15)`), один кадр за тик через `I_UpdateNoVsync`,
  внутри `PS2HWFB_Display` (охранник нехватки памяти: в Hardware сбой переводит экран в Software, как в `D_Display`). Hardware сам открывает кадр при первом 2D-вызове (`frame_prepare`) и копирует предыдущий кадр: отдельного «старта кадра» не нужно.
* **Стиль меню:** фон = небо и логотип титульного экрана (как экран подключения к серверу), панель `M_DrawTextBox(36,19,29,15)`, заголовок «Network» цветом `MENUCOLOR` с линией как у `M_DrawLevelPlatterHeader`, строки фаз (✓ зелёная галочка + время шага / спиннер из 8 точек + имя жёлтым / тусклая точка + серое имя),
  шкала времени шага «n / 15 s», подсказка через 3 с («No link yet: is the Ethernet cable plugged in?» / «No answer yet: is the router (DHCP server) on?»), внизу «⭘ Cancel» и бегущая зелёная полоса.
* **Готово:** панель «Network ready» (зелёный заголовок): адрес, маска, шлюз, DNS, «Obtained by DHCP / Static address», держится 2.5 с или до нажатия Cross/Enter (`-netuiready MS`).
* **Ошибка:** окно в стиле `M_StartMessage` («The network is not available», причина красным: «No network adapter» / «No Ethernet link» / «No DHCP answer» / «Configuration error», подсказка: кабель, `-ip -netmask -gateway -dns` (README), «⨯ Try again  ⭘ Back»); закрывается само через 45 с (`-netuifail S`), чтобы
  неуправляемый запуск (тесты, `-server`) не висел. «Try again» повторяет только ожидание (модули и стек поднимаются один раз); после «Back»/отмены повторный вызов в течение 10 с отклоняется молча (вызовы без участия игрока: регистрация на мастере после неудачного старта сервера).
  Старое сообщение `connect` («The network is not available...», `commands.c`) показывается только если окно не показывалось (`PS2Net_Reported()`).
* **Отмена:** Circle / Escape во время ожидания линка и DHCP (проверено: опрос — отмена мгновенная); в шаге загрузки модулей (`init_eeip_driver`, блокирующий вызов в 3 IRX, ≈ 1 с) отмена сработает после него. Библиотечный цикл `usleep(1 с)` прервать нельзя, поэтому его и заменили.
* **Без картинки** (молчаливый режим, ожидание то же): dedicated-сервер, `rendermode == render_none`, вызов изнутри кадра (`Z_GuardArmed`) или вызов Lua (`PS2Lua_InCall`: HTTP из Lua-хука).
* Тестовые параметры (только отладка): `-netslow MS` (каждый шаг держится не меньше MS на экране), `-netuiready MS`, `-netuifail S`, `-vidshot nN` (N-й кадр экрана сети, `i_video.c`), `-netdebug` (строки `NETUI step ...`).
* Язык: шрифты SRB2 не содержат кириллицы (проверено по `FONTSTART..FONTEND` hu_stuff.h: ASCII), поэтому подписи английские через `M_GetText` (как остальные меню); русского «Сеть» нарисовать нечем.
* Меню Multiplayer в Hardware: окна «Searching for servers...» (`M_Refresh`), «Fetching room info...» (`M_RoomMenu`), «Connecting to server...» (`M_ConnectIP`) рисовались, но не показывались (`if (rendermode == render_soft) I_FinishUpdate()`): меню стояло замёрзшим. PS2-332: `M_PRESENT_WAITBOX()` показывает кадр на PS2 в обоих рендерерах.

**Снимки** (`docs/GATES/g1/opt11-NETUI/`, окно PCSX2; software GS эмулятора, 4:3): до — `before-sw-link.jpg` (чёрный экран с двумя строками), `before-hw-link.jpg` (застывший титул); после — `net-hw-progress.jpg`, `net-hw-ready.jpg`, `net-sw-ready.jpg`, `net-sw-dhcp.jpg` (окно ошибки), `net-sw-noadapter.jpg`.

| Сценарий | Рендерер | Результат (запуск, `tools/ps2/netui_run.py`) |
|---|---|---|
| успех DHCP, `-netslow` | Software, Hardware | экран по фазам; готово: 192.0.2.100/24, шлюз 192.0.2.1, DNS 8.8.4.4; DHCP-фаза 7.2–7.5 с (поведение lwIP в PCSX2, как и на базовой сборке) |
| «No DHCP answer» (`-nettimeout 3`/`6`, `InterceptDHCP=false`) | Software | окно ошибки, «Back» / закрытие по времени; `NETUI failed: window closed by itself after 20 s` |
| «Try again» (клавиша Enter через `-vidkeys`) | Software | после повтора: «DHCP lease received» → «Network ready» (лизинг пришёл, пока открыто окно; повторно ждутся только линк и DHCP, `Starting the IP stack` не повторяется) |
| «No network adapter» (`EthEnable=false`) | Software | `init_eeip_driver` = -2, окно без «Try again» (модули в неизвестном состоянии, повтор в сессии запрещён) |
| «No Ethernet link» | — | **в PCSX2 смоделировать нельзя** (при включённом Ethernet линк всегда есть; при выключенном модули не стартуют): путь `NB_LINK` — тот же цикл, что DHCP, проверен только чтением кода |

## 4. Иконки кнопок геймпада (PS2-333..337) — проверено запуском

Источник: `assets/ps2ui/pad_buttons_sheet.jpg` (автор по подписи на листе: **Max_the_Viking**; README рядом). **PS4-полоски «Share/Options» не используются** (убраны по просьбе пользователя: не вырезаются, не конвертируются, нет токенов и имён в движке).
* `tools/ps2/ui_icons.py` (резка листа): связные компоненты не-белого (порог по минимальному каналу), подписи листа («Classic PS Buttons», «PS4 Buttons», «By: Max_the_Viking») и «острова» внутри букв отброшены, слипшиеся Square и R2 разрезаны по самой редкой строке (y=301); 41 иконка по сетке листа;
  из каждой делается **эталон** — уменьшение до целевого размера по классам цвета (контур/корпус/белый/цветной символ: площадь каждого класса и голосование с весами, дальше ближайший цвет PLAYPAL, замкнутый чёрный контур 1 px).
* `tools/ps2/ui_icons_pixel.py` + `ui_icons_build.py` (сборка): **итоговые иконки нарисованы пиксель-артом** на нечётных сетках (13×13 круги/стики/D-pad, 15×9 L1/R1, 15×13 L2/R2, 15×7 Select, 11×11 Start, 9×5 стрелки), центр на пикселе, формы строятся симметричными правилами,
  символы — явные маски (кольцо ○ 9×9, ✕ 7×7, □ 7×7, △ 9×7, шрифт 3×5 для L/R/1/2/3). Причина: сплошное уменьшение 1200 → 13 px даёт мягкие, «кривые» иконки (какой край чёрный, решает фаза уменьшения; первая версия). Цвета PLAYPAL выбраны вручную (чёрный 31, корпус 23, белый 0, зелёный △ 113, розовый □ 180, красный ○ 35, голубой ✕ 148).
  **Проверка программно (сборка падает, если нарушена):** зеркальное сравнение силуэта и картинки по обеим осям (`mirror_h`/`mirror_v`, число отличающихся пикселей): все силуэты круглых/плюсовых/прямоугольных иконок симметричны по обеим осям (0 px), △ и стрелки — по своей оси, Start — по горизонтальной оси,
  картинки ○ □ ✕ Select и D-pad ←→/↑↓/все — 0 px; буквы L/R/1/2/3 и стрелки направлений на стиках несимметричны по замыслу (проверяется только силуэт). Сравнение с эталоном по пересечению силуэтов (IoU): 0.90–1.00 у кругов, кнопок и D-pad, 0.88 у стрелок. Таблица — вывод `python3 tools/ps2/ui_icons_build.py`.
* Контрольные листы: `docs/GATES/g1/opt11-NETUI/icons-contact.png` (все 41 иконка ×6) и `icons-real-size.png` (×1).
* `src/ps2/ps2_uiicons_data.inc` (генерируется; 41 патч Doom в палитре игры, 9.8 КБ, `aligned(4)`) встроен в ELF: **не в пак** (лишний пак попал бы в список файлов сетевой игры и сломал бы совместимость с ПК), ничего не грузится и не теряется. `src/ps2/ps2_uiicons.c/.h`: `PS2UI_Patch()` создаёт патч при первом показе
  (`Patch_TryCreateFromDoomPatch`, тег `PU_STATIC`; нет места — иконка не рисуется, текст вокруг остаётся), `PS2UI_KeyToken()` (кнопка пада → токен: JOY1..12 = Cross, Circle, Square, Triangle, L1, R1, Select, Start, L3, R3, L2, R2; hat 0 = ↑↓←→; оба пада; раскладка прочитана из `ps2_padmap.c`).
* **Иконки в тексте (PS2-334):** управляющие символы 0x01..0x15 (`PS2I_CROSS "\x01"` ...: Cross, Circle, Square, Triangle, L1, R1, L2, R2, Start, Select, D-pad ↑ ↓ ← →, L3, R3, D-pad ↑↓, D-pad ←→; остальные иконки через `PS2UI_Patch(PS2UI_*)`) понимают `V_DrawString`/`V_Draw*AlignedString` (рисует иконку как букву, по центру строки шрифта),
  `V_StringWidth` и `V_WordWrap` (`v_video.c`, только `#ifdef PS2`, проверка `(UINT8)c < 0x16` на символ). Строки без токенов работают как раньше.
* Где используются: **экран сети** (Cancel / Continue / Try again / Back), **подсказки экранов подключения к серверу** (`client_connection.c`: Abort/Cancel/Back/Scroll list/Download/Join/Players-Addons, макросы `HINT_*`, остальным платформам тексты прежние), **окна-сообщения** (`M_StartMessage`: «Press ESC» → «Press ○», «Press ENTER ... or ESC» → «Press ✕ ... ○», «(Press a key)» → «(Press any button)», `PS2UI_Message`),
  **экранная клавиатура** (`ps2_osk.c`: «✕ type ▢ shift △ del ▶ ok ○ close»), **Setup Controls** (`M_ControlKeyName`: кнопка пада в списке привязок — иконка; заголовок «✕ Change ▢ Clear» вместо «Press Enter to change, Backspace to clear»), **«Press ESC to exit»** в Record/NiGHTS Attack.
* Тестовая карта: консоль `ps2_icons 1` (все иконки на тёмном и светлом фоне, подсказки обычным и тонким шрифтом, окно сообщения), `ps2_icons 2` (экранная клавиатура без поля ввода): `icons-card-sw.jpg`, `icons-card-hw.jpg`, `icons-osk-sw.jpg`, `icons-osk-hw.jpg` — **Software и Hardware дают одну картинку**, края и прозрачность чистые (патчи — обычные `patch_t`, HW делает текстуру штатно, nearest).

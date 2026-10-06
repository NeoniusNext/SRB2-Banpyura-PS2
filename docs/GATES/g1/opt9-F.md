# OPT9, агент F (контент: аддоны, лимиты, Lua, UDMF): рабочий журнал

Продолжение `opt6-F.md`, `opt8-F.md`. Сессия 2026-10-06 ночь. Каталоги `build/opt9-f/{out-full,out-hw,ref-full,ref-base,run,addons}`.
Инструменты: `tools/ps2/make_addons.py`, `ftest_run.py`, `ftest_check.py`, `ftest_golden.py`, `src/ps2/ps2_ftest.c` (хуки `-ftest-*`, строки `FT_`/`FTLUA`).
Эмулятор — только через `tools/ps2/ftest_run.py` / `ftest_golden.py` (→ `run_pcsx2.py`, свободный слот из 4).

## 0. Состояние на старте OPT9 (проверено)

* Дерево OPT8 содержит почти завершённый механизм `PS2_DYNLIMITS` (PS2-104): с `PS2_LIMITS` константы `NUMSTATES/NUMMOBJTYPES/NUMSFX/...` = значения ПК, а таблицы живут
  малыми (размеры старого профиля) и один раз вырастают до размера ПК в `PS2Limits_Grow()` (`src/ps2/ps2_limits.c`) по требованию (freeslot > малого диапазона, `S_AddSoundFx`, кадр спрайта >= 64).
  Живые размеры — макросы `LIMIT_*` (`info.h`, `sounds.h`, `doomdef.h`, `r_defs.h`). `skin_t` — один блок зоны с таблицами анимации после структуры (`R_AllocSkin`), `spriteinfo_t.pivot` — указатель, выделяется при первом использовании.
* OPT8 успел проверить запуском: `e-lim-start`, `e-lim-rt` (рост таблиц при старте и во время уровня, 300 состояний/40 типов/40 цветов/300 звуков/20 sprite2: строки `FTLUA` до `done`); `d-skin`, `d-skin-rt`, `d-autoload`, `d-mc` и `g-full-DEMO_001` оборвались (эмулятор завершился при старте, не дошли до теста).
  Отчёт OPT8 об этом ничего не говорит: всё перепроверяется ниже.
* На диске есть настоящие аддоны SRB2: `D:\games\SRB2\addons\` (скины `CL_Sage-v1.1.pk3`, `CL_MetalReplay_v1.01.pk3`, `VCL_XMomentum-v1.3.4.pk3`; Lua `L_foxBot-v1.7.pk3`, `L_RailAI_V1.05.pk3`, `L_LithCore_V0.13A.pk3`, `L_BuddyEx-v1.2.wad`,
  `L_AdminToolsPlus_v1.2.lua`, `L_ZyphyrCommunityColors-v1.lua`, `Items_Units.LUA`; карты `SL_DOOMII-v0.99.pk3`, `S_MysticRealm-v5.2.pk3`, `MAP19.wad`; большие моды ZombieEscape2/Infinity 36–86 МБ).

## 1. Инструменты этапа (новые)

* `tools/ps2/pc_run.py` — запуск ПК-эталона **из исходников этого дерева** (`build/opt7-s/pc/srb2-s7pc.exe`) с теми же аддонами, сбор строк `FTLUA`. `srb2-assets/srb2win.exe` — другая версия (флаги скинов `SF_*` сдвинуты на один бит, хэши mobjinfo/states другие),
  для сравнения состояния игры не годится; всё сравнение ниже — с `srb2-s7pc.exe`.
* `tools/ps2/addon_compare.py` — один вызов: ПК + PS2 (PCSX2) с одним и тем же списком аддонов, сверка итоговых строк `FTLUA` (аддон `ZSUM.pk3` из `make_addons.py sum` печатает через Lua API: скины и их параметры/число кадров спрайтов,
  число и хэш использованных `mobjinfo`/`states`/`skincolors`/`sfxinfo`, карту, тип игры) и сообщений WARNING/ERROR, которые вызвали аддоны (минус то, что печатает голая игра).
* `ftest_run.py`: `--retries` (повтор, если эмулятор не стартовал), `FTLUA` в показываемых строках. `ps2_ftest.c`: `-ftest-sprites NAME` (`FT_SPR`: число кадров определения спрайта).

## 2. Проверка запуском: база и настоящие аддоны (сборка `build/opt9-f/out-full`, программная, `SRB2_PS2_NO=` пусто, 10 554 532 Б)

| Аддон (D:\games\SRB2\addons) | Что внутри | PS2 = ПК (FTLUA) | Замечания |
|---|---|---|---|
| (без аддона, только `ZSUM`) | — | 13/13 строк совпали | ваниль |
| `CL_Sage-v1.1.pk3` | скин + Lua + SOC + музыка + LongSprites + TRNSLATE | 14/14 | |
| `CL_MetalReplay_v1.01.pk3` | скин + Lua + карта | 14/14, предупреждения те же | |
| `VCL_XMomentum-v1.3.4.pk3` | скин + Lua + SOC + 26 PNG + 5 OGG | 15/15 | |
| `L_foxBot-v1.7.pk3` | Lua (бот, 147 КБ) | 13/13 | |
| `L_RailAI_V1.05.pk3` | Lua (13 файлов) | 13/13 | |
| `L_AdminToolsPlus_v1.2.lua`, `L_ZyphyrCommunityColors-v1.lua` | сырой .lua (аддон-файл без контейнера) | 13/13 | |
| `S_MysticRealm-v5.2.pk3` | SOC + 36 карт-wad + спрайты | 13/13 | |
| `L_LithCore_V0.13A.pk3` | 207 Lua + 556 PNG | 13/13 | **ошибка Lua только на PS2**: `LC_ImportSkincolors.lua:282 '<name>' expected` — имена переменных с байтами >= 0x80 (`local \xd1\x811`), на ПК лексер принимает их по кодовой странице системы (русская Windows), у newlib — C-локаль. Исправлено (PS2-105) |
| `L_BuddyEx-v1.2.wad`, `MAP19.wad` | автономный PWAD | **не загружался**: `Invalid WAD header` | `WPack_Detect` оставлял указатель файла на смещении 4, `ResGetLumpsWad` читал заголовок оттуда. Исправлено (PS2-103: возврат на начало) |
| `SL_DOOMII-v0.99.pk3` | Doom II как мод: 55 Lua, 98 новых состояний/типов, скин JohnDoom | **I_Error `Z_CheckHeap 420 ... doesn't have a proper user`** при старте уровня | **найдено ZDEBUG-сборкой** (`out-zd`, владелец повреждённого блока `lua_hooklib.c:171`): блоки списков хуков `map->ids` принадлежат указателям ВНУТРИ таблицы `mobjHookIds`; `PS2Limits_Grow` → `LUA_GrowMobjHooks` переносил таблицу и освобождал старую, владельцы остались в освобождённой памяти. Исправлено (`Z_SetUser` для каждого `ids`, PS2-104) |

Побочные находки, исправлены в этом же этапе (тесты — ниже):
* `sprites[]`: `PS2Limits_Grow` освобождал и пересоздавал таблицу определений спрайтов, а `R_AddSingleSpriteDef` (вызывает рост при кадре спрайта >= 64) держит указатель на её элемент — обращение к освобождённой памяти. Теперь `R_InitSprites` выделяет таблицу сразу на `NUMSPRITES` элементов (12 КБ), рост только поднимает `numsprites`.
* Userdata Lua на элементы таблиц `states`/`mobjinfo`/`skincolors`/`S_sfx`, созданные скриптом ДО роста таблиц, указывали в старую копию: `LUA_RemapUserdata` (lua_script.c) переносит их на новые элементы.

## 3. Golden: полная сборка против базовой (4 демо, `--ps2ref`, программный рендер, PCSX2)

Сборки: `build/opt9-f/ref-full` (`SRB2_PS2_NO=` пусто, 10 632 164 Б) и `build/opt9-f/ref-base` (`SRB2_PS2_NO=lua,udmf,addons,limits`, старый профиль, 9 827 876 Б). `ftest_golden.py` против `golden/phase0-v2/run1`:

| Демо | ref-full vs golden | ref-base vs golden | ref-full vs ref-base |
|---|---|---|---|
| DEMO_001 | 3 файла: `frames.csv`, `frame-000560.idx` (1 байт, индекс 34054), `frame-000665.idx` (1 байт, 48287) | то же | **0 различий (32 файла)** |
| DEMO_002 | 2: `frames.csv`, `frame-000210.idx` (1 байт, 47784) | то же | **0 (32)** |
| DEMO_003 | 6: `tics.csv` (хэш состояния, последний столбец, с тика 45; позиции/здоровье/RNG те же), 4 кадра по 1–32 байта, `frames.csv` | то же | **0 (32)** |
| DEMO_004 | 5: `frames.csv`, кадры 35/70/105/140 (86/1/1/6 байт; зона плашки уровня), tics совпадают | то же | **0 (32)** |

Вывод: все возвращённые системы F (Lua, UDMF, аддоны, лимиты) в ванильном режиме **побитно** (тики и кадры всех четырёх демо) не меняют результат относительно старого профиля. Отличия от PC-golden (1–2 пикселя PS2-81 в DEMO_001 известны и записаны;
DEMO_002 1 пиксель, DEMO_003 хэш состояния с тика 45 и пиксели, DEMO_004 пиксели плашки) присутствуют уже в базовой сборке на текущем дереве: их источник — правки не-F кода (software-рендер/PS2-9x и т.д.), не возврат контента.
Требует внимания координатора: DEMO_003 `tics.csv` меняется (изменение состояния игры, не картинки).

## 4. Память-карта и аддон-хранилища (PS2-103)

`-ftest-mc ZF.pk3 -ftest-mcformat` (`build/opt9-f/run/d-mc`): `mcman.irx`+`mcserv.irx` из `<data>/modules` поднимаются по первому обращению к `mc0:`; пустая карта PCSX2 (файл из 0xFF, без формата) форматируется через libmc (`mcFormat`, только в тест-хуке: настоящая приставка получает отформатированную карту от BIOS);
файл 4072 Б записан в `mc0:/SRB2/ZF.pk3`, прочитан обратно (CRC `0d816967` = исходник, SAME), каталог перечислен (`.`, `..`, `ZF.pk3`). `tools/ps2/build.py`: `-lmc` при включённых аддонах.

## 6. Строки реестра отличий (PS2-100..119), состояние OPT9

| ID | Область | Отличие и проверка |
|---|---|---|
| PS2-100 | `PS2_ZIPPNG`: `w_wad.c`, `r_picformats.[ch]`, `r_textures.c`, `build.py` (`-DHAVE_ZLIB -DHAVE_PNG`, `-lpng16 -lz`) | pk3/ZIP и PNG возвращены рядом с cooked-паками (паки остаются быстрым путём для базовых данных). OPT6/OPT8: `ZT.pk3`, 34 лампы = Python zipfile (размер+CRC32), 6 PNG; OPT9: настоящие pk3 `CL_Sage`, `VCL_XMomentum` (PNG) = ПК. |
| PS2-101 | `PS2_LUA`: `blua/*`, `lua_*.c`, `lua_*.h`, `deh_lua.c`, `deh_soc.c:get_number` | настоящая Lua-VM; `soc_numbers` остаётся быстрым путём, неизвестное выражение — `LUA_EvalMath`. OPT8: 20/20 строк теста; OPT9: `L_foxBot`, `L_RailAI`, `L_LithCore`, `L_AdminToolsPlus`, `L_ZyphyrCommunityColors`, `SL_DOOMII` дают ту же картину, что ПК. |
| PS2-102 | `PS2_UDMF`: `p_setup.c` | UDMF-карты (TEXTMAP, ZNODES) читаются; `-writetextmap` не включён. OPT8: 3 карты ZombieEscape2 = независимый Python-парсер. |
| PS2-103 | `PS2_ADDONS`: `d_main.c` (автозагрузка), `m_menu.c`/`filesrch.c` (источники MC0/MC1/USB в меню Add-ons), `w_wad.c`+`w_pack.c` (`WPack_Detect` возвращает указатель на начало: автономный `.wad` не открывался), `src/ps2/ps2_addons.[ch]` (драйверы mc0:/mass: по первому обращению, из `<data>/modules/*.irx`), `build.py` (`-lmc`) | меню Add-ons, `addfile`, `-file`, автозагрузка `<data>/autoload`, `<home>/autoload`, `-autoload DIR`. Проверено в PCSX2: `mc0:` запись/чтение/каталог (формат пустой карты — только в тесте), автозагрузка, `-file`/`addfile` на уровне. |
| PS2-104 | `PS2_LIMITS`: `doomtype.h`, `doomdef.h`, `info.[ch]`, `sounds.[ch]`, `r_defs.h`, `r_skins.[ch]`, `r_picformats.[ch]`, `deh_*`, `lua_*`, `src/ps2/ps2_limits.c` | константы `NUMSTATES/NUMMOBJTYPES/NUMSFX/MAXFRAMENUM/...` = значения ПК, таблицы стартуют размером старого профиля и растут один раз (`PS2Limits_Grow`: состояния/типы/спрайты/цвета/sprite2; `PS2Limits_GrowSounds`: звуки, при первом звуке скина или > 256 свободных звуков) до размера ПК. `skin_t` — один блок (66 КБ вместо 4,24 МБ), `spriteinfo_t.pivot` выделяется при первом использовании, `sprtemp` 64 → 256 кадров, `gtdesc_t.notes` — указатель. Все номера, которые видят аддон/сейв/демо — номера ПК. Проверка: `ZF.pk3` (300 состояний, 40 типов, 300 звуков, 40 цветов, 20 sprite2, G_AddGametype, длинный спрайт с кадром 130), `SL_DOOMII` (98 типов/состояний, 21 цвет, звуки). |
| PS2-105 | `src/blua/llex.c` (`PS2`) | идентификаторы Lua могут содержать байты >= 0x80 (на ПК это решает кодовая страница системы; `L_LithCore` пишет `local \xd1\x811`). Принимается надмножество программ. |
| PS2-106 | `lua_script.c:LUA_RemapUserdata`, `lua_hooklib.c:LUA_GrowMobjHooks`, `r_things.c:R_InitSprites` | при росте таблиц userdata Lua на элементы `states`/`mobjinfo`/`skincolors`/`S_sfx` переносятся на новые элементы; владельцы списков хуков (`Z_SetUser`) идут за таблицей; таблица `sprites[]` сразу полного размера (рост не освобождает её). Найдено `SL_DOOMII` (`Z_CheckHeap ... doesn't have a proper user`). |
| PS2-110 | `src/ps2/ps2_ftest.c`, `tools/ps2/{make_addons,ftest_*,pc_run,addon_compare}.py` | тестовые хуки `-ftest-*` (только диагностика, без параметра ничего не делают). |

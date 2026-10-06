# OPT8, агент F (возврат контента: Lua, UDMF, аддоны, лимиты): рабочий журнал

Продолжение `opt6-F.md` (этап a: zlib/libpng/pk3, PS2-100). Сессия 2026-10-06 вечер. Каталоги `build/opt8-f/{out,out-full,run,addons}`.
Инструменты: `tools/ps2/make_addons.py` (генератор тестовых аддонов), `tools/ps2/ftest_run.py` (запуск в PCSX2 через `run_pcsx2.py` с общей блокировкой), `tools/ps2/ftest_check.py` (сверка), `src/ps2/ps2_ftest.c` (хуки `-ftest-*`, строки `FT_`).
Правки OPT7 (Lua/UDMF/p_setup.c/m_menu.c/filesrch.c/w_wad.c) уже в коммите `e4a5c4b` (рабочее дерево на старте OPT8 = HEAD).

## 0. Что реально работает на старте OPT8 (проверено запуском в PCSX2, релиз, 32 МБ, ELF `build/opt8-f/out/SRB2.ELF`, сборка `SRB2_PS2_NO=limits`)

| Этап | Проверка | Результат |
|---|---|---|
| (a) ZIP/pk3 + PNG | `ZT.pk3` (34 записи, 6 PNG), `ftest_check.py zip` | 34 лампы: размер и CRC32 = Python zipfile, 0 проблем (`build/opt8-f/run/a-zip`) |
| (b) Lua-VM | `ZL.pk3` (Lua + SOC с выражениями вне таблицы `soc_numbers`), `-warp MAP01`, `ftest_check.py lua` | 20 из 20 ожидаемых строк `FTLUA` (математика, строки, таблицы, pcall, корутины, freeslot, хуки ThinkFrame/MobjThinker, спавн freeslot-объекта, SOC через `LUA_EvalMath`, `COM_BufInsertText`), ожидание получено из `t_fsin/t_facon` и PC-прогона (`build/opt7-f/pc/zl`) (`build/opt8-f/run/b-lua`) |
| (c) UDMF | `UD.pk3`: MAP17/16/99 пользовательского мода ZombieEscape2 (namespace srb2, ZNODES), `ftest_check.py udmf` | 3 карты загружены как UDMF; счётчики и CRC вершин/секторов/линий/сторон/вещей = независимый Python-парсер `udmf_ref.py`; CRC имён текстур MAP17 совпадает; 0 проблем (`build/opt8-f/run/c-ud{17,16,99}`) |
| (e) лимиты | `grep HAS_FULLLIMITS src` | флаг определён, но НЕ используется нигде: `skin_t`/слоты/кадры остаются урезанными (PS2-09/11), этап не начат |
| (d) аддоны | меню Add-ons и `addfile` скомпилированы (`HAS_ADDONS`), раньше не проверялись | см. разделы ниже |

Находки при проверке:
* `tools/ps2/build_host_profile.ps1` (хост-сборка профиля) не собирается: `src/ps2/net_stub.c` (файл S) использует `__attribute__((weak))`, MSVC его не принимает (`build/opt7-f/host/build-20261006-111043.log`). Хост-профиль для моих тестов недоступен, все проверки идут в PCSX2.
* первая попытка запуска дважды упала в PCSX2 на `Failed to create swap chain` (другой агент держал эмулятор/видеоконтекст), третья прошла: это среда, не движок.

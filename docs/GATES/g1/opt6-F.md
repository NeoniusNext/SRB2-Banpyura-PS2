# OPT6, агент F (возврат контента: ZIP/PNG, Lua, UDMF, аддоны, лимиты): рабочий журнал

Сессия 2026-10-05 (вечер). Каталоги `build/opt6-f/{out,run,pak,addons,base}`. Эмулятор только через `tools/ps2/opt_run.py` / `tools/ps2/ftest_run.py` (обе идут через `run_pcsx2.py`, общая блокировка).
Нумерация реестра: PS2-100..119. Тесты содержимого: `tools/ps2/make_addons.py` (генератор тестовых аддонов), `tools/ps2/ftest_run.py` (запуск в PCSX2), `tools/ps2/ftest_check.py` (сверка), `src/ps2/ps2_ftest.c` (`-ftest-*` хуки, строки `FT_`).

## 0. Переключатели и база

* `src/doomtype.h`: под `PS2_PROFILE` макросы `PS2_ZIPPNG`, `PS2_LUA`, `PS2_UDMF`, `PS2_ADDONS`, `PS2_LIMITS` (по умолчанию включены, выключаются `-DPS2_NO_<ИМЯ>`), производные `PS2_FULLLOADER`, `HAS_ADDONS/HAS_LUA/HAS_UDMF/HAS_ZIPPNG/HAS_FULLLIMITS`
  (вне профиля `HAS_*` всегда определены: PC-код не меняется).
* `tools/ps2/build.py`: `SRB2_PS2_NO=lua,udmf,...` (A/B; список по умолчанию сокращается по мере готовности этапов), `-DHAVE_ZLIB -DHAVE_PNG` и `-lpng16 -lz` при включённом `zippng`.
* База (дерево на старте, `build/opt6-f/base/SRB2.ELF`, релиз, ванильный режим, `-skipintro -warp MAP01 -zquit 60`, PCSX2 32 МБ):
  `ZSTAT arena=24698880 used=16242128 peak=16258272 free=8456752 largest=8338800 cycles=551465422` (прогон `build/opt6-f/run/base-map01`), `size`: text 3275952 data 529692 bss 1153804.

## 1. (a) zlib + libpng + ZIP/pk3 (PS2-100)

* `w_wad.c`: оригинальный загрузчик (wad/pk3/soc/lua/папка) снова собирается рядом с путём cooked-пака (`PS2_FULLLOADER`): пак опознаётся `WPack_Detect` внутри `W_InitFile`, остальное идёт оригинальным путём
  (ZIP central directory, deflate через zlib); чтение лампы: `WPack_ReadLump` только если у файла есть `pool`. `w_wad.h`: `HAVE_ZLIB` больше не снимается при `PS2_ZIPPNG`.
* `r_picformats.[ch]`: настоящие `Picture_IsLumpPNG/PNGConvert/PNGDimensions` (libpng) принимают и cooked-картинки (`Picture_IsLumpCooked`); `PICTURE_PNG_USELOOKUP` (128 КБ мемо в PU_CACHE, лениво).
  `r_textures.c`: быстрый путь потоковой сборки текстуры из cooked-патча обходит настоящий PNG (он идёт через `W_CachePatchNumPwad`).
* Проверка (`build/opt6-f/run/a-zip1`): аддон `ZT.pk3` (deflate/stored, поля extra, пустые лампы, папки, 12 размеров до 1.5 МБ, 6 PNG: RGBA, палитра+tRNS, серый, grAb, 128x128 RGB, 16 бит)
  загружен `-file ZT.pk3`; `ftest_check.py zip`: **34 записи, размер и CRC32 каждой лампы = ZIP (Python)**, 0 проблем. Негативный контроль (`ZT_bad.pk3`, испорчен 1 байт stored-записи): `DIFF Misc/ZTSTORED`, FAILED.
  Все 6 PNG конвертируются в патчи (`FT_PATCH`: размеры 32x32, 16x48, 24x24, 20x40 с offset 8/30, 128x128, 16x16).
* Цена: text +256 КБ (zlib+libpng+ZIP-код; арена 24698880 → 24444928), память зоны в ванильном режиме не изменилась (used 16242032 против 16242128 байт, те же такты).

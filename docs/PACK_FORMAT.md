# SRP2 v1 — временный пак для проверки каркаса

Это существующие леса фазы 1, не завершённый cooked-формат фазы 2.
Конверсия PCM/карт/таблиц и собственный I/O backend пока отсутствуют; PNG-лампы
конвертируются кукером (см. «Cooked picture» ниже, PS2-20). Строгий G1 остаётся
красным до закрытия остальных строк; новые этапы не открыты.

## Layout

Все числа little-endian UINT32, никаких указателей на диске.

64-байтный заголовок: `magic="SRP2"`, `version=1`, `headersize=64`,
`flags`, `numlumps`, `tableoffset`, `pooloffset`, `poolsize`, `dataoffset`,
`filesize`, `blocksize=65536`, затем 20 резервных нулевых байт.
Единственный флаг (бит 0): исходный архив содержит немузикальные данные,
как `W_VerifyNMUSlumps`; он не является проверкой доверия/контрольной суммой.

Начала индекса, пула строк и payload-области кратны 2048; конец файла тоже.
Запись индекса — 24 байта: `{position, disksize, size, fullname_off,
longname_off, codec}`. Пустые записи имеют нулевые position/disksize/size и raw codec.
`fullname`/`longname` — NUL-terminated ASCII в общем пуле строк; короткое
имя и case-insensitive hash выводятся по исходным правилам `ResGetLumpsZip`.

Порядок соответствует central directory каждого исходного pk3, включая
directory markers. Четыре архива загружаются в прежнем порядке; `wadnum`,
`lumpnum` и namespace/folder API остаются прежними. Пак представлен ядру
как RET_PK3, но ZIP-контейнер для pack-пути не используется.

Начало каждого непустого payload: кратно 2048 при size ≥65536, иначе 64.
Это отличается от целевого «каждая лампа кратна сектору»; reader читает
сектора через выровненный bounce-buffer и копирует нужный диапазон.

## Кодеки

* 0 — raw, `disksize == size`.
* 1 — LZ4 block без size prefix. При size ≤65536 это один блок.
* При size >65536: UINT32 длины каждого блока (ceil(size/65536)), затем
  тела подряд. Старший бит длины означает raw block; остальные биты —
  сохранённая длина. Последний decoded block может быть короче 65536.

`cook.py`: LZ4HC compression=12; tiny (<256) и OggS остаются raw;
сжатый вариант берётся только при ratio ≤0.90. OGG не пересжимается.
Сырьё пока сохраняется для всех записей, включая MP/UDMF/MIDI (движок профиля
UDMF-карту не грузит: `I_Error`).

## Cooked picture (PNG-лампы, PS2-20)

В профиле PS2 нет libpng/zlib. 15 PNG-ламп `srb2.pk3` кукер заменяет лампой
того же имени и порядка: 8 байт маркера `89 "SRPIC" 0D 0A` (на месте подписи PNG),
затем Doom-патч (`softwarepatch_t`, little endian, «высокие» патчи допустимы) с
пикселями, прозрачностью и смещениями, которые строит ОРИГИНАЛЬНЫЙ
`Picture_PNGConvert(PICFMT_PATCH)` (ближайший цвет палитры с memo по RGB565,
alpha 0 = нет поста, `grAb` = left/top offset). Конверсию делает хостовая сборка
`src/r_picformats.c` с libpng (`tools/ps2/strip_pics.py`, `strip_pics_host.c`,
порядок = порядок pk3: memo зависит от порядка, измерено: для этих 15 нет).
Рядом с паком пишется `<PACK>.pics.json` (индекс, хэши PNG и cooked-лампы, размеры):
его читают `verify_pack.py` (хэш PNG из pk3, хэш cooked-лампы, сверка патча с независимой
Python-моделью декодера, «PNG в cooked-паке не осталось») и `test_pack_reader.py`.
В движке (`PS2_PROFILE`) `Picture_IsLumpPNG`/`Picture_PNGConvert`/`Picture_PNGDimensions`
— это `Picture_IsLumpCooked`/`Picture_CookedConvert`/`Picture_CookedDimensions`,
остальной код текстур/патчей не менялся.

## Проверка

```powershell
python tools/ps2/verify_pack.py --help
python -B tools/ps2/test_pack_reader.py --pak build/pak --src srb2-assets --out build/agent-pack-g1
python tools/ps2/cook.py --out build/pak-a --tool-dir build/agent-a-tool   # паки с cooked-картинками
python tools/ps2/verify_pack.py --src srb2-assets --pak build/pak-a
python tools/ps2/strip_pics_test.py --negative-controls
```

Python verifier проверяет SHA256 декодированных записей против pk3.
C reader проверяет границы индекса/пула/payload, codec и длины block index;
runtime checksum нет. Хост-тест C проверяет full/partial read, намеренно
невыровненный destination, red zones и повреждённые метаданные.
Доказательства: `docs/GATES/g1/pack/test.log` (до PS2-20), `docs/GATES/g1/a-closure/` (cooked-паки).

## SRP2 версия 2 (OPT12-LOAD, PS2-LOAD-5..11)

Версия 2 — тот же контейнер: те же 64-байтный заголовок, 24-байтные записи индекса, пул строк, кодеки 0/1, блоки по 65536, **та же нумерация ламп** (порядок записей = порядок central directory
исходного pk3: `wadnum`/`lumpnum` не меняются). Читатель (`src/w_pack.c`) принимает версии 1 и 2; версия выше 2 отвергается понятной ошибкой («pack version N is not supported (this engine reads versions 1 to 2):
the pack is newer than the engine»), как и обрезанный файл, битый заголовок («pack header layout is corrupt»), битое расширение, таблица, пул или голова-таблица («... is damaged (checksum)»).
`cook.py --version 1` пишет прежний формат (для сравнения), по умолчанию — 2.

Что добавляет версия 2:

* **Расширение заголовка** по смещению 64 (внутри первого сектора), 64 байта, UINT32 LE: `extsize=64`, `headoffset`, `headbytes=16`, `crcoffset`, `chktable`, `chkpool`, `chkhead`, `chkcrc`, 8 резервных.
  Бит 1 флагов (`SRP2_FLAG_HEAD`, значение 2) означает «есть голова-таблица и таблица CRC32». Бит 0 (немузыкальные данные) как раньше.
* **Голова-таблица** (`headoffset`, кратен 2048): для каждой лампы первые `headbytes`=16 байт её распакованного содержимого (для ламп короче — нули). Движок при старте делает ~22 000 чтений
  заголовков патчей (`W_ReadLumpHeader(.., 16, 0)`); с головой-таблицей они не обращаются к файлу: пак держит таблицу в памяти до `WPack_DropHeads` (конец старта) и только если в нём не меньше 256 ламп
  (`MUSIC.PAK` с 215 лампами её не держит). Чтение, которому нужны байты за головой, идёт обычным путём.
* **Таблица CRC32** (`crcoffset`, кратен 2048): UINT32 LE на лампу, CRC32 распакованных данных. На горячем пути не читается; `-verifypack` (`W_VerifyPackFile`) прочитывает все лампы и сверяет — «целостность вне горячего пути».
* **Контрольные суммы индекса** в расширении: Fletcher-подобная сумма по 32-битным словам LE (`a += w; b += a`, хвост дополняется нулями, результат `a ^ rotl(b, 16)`) для таблицы записей, пула строк,
  голова-таблицы и таблицы CRC. Таблица и пул проверяются при каждом открытии (десятки КБ — несколько миллисекунд), голова-таблица — при чтении в память; таблица CRC — только `-verifypack`.
* **Дедупликация**: лампы с одинаковым содержимым (по SHA-256 распакованных данных) хранятся один раз, записи индекса указывают на одни и те же `position/disksize`; нумерация и имена не меняются.
  Всё, что читает по `position`, и так не предполагает уникальности (`--no-dedup` отключает).
* **Порядок хранения** (необязательно, `cook.py --order FILE`, `tools/ps2/lump_order.py` собирает его из журнала `-lpreads` старта и первого уровня): перечисленные в файле лампы лежат в начале области данных в
  порядке первого чтения, остальные — следом в порядке директории. Старт читает ~1400 ламп несколькими длинными последовательными пробегами. Нумерация ламп не меняется.

Размеры (srb2-assets 2.2.15, байты): `SRB2.PAK` 63 088 640 -> 62 377 984, `ZONES.PAK` 24 444 928 -> 24 442 880, `CHARS.PAK` 2 820 096 -> 2 682 880, `MUSIC.PAK` 100 614 144 -> 100 595 712
(с порядком хранения `SRB2.PAK` 62 382 080).

Совместимость: пак версии 1 читается тем же кодом (без головы-таблицы и без проверок индекса); `--from-pak` перепаковывает существующий пак (v1 или v2) без исходных pk3 и без PNG-инструмента, сохраняя содержимое
ламп (включая cooked-картинки) и их нумерацию. Свои паки кладите в свой каталог (`build/pak2`, `build/pak3`), не в общий `build/pak`.

Проверка (Linux, без MSVC): `python3 tools/ps2/verify_pack.py --src /opt/srb2-assets --pak build/pak2` (SHA-256 каждой лампы против pk3), `python3 tools/ps2/test_pack_reader.py --pak build/pak2 --src /opt/srb2-assets
--out build/hosttest/packtest` (Си-читатель gcc: все лампы четырёх паков, 252 тыс. частичных чтений, 21 отвергаемый случай версии 1, фикстуры версий 1 и 2 с головой-таблицей и без неё (`nohead`),
дедупликацией и порядком, повреждения заголовка/индекса/пула/расширения, версия 3).

## MODELS.PAK (OPT11-MODEL, PS2-HW-260)

Необязательный пак 3D-моделей (`tools/ps2/cook_models.py` из собственных `models/` и `models.dat` раздачи SRB2 2.2.15; в репозиторий не входит). Тот же контейнер SRP2; имена ламп — пути из `models.dat`
(`PLAY/SONIC.md3`, `PLAY/SONIC.png`, `PLAY/SONIC_blend.png`), плюс `models.dat` как есть и `PLAYPAL` (палитра 0, для которой готовились текстуры). Движок (`src/ps2/ps2_models.c`) ищет пак рядом с ELF;
нет пака — тихо, объекты рисуются спрайтами. Форматы ламп (все little-endian, секции кратны 4):
* `SRMD` (модель): заголовок 32 байта (`"SRMD"`, версия 1, полный размер, число поверхностей, кадров, смещения таблиц, флаги, радиус float), имена кадров (16 байт), таблица поверхностей (48 байт: вершин, треугольников,
  кадров, смещения uv / индексов / позиций / нормалей), на поверхность: `float uv[2n]`, `u16 idx[3t]`, `s16 pos[кадры][n][3]` (как их хранит `hw_md3load.c`, масштаб 1/64), `s8 nrm[кадры][n]` (составляющая `z` нормали MD3,
  единственная нужная освещению моделей). Массивы используются на месте (один зональный блок на модель).
* `SRMT` (текстура): 16 байт заголовка (`"SRMT"`, ширина, высота, флаги) и `w*h` байт — индексы палитры 0; 255 = дыра (альфа < 128). Индексы — то, что делает шейдер палитрового рендера ПК с текселем.
* `SRMB` (карта цвета скина): разреженные серии (позиция, длина) и по пикселю (альфа, яркость, среднее каналов); движок строит из неё цветную текстуру в индексах палитры (`hw_md2_ps2.inc`).
Проверка: `cook_models.py --verify` читает пак назад независимым читателем.

## TEXC.PAK (OPT13-IZ, PS2-602, R2): composite textures prebuilt by the cooker

Optional pack beside the ELF (like MODELS.PAK; it is not a wad of the engine: no lump numbers, no netgame file list). The same SRP2 **version 2** container (header, extension, tables, CRC32 per lump,
head table, LZ4HC blocks of 64 KiB, identical lumps stored once), so every reader of the packs reads it; an engine that does not know it simply never opens it, and an engine that does knows how to
run without it. Pack versions 1 and 2 of the game packs are unchanged (nothing in them depends on TEXC.PAK); a game pack that is version 1 has no content identity (below), so no texture whose patch comes
from it has a prebuilt composite.

Why: the hardware renderer composes a wall texture from its patches (`HWR_GenerateTexture`, P8: one palette index per texel, 255 where no patch covers): 4 M EE cycles for a 256x256 texture with its
patches in the zone, 260..320 M when they are not (a lump read from the pack costs 4 M cycles whatever its size), 26 M for the sky (151 patches); it happens in the middle of a frame whenever a texture is
asked for the first time or after the zone dropped its data (docs/research/rdrv/OPT13_RDRV.md, 2.6). The cooker makes the same pixels once; the console decodes them (0.1..1.5 M cycles).

* **Lumps.** `TEXCINFO` (lump 0, 16 bytes: `"TXC1"`, UINT32 format 1, UINT32 number of composites, UINT32 0), then one lump per distinct texture definition, named by the 16 lower case hex digits of its
  key (no extension), in texture list order (a zone's textures lie together, so the prefetch of a level reads a few long runs). The lump is the `width * height` bytes of the composite, rows top to
  bottom, as `HWR_GenerateTexture` makes them for `GL_TEXFMT_P_8`; stored raw below 256 bytes or when LZ4HC does not get under 90%, else as every other lump (block index when over 64 KiB).
* **Key.** FNV-1a 64 over (little endian words) the format number (1), width, height, type, patch count, and for every patch: origin x/y, wad number, lump number, flip, alpha, and the three index
  checksums of the pack the patch lump is in (`chktable`, `chkpool`, `chkcrc` of the version 2 extension: the checksum of the CRC32 table covers the content of every lump). `PS2TexC_Key`
  (src/ps2/ps2_texc.c) computes it on the console and in the host engine of the cooker alike. A texture whose key is not in the pack (an add-on's definition or patches, a pack other than the one the
  composites were made for, a blend style other than copy and translucent (those find the nearest colour of the palette of the moment), a square flat (a floor), a size over 1 MiB) is composed
  from its patches exactly as before, so an add-on can never see a stale composite. A translucent patch (a blend in the translucency tables TRANS10..TRANS90) puts the identity of those nine lumps in the
  key too. Vertical flips, translucent patches and flats that are walls (the sky `SKY4` is a flat of 512x768) are included: they are the slowest textures of the game.
* **Making it.** `tools/ps2/cook_texc.sh PAKDIR OUTDIR`: the host engine (the PS2 profile on x86) loads the packs, builds its texture list with the engine's own code and composes every eligible
  texture (`SRB2 -texcdump FILE`, about 1 s); `tools/ps2/cook.py --texc FILE --out OUTDIR` packs the dump. The pack is for exactly the game packs in PAKDIR: cook the game packs again and this
  again (a changed pack changes its checksums, hence every key, hence every lookup misses: nothing wrong is ever shown).
* **Reading.** `PS2TexC_Fetch` (hw_cache.c: first thing in `HWR_GenerateTexture` for a P8 texture): the stored form from the level's prefetch (`PS2TexC_PrefetchLevel` after the level was loaded: the
  textures of its sidedefs, its sky and the animations they belong to, one read per run of nearby lumps, kept in one long-lived block), else one lump read; `WPack_DecodeMem` decodes it straight into
  the texture's data block. `-notexc` ignores the pack, `-texcmem KiB` limits the prefetch (default 1024), `-texccheck` compares every stored composite with the original composition at the first
  level load (`TEXC check: N textures checked, M differ`), `-hwdbg 16777216` checks each texture when it is made.

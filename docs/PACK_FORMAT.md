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

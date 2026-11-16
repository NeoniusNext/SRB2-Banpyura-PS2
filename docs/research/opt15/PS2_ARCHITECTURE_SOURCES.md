# OPT15: архитектура PS2 и границы оптимизаций без потерь

Проверено 2026-11-15; исходная точка — `e10593b0d` до изменений этой волны.
Это адресная проверка для нынешнего Hardware renderer, а не «вся документация
PS2». Широкий каталог уже есть в
[architecture sources](../../PS2_ARCHITECTURE_AND_OPTIMIZATION_SOURCES_20261115.md),
а материалы swizzling и clear strips — в
[NEXT13](../../PS2_RESEARCH_NEXT13_20261115.md). Здесь добавлены конкретные
ограничения upstream allocator/VIF и карта зависимостей ресурсной политики.
Сам отчёт не меняет runtime, зависимости или настройки и не доказывает выигрыш.

## Проверенные первичные источники

Sony manuals — оригинальные SCE документы v6.0, April 2002, в архивном зеркале,
не на современном сайте поддержки Sony. PDF получены HTTP GET и просмотрены
через `pdftotext`; web-reader отвергает их `application/octet-stream`.
Полные документы в репозиторий не добавлены. Читались перечисленные разделы.

| Источник | Проверенные разделы и практическая роль |
| --- | --- |
| [EE Overview](https://raw.githubusercontent.com/ninjadynamics/PS2Docs/main/EE_Overview_Manual.pdf) | §§1.5/2.2.1/2.3.1–2.3.4: раздельные памяти и связь EE/VU/GS; вводные части §§3.1–3.2: транспорт данных. |
| [EE Core User's Manual](https://raw.githubusercontent.com/ninjadynamics/PS2Docs/main/EE_Core_Users_Manual.pdf) | §§1.6/6.2, особенно p131: cache geometry; §8.1 pp155–156: ограничения формата FPU. Нельзя доказывать точность EE/VU преобразования одной host-моделью. |
| [EE User's Manual](https://raw.githubusercontent.com/ninjadynamics/PS2Docs/main/EE_Users_Manual.pdf) | §5 p41: физические DMA addresses, qword alignment, отсутствие snooping; §§6.3.2–6.3.3 pp87–88: NUM/alignment; §6.4 pp111–113: FLUSHE/FLUSH/FLUSHA; §§7.1.1–7.1.3 pp148–149: PATH arbitration. |
| [GS User's Manual](https://raw.githubusercontent.com/ninjadynamics/PS2Docs/main/GS_Users_Manual.pdf) | §§2.6–2.7 pp27–31: indexed stride/CLUT; §3.4.7 pp55–57: internal CLUT loading; §4.1.5 p76: transfer restrictions; §§8.2–8.4 pp163–170: page/block layout и разница occupied blocks/contiguous span. |
| [VU User's Manual](https://raw.githubusercontent.com/ninjadynamics/PS2Docs/main/VU_Users_Manual.pdf) | §1.1 p16: local memories; §2 pp25–29: numerics; §3.4 pp44–47,50–52 и XGKICK p196: зависимости и жизненный цикл output buffer. |
| [SCE: EE и GS на одном chip](https://sonyinteractive.com/en/press-releases/2003/emotionengine-and-graphics-synthesizer-used-in-the-core-of-playstation-become-one-chip/) | **Новый официальный endpoint:** release от 2003-04-21, таблица New Semiconductor Specification подтверждает 4 MB embedded DRAM и 294.912 MHz. Это характеристики chip, не руководство по синхронизации. |
| [PS2SDK graph_vram.c](https://github.com/ps2dev/ps2sdk/blob/b04e8973c8a9839d0653d87a8d30cc67fd3c3f05/ee/graph/src/graph_vram.c), [header](https://github.com/ps2dev/ps2sdk/blob/b04e8973c8a9839d0653d87a8d30cc67fd3c3f05/ee/graph/include/graph_vram.h) | **Дополнение к прежнему каталогу:** `graph_vram_size` и единицы API. Header считает 32-bit words; функция предполагает, что width ≤16 означает palette. Это не универсальная замена точного footprint. |
| [PS2SDK packet2_vif.h](https://github.com/ps2dev/ps2sdk/blob/b04e8973c8a9839d0653d87a8d30cc67fd3c3f05/ee/packet2/include/packet2_vif.h), [gsKit gsTexture.c](https://github.com/ps2dev/gsKit/blob/8ef73d05f022d14b3ded7c70cba670fe333ed61a/ee/gs/src/gsTexture.c) | Повторно проверены UNPACK/DIRECT open/close и `gsKit_texture_size_ee`/`gsKit_texture_size`/upload. Это исходники конкретных библиотек, а не дополнительные аппаратные гарантии. |

Повторная проверка GitHub API подтвердила upstream tips из прежнего каталога:
PS2SDK `b04e8973c8a9839d0653d87a8d30cc67fd3c3f05` (2026-11-14), gsKit
`8ef73d05f022d14b3ded7c70cba670fe333ed61a` (2026-09-16). Это не определяет
версию библиотек, реально связанных с ELF порта.

## Ограничения, которые легко перепутать

EE имеет 32 MiB основной RAM, 16 KiB I-cache, 8 KiB D-cache с 64-byte lines
и 16 KiB scratchpad. VU0 имеет по 4 KiB micro/data memory, VU1 — по 16 KiB.
`DC_BUDGET=4 MiB` в драйвере — **программный кэш текстур в EE RAM**, а не
аппаратный D-cache. GS имеет 4 MiB общей памяти для цвета, Z, textures и CLUT.

GS page — 8192 B, block — 256 B. У T8 page 128×64, block 16×16;
у T4 page 128×128, block 32×16. Indexed TBW задаётся в единицах 64 texels,
но должен соответствовать ширине, кратной 128. CT32/Z24 layout, CLUT, transfer
padding и занимаемые диапазоны нельзя заменить подсчётом исходных texel bytes.

Новый показательный пример из GS §8.3.3 и upstream source: T8 16×32 с TBW=2
касается blocks **0 и 2**. Payload равен 512 B, а минимальный непрерывный span —
768 B. `graph_vram_size(16,32,GS_PSM_8,GRAPH_ALIGN_BLOCK)` возвращает
128 words = 512 B из-за palette shortcut. gsKit даёт консервативные 1024 B;
драйверный `tex_footprint` — точные 3 blocks. Это вывод из адресной схемы и кода,
не запуск GS; пример показывает, почему копирование чужого allocator опасно.

CSA выбирает место в **внутреннем** CLUT buffer. Это не смещение источника
от CBP. Нынешние CSM1 palettes и их generations/CLD/TEXFLUSH нужно сохранять,
включая alpha-zero RGB: эти цвета участвуют в bilinear filtering.

DMA требует 16-byte alignment; cache coherency и завершение чтения REF source
отдельны от завершения GS drawing. `FLUSHE` ждёт microprogram, `FLUSH` также
PATH1/2, `FLUSHA` добавляет PATH3. Важная оговорка p112: `FLUSH` может не
дождаться queued `XGKICK` с E-bit. Нынешние `.vsm` используют обычный XGKICK
и отдельный `NOP[E]`; слияние этих инструкций требует нового доказательства.

`UNPACK.NUM=0` означает **256**, не пустую команду. Текущий chunk 255 QW —
выбор протокола драйвера. Его VU1 layout уже содержит input 64..319/320..575,
output 576..655/656..735, vertex lists 736..855; свободного общего буфера
«ещё 16 KiB» нет. Перенос границ должен учитывать constants, row tables,
clip expansion, XGKICK readers и sprite program.

## Что уже реализовано и что пригодно сейчас

В исходной точке уже есть точный block footprint, merged free ranges, LRU,
keyed/plain views, CLUT generation tracking, zero-copy uploads с pinning,
bounded DMA ring, VIF/VU1 double buffering, REGLIST short fans и точные capture
copy/spill paths. Есть ранние plane/sphere/quad tests. Их наличие проверено в
`ps2_hw_tex.inc`, `ps2_hw_gs.inc`, `ps2_hw_plan.inc`, `ps2_hw_vu1.inc`,
`ps2_hw_fx2.inc` и `hw_front.inc`; повторная реализация этих механизмов не
считается новым выигрышем.

Для данной волны подходят сокращение CPU поиска и повторных вычислений,
уплотнение только metadata при проверке целевого ABI и сохранении ёмкости, memoization уже
существующего predicate. Сохраняются результаты, порядок обращений, LRU,
packet lengths, точки pin/release и ошибки. В частности, ускорение прежнего
plane test не должно добавлять новые rejects или переставлять float expressions.

Для новых culls опасны near/eye crossings, homogeneous w, float uncertainty,
interpolation, rolling/paper sprites, модели/attachments/shadows, slopes/3D
floors, skybox и splitscreen. Граница или невалидные данные идут в прежний
полный путь. Center-distance cutoff и прошлый invisible result не являются
доказательством отсутствия нынешних видимых pixels.

## Почему уменьшение видимой работы ещё не сохраняет картинку

Это вывод из текущего кода и уже исполненных
[visibility](../../PS2_VISIBILITY_NEXT12_20261115.md),
[P4](../../PS2_P4_NEXT12_20261115.md) и
[clear](../../PS2_VRAM_NEXT13_20261115.md) counterexamples:

| Изменение | Цепочка до будущей картинки |
| --- | --- |
| Пропуск невидимого draw | texture selection/transparent classification → `plan_tex_touch`, `unplanned_cur`, eviction/thrash → `plan_reserve`/`plan_pct` → выбранные levels. Удаление sort node дополнительно меняет plane-run barriers. |
| Меньший физический footprint | changed residency/eviction history → другой budget → другой результат немонотонного controller. «Больше места» не гарантирует сохранения каждой текстуры. |
| Другая длина GIF | другой `pk_flush` → `rel_release_buf`/`rel_poll` → другой busy/idle victim в `dc_slot` → другой source в `dc_find_finer` → другие bytes `dec_level`. |

Поэтому будущий logical/physical split должен сохранять **две** истории:
канонические VRAM admission/eviction/quotas и CPU source-cache selection/
lifetime. Одного прежнего `nblk` недостаточно. Shadow accounting только
прибавленных blocks не воспроизводит retries, views, reuploads или busy sources.

Практическая последовательность следующего исследования:

1. Сначала read-only trace уже существующих решений: texture key/generation,
   chosen level, source level/hash, victim/stamp, logical footprint, planner
   state, REF pin/release и ring boundaries. Сопоставлять причинные события,
   а не лишь итоговый framebuffer или число free bytes.
2. Для физического packing сохранить canonical admission и eviction decisions;
   экономию физического места не возвращать controller как новую quota.
   Отдельно доказать CPU cache trace, DMA source ownership, captures/views,
   failure/retry и publication после relocation.
3. Детерминированное построение каждого level от canonical source устранило бы
   зависимость bytes от cache history, но **меняет существующее поведение**:
   текущий `dec_level` неассоциативен. Это отдельное исправление с собственной
   согласованной семантикой, не скрытая оптимизация данной волны.

Virtual ring cursor или logical release сами по себе проблему не решают:
физическая DMA может ещё читать нужного canonical victim. Ожидание сохраняет
его lifetime, но добавляет stalls. Кроме того, текущие asynchronous completion
observations зависят от timing; trace при одной completion schedule не доказывает
равенство на всех физических расписаниях. Ни compact VRAM, ни новые culls этим
отчётом не принимаются и не объявляются невозможными.

## Что требует измерений на PS2

После семантических проверок нужны целевой build/ABI/code binding и совместный
frame/resource replay. Физическая PS2 нужна для EE cache/bus stalls, VU/GIF
backpressure, CLUT/TEXFLUSH поведения и завершённой GS работы. Измерять отдельно
EE submission и время до FINISH, а RAM — по одновременно живым allocations с
alignment/pins. PCSX2 или host могут проверить широкий набор cases, но не
устанавливают эти physical timings. Новые FPS/RAM/VRAM цифры здесь не заявлены.

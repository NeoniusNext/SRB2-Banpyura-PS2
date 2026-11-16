# PS2 Hardware OPT17: исправленный tile-V9, strict admission не пройден

**Текущий кандидат — `321c8d4d`; полный strict admission не пройден.**
Чистый Hardware/LTO ELF собран заново после двух исправлений CPU ownership.
Предыдущий `22b5620` / ELF `82bc5f26` отклонён: оба switch-chain прогона
PAL и 480p завершились heap ownership fatal. Их результаты ниже сохранены
как исторические, включая strict FAIL.

Документ относится к обычным **32 МиБ RAM**. Применённый tile-путь —
**V9 borrowed-wide**, расширение private V8 indexed128. Здесь «V9» означает
версию tile-пути; прежний **core-v9 до tiles** — другой снимок. Завершённые
component proofs и прототипные switch-проверки не заменяют admission
нового shipping ELF. База A/B —
`604cfd150c5240142d8cb5367950b53d987916d1`.

Текущий production source закреплён коммитом
`321c8d4da2cdc4d175271726b8b2bc450cbbc57b`: **607 source-файлов**.
Официальный [build tool](../tools/ps2/build.py) заново собрал **204 C units
и две VU программы**, без compile/link diagnostics. Чистый недиагностический
ELF использует обычный OS-seeded RNG, имеет размер **11329336 Б**, SHA256:
`7bf67e21438e697cf40c112b9166c2a3018956fbe909ef34f0780de17750e96d`.
607 source hashes проверены до и после сборки; generated build metadata
соответствует `321c8d4d`. SHA256 полного source manifest:
`c35217c87713e8f4be5edd8e765bbd92ef78997f1a1aeef259b4a4b536bb078c`;
он закреплён в [public production-input binding](research/opt17/restart/unchanged-gs-production-inputs.json).
[Итоговый validation journal](research/opt17/final-321c/validation-journal.json)
подтверждает завершение **всех 12 требуемых стадий**, а также дополнительных
resource и MAP11 наблюдений. В
[артефакте](../prebuilt/hardware-opt17-20261116/README.md) находятся
[ELF](../prebuilt/hardware-opt17-20261116/SRB2-PS2-Hardware.ELF),
[source manifest](../prebuilt/hardware-opt17-20261116/source-sha256.json),
[build report](../prebuilt/hardware-opt17-20261116/build-report.json),
[validation](../prebuilt/hardware-opt17-20261116/validation.json) и
[SHA256SUMS](../prebuilt/hardware-opt17-20261116/SHA256SUMS).
Validation сохраняет `all_required_matrix_stages_completed=true`,
**`all_strict_checks_pass=false`**, `unexpected_new_failures=[]`.
Это завершённый измерительный раунд с указанными ниже ограничениями.
Предыдущая попытка сборки r2 прервалась при смене окружения до получения
ELF и сохранена как incomplete.

| Поле текущего кандидата `321c8d4d` | Статус |
| --- | --- |
| Production source | **Закреплён:** 607 файлов; полный manifest SHA256 выше |
| Hardware/LTO build, размер и SHA256 ELF | **Собран:** 11329336 Б, 204 C + 2 VU, 0 compile/link diagnostics; полный hash выше |
| CPU ownership | Два узких исправления; actual-function/real-Zone host proofs завершены |
| PAL/480p switch chain на новом shipping ELF | **Завершены:** оба по 175f/12 PPM без fatal/fallback/audio errors; по 40 missing-draw diagnostics, strict **FAIL** |
| Default A/B | **15/15 точных PPM пар**, loader/config точны; 400 diagnostic lines, десять strict errors — **FAIL** |
| Full A/B | **15/15 точных PPM пар**, loader/config точны; 0 skips/warnings/errors — **PASS** |
| Default 350f MAP02 RAM / ordered streams | **+1920768 Б net available**, все пять streams точны; три strict memory errors — **FAIL** |
| Shipping PAL/480p switch + heat/wipe | **Все 8 integration chains завершены, 46 valid full PPM captures**; 0 fatal/fallback/warnings/audio errors; по 40 missing-draw diagnostics, strict **FAIL** |
| Canonical стандартный GS | **34/34 readback hashes точны**, reference unchanged; CT32 55/56, CT16 50/51, прежний VU0 strict **FAIL** |
| Resource A/B | Gap-layout и TEXC prefetch 0: по **3/3 точных PPM пар**, loader/config точны, 0 errors — scoped **PASS** |
| MAP11 | Ordinary heuristic → Software на frame 0; diagnostic admission → actual OOM **17544 Б PU_RENDERWORK** → Software на frame 1; оба wrapper exit 0 / I_Shutdown marker |
| Итоговый strict admission | Все требуемые стадии завершены; **FAIL сохранён**, общий FPS gain и работа на физической PS2 не установлены |

Физика, игровые ресурсы, форматы, ID и функциональные лимиты не урезаны.
Экономия не основана на уменьшении логических текстур, точности framebuffer
или depth, отключении эффектов, Lua, аддонов либо сети. Совместимость всех
карт, отсутствие fallback во всей игре, общий прирост FPS и результат на
физической PS2 не установлены.

**Разрешение PAL:** при выбранных в меню **320×200** это logical size игры.
[`hw_choose_size`](../src/ps2/hw/ps2_hw_gs.inc#L754) выбирает для PAL
внутренние framebuffer/viewport **320×256 CT32**: высота делит 512 и позволяет
целочисленное CRTC scaling с PAL field/frame режимом. Отображаемые PCSX2
**640×512** относятся к видеовыводу; 3D raster в этом случае имеет размер
320×256. Полный кадр и его aspect сохраняются. Для NTSC и 480p функция
выбирает размеры по их собственной развёртке.

## Исправление CPU ownership при возврате Hardware

[Публичный restart index](research/opt17/restart/README.md),
[actual-function разбор](research/opt17/LIFECYCLE_ZONE_OWNERS_20261116.md)
и [host proof](research/opt17/restart/lifecycle-host-results.json) отделяют
наблюдённую причину fatal от независимо найденного второго дефекта.
Failure-only diagnostic ELF сохранил исходный `Z_CheckHeap` и определил
живой **PU_HWRPLANE блок 818176 Б**: payload `0x0191db00`, owner cell
`0x0082ba18`, held value `0x01400cc0`. Link map определяет owner как
`&gc.blk` (`gc + 16`). Расположение owner вне Zone arena допустимо;
нарушение — несовпадение значения owner с payload старого живого блока.

При same-level Software → Hardware цепочка
`HWR_LoadLevel → HWR_PlaneCacheReset → HWR_GCacheLevel` могла выполняться
до освобождения PU_HWRPLANE. Старый reset забывал живой `gc.blk`; последующая
allocation записывала новый payload в ту же owner cell. Новый reset сначала
отсоединяет interned batch-state pointers, освобождает живой `gc.blk` через
`Z_Free`, затем выполняет прежний reset. Когда Zone уже освободил блок при
level teardown, owner равен NULL и повторное действие безопасно. Размеры
cache, reclaim policy и heap-checker не изменены.

Второй дефект: PS2_PROFILE keyed-flat child имеет собственную CPU pixel copy,
но cleanup применял legacy alias rule и освобождал child metadata с живым
Zone owner. Теперь собственные pixels освобождаются перед metadata в paged,
compact и dense opt-out вариантах. Для legacy shared alias без PS2_PROFILE
прежний `delete=false` сохранён; source stealing с `child.data=NULL` безопасен.
Raster driver, framebuffer/depth precision, tile layout и graphics tolerance
этими двумя исправлениями не менялись. Mipmap/driver records остаются **68/140 Б**.

Actual `HWR_GCacheLevel` с реальным Zone проходит по **1495 checks / 64 resets**
в normal и ASan/UBSan вариантах. Actual flat create/regenerate/steal/cleanup
проходит по **504 checks / 24 cycles / 12632064 exact source bytes** для
pages, compact и dense. Старые реализации воспроизводят proper-user error
и живой Zone child после actual libc metadata free; owner-reuse negative
воспроизводит ошибку без host use-after-free. Target compilation проходит.
Это CPU lifecycle proof с fixture boundaries для driver/raster services.

Оба прототипных [PAL](research/opt17/restart/fixed-switch-pal-prototype.json)
и [480p](research/opt17/restart/fixed-switch-480p-prototype.json) прогона
с объединёнными исправлениями завершили исходную цепочку **175 кадров / 12 PPM**
на 32 МиБ, с неизменённым heap-checker, без fatal, automatic fallback и audio
errors. Прототипный ELF SHA256:
`9c6a93248cff46fed3cc1e6eac1ecabf3481f2e4a52d4fe0eb8ed1ae3ffa85fb`.
Он ещё имеет build metadata `22b5620`, поэтому это отдельная прототипная
проверка двух исправлений, а не результат нового shipping ELF `7bf67e21`.
В каждом прототипном прогоне остаются **40 `selected while collecting`**
diagnostic lines: `integration_passed=true`, но
**`strict_missing_draw_passed=false`**. Heap fix не отменяет этот strict FAIL.

Combined lifecycle manifest SHA256:
`d102eb845754e72320653ec7093eb8404219500576d8d9bf96d89095b3a2103a`.
Он охватывает **21 исходник**, включая `hw_gcache.inc`; относительно `22b5620`
изменились только `hw_cache.c` и `hw_gcache.inc`. Исторические tile fixtures
привязаны к прежним 20 файлам и не компилируют эти CPU engine routines.
[Production-input binding](research/opt17/restart/unchanged-gs-production-inputs.json)
проверяет по **102 реально компилируемым production inputs** для прежних
positive и halo-negative builds, а также **35 unhooked driver files**:
все побайтно совпадают с текущим source. `hw_cache.c` / `hw_gcache.inc`
не входят в standalone compilation; их покрывают отдельные actual-Zone
proofs и engine switch chains. Это перенос component GS evidence при
неизменных inputs, а не семь новых boots и не равенство старого 20-файлового
guard всему новому 21-файловому admission.

## Текущий `321c8d4d`: завершённые стадии

### Shipping switch chains

На новом `321c8d4d` / ELF `7bf67e21` оба исходных
[PAL](research/opt17/final-321c/switch-pal.json) /
[480p](research/opt17/final-321c/switch-480p.json) прогона завершили
**175 кадров / 12 PPM** с `wrapper_exit=0`, `integration_passed=true`,
корректными payload/dimensions, без fatal, automatic fallback, audio errors
и warnings. В обоих сохранён исходный heap-checker. Это свежая engine
проверка исправленного same-level Hardware → Software → Hardware lifetime.
Report SHA256: PAL
`f245617af04bd079b0c6a892e416bb86ead9ac4945a1f3440b4c523533043b86`,
480p `2be64c38ec2445e7db9b1fdfbb507d5c55f8df4242375360d9d52b2ad5c3eed0`.

Каждый прогон сохраняет **40 `selected while collecting`** diagnostic lines
и **`strict_missing_draw_passed=false`**. Отсутствие heap fatal не отменяет
strict missing-draw FAIL. Эти 12 captures не являются A/B pixel parity всех
high-res сцен; остальные стадии имеют отдельные gates.

### Default image A/B

Свежий [default stage](research/opt17/final-321c/default-images.json)
на `321c8d4d` завершил пять сцен (fog/NPOT, MAP02,
MAP01, legacy, legacy-sprites), десять boots и **15/15 побайтно точных полных
PPM пар**. Все пять loader-hash и saved-config сравнений точны. Парные ELF
имеют одинаковый diagnostic local RNG seed `0x01b2d3f5`; это scoped A/B
нового source, отдельно от обычного shipping RNG. Report SHA256:
`8e6693590b247107e26f33852c77b7752d3075c7e8fbca580cf7f920165a1c12`.
[Image runner](../tools/ps2/visibility_memory_integration.py) сохраняет
complete-scene и strict missing-draw gates.

Strict stage **FAIL: десять errors, по одному на boot**. Сохранены
**400 HWSKIP diagnostic lines — 40 на каждый из десяти boots**, по 200 для
baseline и candidate. Все `selected while collecting`; `purged_source`,
`not_resident`, `other` — 0. Это ограниченные diagnostic lines, не полный
счётчик пропусков; точные PPM не отменяют strict errors.

### Full-texture image A/B

Свежий [full-texture stage](research/opt17/final-321c/full-images.json)
завершил те же пять сцен и десять boots с `-hwdbg 33554432` в обеих ролях.
Все **15/15 полных PPM пар**, loader hashes и saved configs точны. Все четыре
skip-классификации, warnings и run errors равны **0**; stage **PASS**.
Report SHA256:
`375cfcf28983a6e982d71d3df0fa53466ea57dcd29bc1bdbf7fce2329cd463e8`.
В сумме default/full получены **30 точных PPM пар**. Это diagnostic local-RNG
A/B; full-mode PASS не отменяет default strict FAIL.

### Default MAP02 pressure: 350 кадров

Свежий [pressure report](research/opt17/final-321c/default-pressure.json)
использует одинаковые наблюдатели (`con_hudlines 0`) и 350 кадров MAP02
в обеих ролях. SHA256:
`6c2b8d7b589a7aa2ffd8976146edd3fac22d636f3a10cf934eba0f509e2cb5e1`.
[Pressure runner](../tools/ps2/visibility_memory_pressure.py) сохраняет
исходные strict memory правила. Это новое измерение `321c8d4d`; совпадение RAM deltas с историческим `22b5620`
не заменяет эту отдельную source-bound привязку.

| Показатель, байт | База 604 | Текущий `321c8d4d` | Изменение |
| --- | ---: | ---: | ---: |
| Zone used / peak / global peak | 22018736 | 20044144 | −1974592 |
| Zone free | 1529168 | 3462800 | +1933632 |
| Zone largest | 1511824 | 3441008 | +1929184 |
| C-heap free | 169656 | 156792 | −12864 |
| Net available: zone free + C-heap free | 1698824 | 3619592 | **+1920768** |
| Zone arena | 23547904 | 23506944 | −40960 |
| Относительный C-heap peak | 426496 | 442432 | +15936 |
| Spill: текущий / peak | 2118912 | 1360512 | −758400 |
| Использованный stack | 132560 | 132560 | 0 |

Чистый прирост доступной RAM — **1,831787109375 МиБ** в этом MAP02/350f
сценарии. Spill уже входит в zone accounting и не прибавляется повторно.
Gameplay evictions после стартовой очистки — **0 в обеих ролях**; startup
имеет по 31 evictions, 7 failed attempts и 0 flushes. Evicted bytes меняются
110784 → 110800 (+16 Б).

Все пять полных ordered streams побайтно точны: **10 LHASH, 407 HWPH,
2 HWLT, 49 HWWS и 3 HWPROF resource tail**. Strict pressure stage всё равно
**FAIL по трём исходным правилам**: `libcpeak` +15936 Б, `arena` −40960 Б,
`libcfree` −12864 Б. Count 387113003 → 407122528 и `t_ms` 27893 → 27979
не интерпретируются как FPS gain. Эти RAM числа не охватывают retained
high-res captures или все карты.

### Shipping PAL/480p: switch, heat и wipe

Все **восемь** high-res/mode/effect scenarios на чистом shipping ELF
`7bf67e21` завершены с `wrapper_exit=0`, `integration_passed=true`, без
fatal, automatic fallback, warnings и audio errors. Получены **46 valid
full PPM captures**. Это 46 сохранённых кадров с корректными dimensions и
payload, а не 46 A/B pixel comparisons.

| Публичный stage report | Кадры | PPM | Zone global peak, Б |
| --- | ---: | ---: | ---: |
| [Switch PAL](research/opt17/final-321c/switch-pal.json) | 175 | 12 | 20668064 |
| [Switch 480p](research/opt17/final-321c/switch-480p.json) | 175 | 12 | 20653008 |
| [Heat PAL](research/opt17/final-321c/heat-pal.json) | 45 | 3 | 22408576 |
| [Heat 480p](research/opt17/final-321c/heat-480p.json) | 45 | 3 | 23310368 |
| [Wipe PAL](research/opt17/final-321c/wipe-pal.json) | **137** | 5 | **23468224** |
| [Wipe 480p](research/opt17/final-321c/wipe-480p.json) | **137** | 5 | 23405504 |
| [Heat-wipe PAL](research/opt17/final-321c/heat-wipe-pal.json) | 38 | 3 | 22469840 |
| [Heat-wipe 480p](research/opt17/final-321c/heat-wipe-480p.json) | 38 | 3 | 23362208 |

**Каждый** из восьми reports сохраняет **40 `selected while collecting`**
diagnostic lines и **`strict_missing_draw_passed=false`**. Heap recovery
и integration success не отменяют strict missing-draw FAIL. Эти прогоны
не устанавливают A/B parity всех игровых high-res кадров или FPS gain.

Наибольший наблюдавшийся Zone global peak — **23468224 Б** в wipe PAL
при arena **23506944 Б**: разность capacity−peak всего **38720 Б**. Это
разность Zone accounting, а не общий запас всей RAM: C-heap учитывается
отдельно, часть cache purgeable, а итоговая free-память относится к другой
точке прогона. Spill уже входит в Zone и не прибавляется повторно. Эти
scoped peaks не доказывают, что все карты или все transient working sets
помещаются в 32 МиБ. Завершение этих сценариев не отменяет strict
missing-draw FAIL или отдельный MAP11 fallback ниже.

### Canonical стандартный GS

[Свежий canonical report](research/opt17/final-321c/standard-gs.json), SHA256
`a4817f282b47282291be5074dbdfa8f0e2b1fdd7a4c4b3bb8c3c281a40001280`,
использует исходный [GS-list builder](../tools/ps2/gs_list_eetest.py), включая
обе GS-list model checks. Это отдельный native fixture ELF
`265b4b5132755eceec2b701d2d14c660e912e158f3ff2fbf928195be89cf4c2e`,
а не shipping ELF `7bf67e21`. Все **34/34 полных CT32/CT16 readback hashes**
побайтно совпадают с неизменённым golden reference:
`reference_unchanged=true`, `regression_errors=[]`.

Strict gate всё равно **FAIL**: CT32 **55 pass / 56 checks**, CT16
**50 pass / 51 checks**, в обоих тот же `vu0_transform` numerical failure:
49152 vertices, 0 outcode differences, max dX=1, dY=1, dZ=86 и max relative
1/w error **3.61343e−5**. `strict_passed=false`, suite exit **1** сохранён;
reference, transform arithmetic и tolerance не изменены. Наличие точных
readbacks не превращает numerical FAIL в pass. Первый incomplete basic-fixture
прогон с 30/34 readbacks сохранён отдельно в [restart history](research/opt17/restart/README.md);
его неполный набор не заменяет эту корректную canonical проверку.

### Optional resource sets: same-final-ELF A/B

[Свежий resource-native report](research/opt17/final-321c/resource-native.json),
SHA256 `96dfe5ed6cd5255509d96120a69e4d1affedd93c76de2cf075643155c16d607c`,
проверяет gap-layout PAK и TEXC с `-texcmem 0` относительно stock ресурсов
на **одном и том же final-source diagnostic ELF** `19384ec9`, с local RNG
seed и `-hwdbg 33554432`. Оба набора дали по **3/3 полных буквальных PPM пар**,
точные loader hashes и saved config, без skips, warnings или errors;
оба scoped **PASS**. Это visual/resource admission MAP02, без вывода об общем
ускорении загрузки, FPS, steady-state RAM или всех картах. Опциональность
layout и default append-only policy сохранены.

### MAP11: обычный guard и diagnostic admission

На том же чистом shipping ELF `7bf67e21`
[ordinary guard](research/opt17/final-321c/map11-guard.json) распознаёт
**15942 subsectors** и переводит MAP11 из Hardware в Software на **frame 0**.
Production thresholds и actual OOM recovery в этом прогоне не переопределены.

В отдельном [diagnostic admission](research/opt17/final-321c/map11-admission.json)
только preemptive SS/free heuristics заданы через
`-hwfbss 20000 -hwfbfree 0`; **actual OOM/driver recovery остаётся armed**
(`-hwfb`). Уже на **frame 1** возникает recoverable request
**17544 Б PU_RENDERWORK**, и guard переводит карту в Software. Это свежий
фактический OOM текущего кандидата, отдельно от исторического core-v9
PU_PATCH request 19504 Б. Размеры и теги относятся к разным observations;
их разность не является RAM gain.

Оба MAP11 прогона доходят до 35-frame quit, `wrapper_exit=0`, без fatal,
с `I_ShutdownSystem(): end of logstream.`. [Exact runtime commands](research/opt17/final-321c/map11-runtime-commands.json)
закрепляют аргументы, armed OOM recovery, неизменность frozen inputs,
non-UI settings и emulator binary, а также shutdown markers. Software
fallback здесь реально наблюдён; полного Hardware admission MAP11 или
универсального Hardware residency не установлено.

## Исторический игровой A/B: отклонённый `22b5620`

Production source этого исторического кандидата:
`22b5620cf0710b06541b36eee8801eaad772deb3`, **607 source-файлов**, прежний
20-файловый tile/source-lifetime guard. SHA256 полного source manifest:
`bd6a188a3ee45e5c5d68467ee8723f6caf2298f824c4c81c9b29825fd7411ef6`.
Его чистый ELF размером **11329336 Б**, SHA256
`82bc5f2690d56e87c3f9b9d032ef0bccefb73226037aadeac96a54e36de66d7a`,
**REJECTED для release**. Все числа в этом разделе относятся к нему,
а не к текущему `321c8d4d`. Парные image/pressure прогоны используют отдельные
одинаково изменённые diagnostic ELF с local RNG seed `0x01b2d3f5`;
это не проверка shipping RNG.

Исторический default image stage дал **15/15 точных PPM пар**, точные
loader/config и **400 diagnostic lines (40 × 10 boots, 200 на роль)**,
все `selected while collecting`; **десять strict errors — FAIL**.
Full-texture stage дал **15/15 точных пар**, точные loader/config и
**0 skips/warnings/errors — PASS**. SHA256 старых reports: default
`7a98b2654f1944bb111503db3af608459e81b6c02b17179e077c6c591479906f`,
full `27af352d644f6cc2a1698e79af43715f157af0be1af3883507635d7b90b5bdc7`.

Исторический pressure report SHA256:
`35453803933de9bf622230c5a9db64119964312314eb5b99a258e9c1e25ef1e2`.
На его MAP02/350f было **+1920768 Б net available**, точные пять streams
10/407/2/49/3 и те же **три strict memory errors** (`libcpeak`, `arena`,
`libcfree`). Эти старые наблюдения сохранены; подробная таблица выше уже
относится к отдельно повторённому текущему `321c8d4d`.

Оба исторических switch chain — [PAL](research/opt17/restart/failed-switch-pal-22b.json)
и [480p](research/opt17/restart/failed-switch-480p-22b.json) — завершились
на gametic 141 с `wrapper_exit=4`, `integration_passed=false` и
`Z_CheckHeap -1140: ... doesn't have a proper user` (block 2028 и 1945
соответственно). Каждый успел сохранить **11 PPM** с корректными
payload/dimensions, включая high-res modes и Hardware → Software → Hardware,
но возврат всей mode-chain не принят. Эти captures не доказывают отсутствие
corruption или пиксельное равенство high-res сцен. Automatic fallback,
audio errors и warnings отсутствуют; **40 `selected while collecting`**
diagnostic lines в каждом прогоне также сохраняют strict FAIL. Исходный
PAL report SHA256:
`d4f37d9413bba9b9aa5cab12fd4817b26fe01e271b04dd2809e03ab98bec1714`.
[High-res runner](../tools/ps2/opt17/highres_game.py) не освобождает fatal
или missing draws от admission. Причина и два исправления описаны выше;
старые failed reports остаются историческими отрицательными результатами.

Release `22b5620` / `82bc5f26` отклонён. Завершённые default/full/pressure
наблюдения не становятся результатами нового `321c8d4d`; прогресс его
отдельной матрицы приведён выше.

## Завершённый core и точный CPU-кэш patch

Предыдущий [renderer-v7 checkpoint](PS2_HARDWARE_OPT16_20261116.md) сохраняет
свои отдельные результаты. Он включает ленивые стабильные страницы texture
metadata, компактный 68-байтовый EE mipmap, ранний отказ прежнего conservative
visibility test, сокращённый scratch планировщика, try-only AP88 purge rescue
и восстановление VRAM allocation после первых 64 LRU-попыток. Диапазоны ID,
владение живыми записями и обязательные синхронизации сохранены.
[Metadata/lifecycle proof](PS2_TEXTURE_METADATA_PAGES_20261116.md),
[AP88 rescue](research/opt16/PATCH_RESCUE_20261116.md) и
[VRAM recovery](research/opt16/VRAM_RECOVERY_20261116.md) задают границы этих
изменений; результаты v7 не перенесены автоматически на будущий ELF.
Точный full-size PSMT4 и MMI upload packing сохраняют исходные texels;
их admission и прежние отклонённые стадии описаны в
[P4 checkpoint](research/opt16/P4_FULL_SIZE.md).

Обычный канонический AP88 patch теперь хранит tight plane индексов и coverage
битов: для реального `N = uw*uh` требуется `N + ceil(N/8)` байт. Логический
AP88/POT прямоугольник, colormap, alpha и выбор GS-формата сохранены. Виртуальная
область вне реального изображения воспроизводит прежний `0x00ff`; RGB
alpha-zero пикселей также точен. Нормальная загрузка не создаёт полный
промежуточный AP88-буфер. Уже закреплённые канонические колонки обслуживают
прочих потребителей и purge rescue.

Mode/fact bits занимают старый padding: основной mipmap остаётся **68 Б**,
driver record — **140 Б**. Models, raw flats, fade masks и неподходящие patch
сохраняют свои рецепты. Normal try/mandatory policy использует прежний
логический AP88 размер и порог 64 КиБ; rescue остаётся только try. Fast scan
хранит лишь доказанные факты исходных байтов, без palette-dependent RGB cache.

По **7306 sprite/character изображениям** сумма AP88 payload меняется
**265310986 → 72292822 Б: −72,75% по корпусу**. Это разные изображения,
не одновременно занятая RAM и не экономия VRAM или 72,75% общей памяти игры.
[Описание представления](research/opt17/SPRITE_CPU_PATCH_CACHE_20261116.md) и
[публичные component results](research/opt17/SPRITE_CPU_PATCH_RESULTS_20261116.json)
содержат 37329 построений по 12443 stock patch, 2716645484 сравнённых AP88
байта на профиль, 347600782 upload-байта, PNG/rotation/rescue lifecycle и
девять исполненных отрицательных контролей. Native timings относятся к
scan/fill-процедурам, а не к FPS игры.

При композиции новые локально принадлежащие канонические patch освобождаются
после последнего размещения. Повторные размещения, чужие cached sources,
предел временной таблицы 32 и прежние try-пороги сохранены. Это добавляет
128 Б временного EE stack payload без persistent allocation.
[Actual-function proof](research/opt17/COMPOSITE_LAST_USE_20261116.md) и
[результаты](research/opt17/COMPOSITE_LAST_USE_RESULTS_20261116.json) сравнивают
631152467 полных выходных байтов при одинаковых 16851 canonical builds.
Для SKY11 нормализованный component payload peak снижен на **76472 Б**;
это не замер игровой RAM и не доказательство admission MAP11.

## Четыре alpha/source-lifetime исправления

* Derived mip больше не публикует отсутствие index 255 как факт о полном
  каноническом источнике; keyed twin получает правильный CLUT.
* Band-copy debug-путь использует stride выбранного cached mip, устраняя
  неверные индексы и выход за границу.
* Удаление последнего canonical engine object удаляет его full/derived source
  cache entries; новый объект с тем же ID не использует старые пиксели.
* Временный lock исходника снимается после создания mip. Источник с queued
  DMA reference остаётся защищён до fence, затем снова допускает purge.

[Разбор](research/opt17/TRANSPARENCY_20261116.md),
[объединённый host proof](research/opt17/TRANSPARENCY_INTEGRATED_RESULTS_20261116.json)
и [actual EE/GS alpha/restart proof](research/opt17/TRANSPARENCY_NATIVE_RESULTS_20261116.json)
связывают исправления с uploaded index/RGBA и исполненными отрицательными
контролями. Generic shared-map `UpdateTexture` candidate остаётся отдельно:
его полный sibling CPU-data contract не принят.

Отдельный planner clock исключает повторное использование прежних plan/full
stamps после GS restart или смены logical size в том же framebuffer.
[Restart/high-res audit](research/opt17/HIGHRES_AND_RESTART_AUDIT_20261116.md)
отделяет это исправление состояния от недостаточной texture/capture capacity.

## Ресурсы: точные байты и опциональный PAK layout

Опциональный `--fill-gaps` уменьшает четыре stock PAK
**190103552 → 188952576 Б**, то есть на **1150976 Б (0,605%) на диске**.
Stored/decoded bytes, порядок, номера и content сохранены. Layout сам по себе
не сокращает RAM/VRAM. На synthetic directory-order scan locality ухудшается,
поэтому append-only остаётся default; общего ускорения загрузки не заявлено.
[PAK report](PS2_PAK_OPT17_20261116.md),
[machine-readable results](research/opt17/PAK_LAYOUT_RESULTS_20261116.json) и
[actual-reader verifier](../tools/ps2/pak_layout_verify.py) сохраняют
буквальное сравнение 401801745 decoded bytes и malformed/changed-payload controls.

[Resource stream proof](research/opt16/RESOURCE_STREAM_20261116.md) и
[TEXC proof](research/opt16/TEXC_STOCK_20261116.md) проверяют bounded
verification/prefetch и decode прямо в destination без прежнего stored-data
temporary. Metadata, stream window и optional prefetch самого TEXC всё ещё
занимают RAM. TEXC нужно пересоздать для выбранного набора PAK: даже lossless
layout меняет directory checksum и cache identity. Ineligible textures и
аддоны сохраняют runtime composition path.

Отдельные pre-tiles resource A/B на одном ELF дали по три точных кадра для
нового PAK и TEXC с prefetch 0. Это исторический scoped результат для ресурсов. Свежий same-final-ELF
resource A/B двух наборов и default/full image результаты приведены выше;
ни один из этих прогонов не охватывает все карты.

## CPU / GPU~ HUD: завершённый scoped admission

Показания ограничены **0–100%**. CPU — foreground EE activity за окно времени
за вычетом объединения явно наблюдаемых main-thread waits. Это не загрузка
всех EE threads. GPU~ — наблюдаемый submit-to-FINISH queue span за то же окно.
Он включает CPU feeding gaps, transfer/VU и задержку наблюдения FINISH:
это верхняя оценка queue activity, а не точный GS busy counter. При неполном
наблюдении показывается N/A. Процент не выводится из FPS, CPU или VRAM occupancy.

Off/On/RAM/VRAM сохраняют значения 0/1/2/3 и прежнюю девятитиковую cadence.
22192-байтовый workspace теперь optional и освобождается при Off; устойчивый
кадр не добавляет allocation, при неудаче остаётся direct text.
[Описание](research/opt17/ACTIVITY_HUD_20261116.md),
[528016 host checks](research/opt17/ACTIVITY_HUD_HOST_RESULTS_20261116.json) и
[native admission](research/opt17/ACTIVITY_HUD_NATIVE_RESULTS_20261116.json)
проверяют арифметику, waits/tokens/reset, форматирование, allocation failure
и семь полных PPM по режимам монитора с переходом Hardware 320×200 →
Software 640×400 → Hardware 320×200. Сохранён допустимый `showfps Full`;
финальный sample — CPU 100%, GPU~ 7%, valid=1. Визуально проверен зазор
между тремя строками и FPS в 320×200. Первый запуск с недопустимым `Yes`
остаётся историческим failed admission, сменённым успешным `Full`.

Это завершённый HUD admission на core до tiles. Он не закрывает **40**
унаследованных `selected while collecting` missing-draw diagnostics:
strict gate остаётся **FAIL без исключения**. Нативная точность процентов
на физической PS2 и общий FPS из этих семи кадров не следуют.

## Tile-V9: ограниченный borrowed-wide путь

[Публичное описание](research/opt17/HIGHRES_TILE_STREAMING_20261116.md) и
[source-bound ledger](research/opt17/HIGHRES_TILE_RESULTS_20261116.json)
сохраняют историю и component proofs. Oversized source представлен proxy;
полные logical dimensions и source bytes сохранены. CT32 captures имеют
EE backing и передаются частями, поэтому не требуют целого FB-sized объекта
в GS texture pool. Non-regenerable proxy удерживает прежний CPU source;
lease-aware release не принимает произвольный AP88 за гарантированно
восстановимый. Queued DMA lifetime защищается отдельно.

Основа V8 использует постоянный **16 КиБ GS scratch**: general indexed PSMT8
window 128×128 занимает тот же span, что direct-colour 64×64. Source-X map,
wrap/clamp, origin, halo, TBW и cache layout key согласованы. General draw
сохраняет исходные GS triangles/STQ/RGB/XY/low24 depth; grouped masks и
bounded box classification сокращают повторную CPU/GS работу. Captures и
screen sprites имеют отдельные правила. Исходная фильтрация, palette/alpha,
effect passes и порядок сохраняются; применение механизма само по себе
не означает ускорения игры.

Применённое расширение **V9 borrowed-wide** оставляет постоянную резервацию
**16 КиБ VRAM**. Оно может временно занять только соседние уже свободные
**16 КиБ**: без eviction и без второй постоянной резервации. Если обычный
free-range allocation не помещается, prefix сначала возвращается и allocation
повторяется — до failure counters, eviction victims и screen spilling.
При недоступности adjacent prefix сохраняется путь V8. Занятые image
allocations не вытесняются ради широкого окна. V8 measurements не выдаются
за результаты расширения. Wide shapes — **PSMT8 128×256** и **CT32 128×64**;
GS region 0 и CT16 framebuffer/depth policy сохранены.

High modes резервируют фактический scratch, когда полный capture не
помещается, а ordinary allocator/planner вычитает его один раз. Default 320
не резервирует tile до необходимости. Два CT32 EE captures по-прежнему могут
требовать **2457600 Б при 640×480** или **2621440 Б при 640×512**; retained
sources, capture overlap и временное borrowed window должны войти в новый
RAM/VRAM-замер. Постоянный mipmap/driver layout остаётся 68/140 Б.

Исторический [V9 host index](research/opt17/highres-host-borrowed-v9.json)
закрепляет прежние 20 исходников: 154070 allocator checks / 180224000 exact conversion bytes,
262417 sanitized queued-draw checks и 7949821 sanitized capture checks без
ошибок. Пять allocator negatives и stale-reference negative обнаружены.
Нового persistent BSS, context field или record byte нет; isolated generic
EE loader frame вырос 880 → 928 Б. Это component/stack evidence, не game RAM.

Исторический [V9 native index](research/opt17/highres-native-borrowed-v9.json)
сохраняет семь boots в стандартных 32 МиБ PCSX2. Borrow suite: **318 checks / 0 failures**,
1792000 полных RGBA/Zlow24 pixel comparisons точны. Проверены обе wide shapes,
occupied-prefix fallback и восемь queued reclaim/overwrite пар. Прежние
144 common-EE, 46 attribute и 40 coordinate checks точны; 257 window checks
проходят, два ожидаемых halo-negative FAIL обнаруживают испорченные sampler
guards. Но исходный default suite имеет
**один FAIL из 231**: один RGB channel одного пикселя отличается на **1** от
существующего fast reference. Допуск не изменён, strict gate не перекрашен
в pass. Capture/wipe exactness и нулевые coordinate/solid-coverage differences
в своих fixtures не заменяют этот результат или игровой gate. Первая borrow
fixture с 20 branch-assertion FAIL сохранена в индексе: boundary input реально
попадал в точный fallback; новый interior positive добавлен без смены source
или graphics tolerance.

Предварительный V15 MAP01 smoke даёт clear около **143–146 млн Count** и полный
warm frame около **153 млн**, близко к V14 (140/149 млн). Heat SPRITES около
326 млн Count также близок V14. Synthetic снижение packet/classifier работы
не доказывает реальный wide-path FPS gain. Отдельный V15 heat/wipe smoke
завершил 38 кадров трёх карт без наблюдавшихся fatal/fallback/audio errors,
с global peak 22469600 Б. Это предварительный scoped smoke, не результат
новой финальной матрицы. High-res остаётся тяжёлым: в этих сценах порядка
2 FPS без heat и около 0,5 FPS с heat; общего ускорения и shipping performance
admission нет.

## История и незакрытые gates

| Стадия / проверка | Сохранённый результат и граница |
| --- | --- |
| Early v2/v3 geometry | Отклонены; точный отдельный capture/wipe proof не исправляет geometry FAIL |
| final-r2, integrated v10 | PAL MAP01 изображение корректно, но примерно 8,6–9,5 с на кадр; прогон остановлен, performance не принят |
| Grouped/combined, measured V7, indexed V8 | Сокращают измеренную работу отдельных путей; V8 — основа V9, не итоговый game/FPS admission |
| Core-v9 default MAP02 до tiles | Исторический net available +1964880 Б; все пять ordered streams точны; два strict pressure нарушения: C-peak +15040 Б и C-free −13776 Б |
| Исторический `22b5620` default pressure | Net available +1920768 Б на MAP02/350f; пять streams точны; три strict errors `libcpeak`, `arena`, `libcfree` — **FAIL** |
| Исторический `22b5620` default missing draws | 400 diagnostic lines = 40 на boot × 10; все `selected while collecting`, десять strict errors; **FAIL** без исключения |
| Tile native fast reference | Один RGB channel / один pixel / delta 1; strict **FAIL** сохранён |
| Исторический `22b5620` switch-PAL/480p | Оба heap ownership fatal на gametic 141 после 11 captures каждый; **FAIL** |
| Исторический release ELF `82bc5f26` | **REJECTED**; исходные failed reports сохранены |
| [Исторический стандартный GS suite](research/opt17/standard-gs-reference.json), VU0 | **FAIL:** CT32 55/56, CT16 50/51; max dX=1, dY=1, dZ=86, relative 1/w error 3.61343e−5; допуск не изменён |
| Исторический core-v9 MAP11, реальный guard/OOM | Был PU_PATCH OOM 19504 Б; это история, отдельно от текущего PU_RENDERWORK OOM 17544 Б |
| Новый `321c8d4d` / ELF `7bf67e21` | Все 12 стадий и дополнительные resource/MAP11 наблюдения завершены; strict red gates сохранены |
| Prototype combined fixes / ELF `9c6a9324` | PAL и 480p по 175f/12 PPM без heap fatal; оба missing-draw strict **FAIL** сохранены |
| Текущий MAP11 / общий итог | Ordinary heuristic fallback на frame 0; diagnostic actual OOM 17544 Б PU_RENDERWORK на frame 1; полный strict admission **FAIL**, общий FPS gain/physical PS2 execution не установлены |

Исторические core-v9 +1964880 Б относятся к состоянию до tiles; arena used/peak
были ниже на 1974560 Б, arena больше на 4096 Б. Gameplay evictions не наблюдались,
startup evicted bytes выросли на 16 Б. Эти величины не являются итоговыми
V9 RAM numbers. Spill входит в zone accounting и не прибавляется повторно.

В историческом **core-v9 MAP11 до финальных tiles/last-use изменений**
зафиксирован recoverable PU_PATCH request **19504 Б**, total zone free
24304 Б, largest hole 7392 Б при arena 23552000 Б и peak 23540032 Б.
Нужен как минимум 19520-байтовый непрерывный блок до alignment; наличия общей
свободной памяти недостаточно. Последовал guard/fallback. Last-use compositor
proof даёт отдельную component-экономию, но не доказывает, что полный MAP11
frame или последующий working set помещается. Last-use release мог изменить
реальную fragmentation; старый OOM не выдаётся за результат нового source.
Свежие ordinary-guard и diagnostic-admission прогоны завершены и описаны
выше: текущий MAP11 тоже переходит в Software, с новым actual PU_RENDERWORK
OOM в diagnostic admission. Actual OOM guards остаются включены; source и
production policy ради прогона не менялись. Geometry, контент, физика
и лимиты не сокращены.

[Итоговый journal](research/opt17/final-321c/validation-journal.json)
закрепляет все **12 завершённых стадий**: обе исходные PAL/480p switch chains,
default/full five-scene A/B, 350f MAP02 pressure, все PAL/480p
heat/wipe/heat-wipe scenarios и корректный canonical GS. Дополнительные
resource-native и оба MAP11 наблюдения также завершены. **Завершение проверок
не означает strict PASS**: default/high-res missing draws, три pressure
нарушения, native fast-reference pixel и VU0 numerical failure сохранены.
Graphics tolerance и strict gates не менялись; actual OOM recovery
оставался armed.

[Original matrix с incomplete GS](research/opt17/final-321c/original-matrix-with-incomplete-GS.json)
и [post-matrix journal](research/opt17/final-321c/post-matrix-journal.json)
сохраняют историю первого неполного harness и последующего canonical rerun.
Production source и shipping ELF между этими наблюдениями не менялись.
Это итог измеренного раунда с частичными границами: high-res sky/heat остаётся
тяжёлым, MAP11 использует Software recovery, общего FPS gain, all-map
Hardware residency и проверки на физической PS2 нет. Время полного кадра
отделено от native helper Count; MAP02 RAM не переносится на весь working set.

В этом документе ссылки на proof ведут к публичным отчётам и воспроизводимым
tools; локальные build snapshots не используются как замена опубликованному
report. Артефакт `7bf67e21` относится к текущему source `321c8d4d`; прежний
`82bc5f26` остаётся отклонённой историей.

# Продолжение D4 зоны — 2026-10-02

**Allocator host regression: PASS. G1 целиком не закрыт.** Проверен фактический
`src/z_zone.c` + `src/ps2/ps2_mem.c` на x86, с шириной указателя/size_t 4 Б.
EE integrated build, PCSX2, gameplay/idle и real-hardware cache/DMA остаются
за координатором. Здесь выполнена только EE syntax-проверка двух owned единиц.

## Изменения в пределах владения

Сохранена существующая незакоммиченная D4 реализация: одна `memalign(64)`
арена, segregated free lists с bitmap, footer/boundary tags, O(1) coalescing;
постоянные теги выделяются с верхней стороны выбранного свободного блока,
level/cache — с нижней. Это стороны **выбранного free block**, а не два
изолированных фиксированных раздела. Учёт `Z_TotalUsage`/`Z_TagUsage` теперь
считает реальные занятые байты арены (header + payload + padding/guards).

Доработаны только `src/z_zone.c,h`, `src/ps2/ps2_mem.c,h`,
`tools/ps2/zone_hosttest.c,py` и этот отчёт. Существующие изменения
`src/z_zone.h` сохранены, уточнён root-only контракт `Z_Touch`.
`d_main.c` изучен, не редактировался.

* Воспроизведён дефект: age 200 проигрывал age 100 из-за объединения всех
  возрастов >=63 в один bucket. Исправлено точное LRU по 24-битному stamp:
  overflow bucket уточняется четырьмя six-bit radix scans, с повторным
  использованием 64-element histogram, без malloc/сортировки/per-victim scans.
  Equal-frame ties разрешаются физическим порядком. Проверено независимым
  descending sorted oracle на 128 блоках разных размеров/возрастов.
* При ownerless allocation с тегом >=100 ошибка теперь выдаётся **до**
  выделения/вытеснения: нет утечки и изменений учёта на invalid request.
  PS2 `Z_SetUser(ptr, NULL)` для non-purgable блока больше не пишет по NULL.
  PC-ветка оставлена исходной; при retarget старый owner slot не разыменовывается
  (он может принадлежать уже заменённой таблице владельцев).
* После exact-LRU прежний `za_prefer=1` не прошёл сравнительный fragmentation
  критерий. Измерены 2/8/16 кандидатов; 16 прошёл release/ZDEBUG/x64, выставлен
  default 16. Поиск сравнивает адреса не более 16 **подходящих** кандидатов
  в выбранном size bin; не подходящие по alignment узлы могут просматриваться
  дополнительно. На EE скорость этой политики ещё не измерена.
* Добавлены проверки current-frame headroom с настоящим `Z_NextFrame`,
  rollover, realloc source pin при purge/OOM, owner retarget/detach, стоимости
  headroom по возрасту, nested-lock/interior texture lifetime. Исправлены
  устаревший mutation anchor и fixture, в котором fallback frame advance
  маскировал current-frame случай. Optional x64 coalescing fixture задаёт
  alignbits=4 явно: `Z_Malloc` на x64 использует sizeof(pointer)=8 как
  alignbits, то есть 256 Б, и создавал дополнительные alignment holes.
* Negative controls засчитываются лишь при exit 1/2 и сообщении `FAIL:` /
  `Unexpected I_Error:`. Crash/ошибка компиляции/выжившая мутация — провал.
  `--trace-only` и `--prefer 1..16` позволяют воспроизводить policy experiments;
  неизвестный `--only` теперь отвергается вместо запуска нуля тестов.

## Header и отличие от ADR

| ABI / режим | Измеренный sizeof(header) | Сравнение с ADR D4 |
|---|---:|---|
| x86 release, pointer=4 | **16 Б** | совпадает с 16 Б ADR |
| x86 ZDEBUG, pointer=4 | **32 Б** | +16 Б: owner file:line и padding |
| x64 release, pointer=8 | **32 Б** | portability-only, не EE ABI |

Для 32-bit ABI добавлены compile-time asserts 16/32 Б. EE GCC проходит оба.
Payload >=16 Б; начиная с request size 2048 Б >=64 Б, больший alignbits
соблюдается. Host test проверяет bits 0..16 (до 65536 Б), границу
2047/2048, обе стороны, alignbits <0/>=32 и size overflow. 32-Б release header
из исторического G1/PS2-08/PS2-14 относится к **старой malloc-зоне**, не D4.

Default reserve libc = 2 МиБ, headroom = 4 МиБ, eviction slack = 256 КиБ;
`-zarena`, `-zreserve`, `-zheadroom` задаются в КиБ. Red zones включены по
умолчанию в ZDEBUG, в release по `-zredzone`. Headroom — best effort:
non-evictable блоки могут сделать его недостижимым. Под lock allocator тогда
отказывает с OOM, не освобождая живые cache pointers. Явные `Z_Free`/`Z_FreeTags`
не являются automatic eviction и не запрещены lock.

## Запуски и результаты

Все каталоги этой работы имеют префикс `build/continue-zone*`.
Основной завершённый прогон (exit 0):

```powershell
python tools/ps2/zone_hosttest.py --out build/continue-zone-complete --negative-controls --identical --x64
$env:SRB2_PS2_OUT='D:/Ai-Project3/SRB2-PS2-Port/build/continue-zone-ee-syntax'
python tools/ps2/build.py --syntax --jobs 1 src/z_zone.c src/ps2/ps2_mem.c
$env:SRB2_PS2_OUT='D:/Ai-Project3/SRB2-PS2-Port/build/continue-zone-ee-syntax-debug'
python tools/ps2/build.py --syntax --zdebug --jobs 1 src/z_zone.c src/ps2/ps2_mem.c
```

| Проверка | Actual output / результат | Лог |
|---|---|---|
| x86 release | `PASS all 1389562952 checks`, exit 0 | `build/continue-zone-complete/ps2-x86/test.log` |
| x86 ZDEBUG | `PASS all 1400780324 checks`, exit 0 | `build/continue-zone-complete/ps2-x86-zdebug/test.log` |
| host PS2_PROFILE no-op | `PASS all 2 checks`, exit 0 | `build/continue-zone-complete/host-profile/test.log` |
| optional x64 release | `PASS all 1400943198 checks`, exit 0 | `build/continue-zone-complete/ps2-x64/test.log` |
| EE syntax release / ZDEBUG | `compiled 2/2 files`, `failed 0`; diagnostics пусты | `build/continue-zone-ee-syntax*/build.log` |
| MSVC C17 | `/O2 /W4 /WX /DPARANOIA`, builds успешны без warnings | соответствующие `build.log` |

Каждый PS2 full suite: 20000 alloc/free, 200 explicit realloc cycles;
40 coalescing rounds (ascending/descending/38 shuffled), hole reuse;
6 configs × 2 seeds × 60000 randomized top-level ops (**720000**), плюс
40 операций внутри каждого lock scope. Model независимо проверяет
accounting, no overlap, alignment, contents, owner clearing и eviction legality.

Release torture actual output:

```text
680266 alloc 221645 free 154481 realloc 42882 retag 9736 setuser
100135 touch 29082 frames 4380 level exits 4328 purges 12789 iterate
14229 lock scopes; 84861 evictions, 9059 OOM refusals
```

ZDEBUG torture: **682310 alloc, 222109 free, 156231 realloc,
86933 evictions, 9048 OOM refusals, 14349 lock scopes**.
Каждый завершённый сценарий возвращает арену в один free block, used=0.
Red-zone overflow, last guard byte, free-time check, повреждённый header
и неправильный owner обнаруживаются. OOM log содержит request/tag/alignment,
lock/frame, usage по тегам и arena health.

Non-PS2 preprocessed source = upstream pin
`0e09462308610005f640ed84b21ec8a4ef116a4b` в четырёх вариантах:
plain **2921**, ZDEBUG **2954**, ZDEBUG+PARANOIA **2960**,
без Valgrind + HWRENDER **91295** строк — **IDENTICAL**.
Нормализованы пустые строки/trailing whitespace и ZDEBUG call-site file:line.
Это проверка исходной PC-ветки allocator, не PC gameplay regression.

### Negative controls: 19/19 обнаружены

В `build/continue-zone-complete/negative-*/test.log`:
next/prev coalescing, alignment, accounting drift, undersized block,
red-zone check, current-frame eviction, collapsed long-age LRU,
realloc source purge, locked eviction, owner clearing, ignored touch,
iterator callback, accepted alignbits 32, total-only headroom,
current-frame headroom, age-cost headroom, missing headroom и purgable-first.
У каждой compile PASS и ожидаемый exit 1/2; crashes не засчитаны.

### Fragmentation: actual release numbers

14 levels × 240 frames × 3 arena sizes (10/11/12 МиБ) × 8 seeds;
base skew 0/16/32/48 КиБ, отдельно startup-only и lazy long-lived patterns.
Это synthetic stress, не MAP11. OOM намеренно допускается маленькими аренами.

| Pattern / policy | Mean frag | Mean largest free на exit | Cache misses | OOM refusals | Free nodes scanned / alloc |
|---|---:|---:|---:|---:|---:|
| startup-only, one-sided first-fit | 46% | 1369 КиБ | 33094 | 13 | 1.01 |
| startup-only, two-sided default16 | 45% | 1369 КиБ | 29634 | 0 | 1.21 |
| lazy long-lived, one-sided first-fit | 56% | 1152 КиБ | 69088 | 477 | 1.01 |
| lazy long-lived, two-sided default16 | 55% | 1180 КиБ | 65190 | 440 | 1.29 |

Lazy ZDEBUG: one-sided **76691 misses / 420 OOM / 1.01 scans**;
default16 **69544 / 175 / 1.24**. Optional x64: **115432 / 1442 / 1.09**
против **107426 / 977 / 1.38**. Header/caller alignment различаются по ABI.
Scans — метрика free-list lookup, не полный CPU cost (не включает LRU walks).

Сохранённые RED experiments:
`continue-zone-long-age-red` доказывает long-age LRU ошибку;
`continue-zone-regression` — провал старого first-fit после exact LRU;
`continue-zone-prefer2`, `continue-zone-prefer8` — провалы сравнительного
критерия; `continue-zone-prefer16` — прошедший trace-only experiment.
`continue-zone-final-x86` обнаружил выживший current-frame headroom mutant
до исправления fixture; `continue-zone-verified` и итоговый
`continue-zone-complete` обнаруживают его.
`continue-zone-x64` — исходный неверный hole-count fixture; исправление
проверено в `continue-zone-x64-fixture`. Ранние RED не засчитываются как PASS.

## Lifetime contract, integration blockers для координатора

1. `d_main.c:D_RunFrame` уже вызывает `Z_NextFrame()` после `D_Display()`
   под `PS2_PROFILE`. `r_main.c:R_RenderPlayerView` охватывает setup/BSP/
   planes/masked purge lock. Правки frame hook по итогам этих тестов не нужны.
   Fallback advance на outer lock действует только до первого explicit
   `Z_NextFrame`; это не доказательство фактического числа кадров в EE.
2. **Нужны cache-hit touches вне владения:** `r_textures.c:R_CheckTextureCache`,
   `R_GetColumn`, early-return cached `R_GetFlatForTexture` сейчас не вызывают
   `Z_Touch` для root allocation. Следует stamp **root**, не interior column/
   post/pixel pointer, под `#ifdef PS2` либо profile no-op API. Без этого LRU
   отражает build/retag, но не все render cache hits. Это blocker заявления
   о полной реализации frame-LRU на реальных texture accesses.
3. Allocator очищает **зарегистрированный owner root**, но не произвольные
   interior aliases. `texturecolumns[]`, `column->posts/pixels` нельзя
   разыменовывать после root=NULL; они перестраиваются целиком. Проверен
   representative fixture с actual allocator: aged root + column/post/pixel
   aliases остаются целы под nested lock и allocation/OOM pressure; inner
   unlock оставляет защиту; после outer unlock eviction очищает root,
   aliases отбрасываются и восстанавливаются от нового root. Это **не**
   запуск actual renderer с eviction во всех сценах. В `R_GetColumn`
   profile root-check/rebuild уже есть; caller обязан не удерживать alias
   через outer lock preflight, который вправе вытеснять current-frame cache.
   Registered owner slot должен жить дольше блока или быть заранее retargeted.
4. Нет integrated EE link/title/600s idle/gameplay/content/MAP11 stress,
   аппаратного DMA/cache и COP0 cost для exact-LRU/default16. Проверки heap
   sizing используют host-stub capacity; actual EE reserve/headroom и OOM
   diagnostics на целевых картах должен измерить координатор. G2 не открыт.

## Candidate detail для PS2-21 (реестр здесь не редактировался)

> Только PS2 (`z_zone.c,h`, `src/ps2/ps2_mem.*`, frame hook в `d_main.c`):
> D4 arena вместо временной per-block malloc-зоны PS2-08/PS2-14. Release
> header 16 Б (ADR D4), ZDEBUG 32 Б; payload 16/64 Б и запрошенный alignbits.
> Segregated free lists/bitmap + boundary-tag coalescing, top/bottom allocation
> policy с bounded 16-candidate address preference. Usage — реальные arena
> bytes, payload отдельно в ps2_mem. PU_CACHE с owner вытесняется exact LRU
> по 24-bit frame stamp; current frame/realloc source защищены. >=100 tags
> purged first; nested render lock делает best-effort contiguous headroom
> до захвата и запрещает automatic eviction внутри, при нехватке — OOM.
> libc reserve 2 МиБ, headroom 4 МиБ, slack 256 КиБ; ps2_mem/tag/owner/peak/
> fragmentation/OOM reports и optional red zones. Actual x86 release
> 1389562952 checks, ZDEBUG 1400780324, 19/19 negative controls; PC allocator
> preprocessing identical upstream. Cache-hit touch integration и integrated
> EE/PCSX2 проверка остаются открытыми — пока не объявлять D4/G1 закрытым.

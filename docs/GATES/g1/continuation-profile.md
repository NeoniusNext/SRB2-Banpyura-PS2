# Continuation: профиль PNG/zlib/ZIP/UDMF/addons

Дата: 2026-10-02. Workspace `D:/Ai-Project3/SRB2-PS2-Port`, ветка `ps2`.

**Результат: проверки паков и моей зоны PASS; общий профиль остаётся FAIL.**
Source audit действительно возвращает **exit 1**: вне ownership найдены
**29** запрещённых определений/регистраций/startup-входов из проверяемого списка.
Это не полный ELF verdict и не закрытие G1. Полная EE сборка, host gameplay
regression и PCSX2 integration в этой сессии не запускались.

## 1. Исходное состояние и изменения

Сначала прочитаны `docs/AGENT_BRIEF.md`, `docs/PACK_FORMAT.md`,
`docs/GATES/G1.md`, текущие исходники, инструменты и их незакоммиченный diff.
На старте уже существовали profile guards, cooked-picture decoder, PNG cooker,
sidecar-aware verifier/reader test и `build/pak-a`. Они исследованы и продолжены.
`git status --short`, `git branch --show-current`, `git diff --stat` подтвердили
общее незакоммиченное рабочее дерево и ветку `ps2`.

Правки этой сессии:

* `src/netcode/d_netcmd.c`: под `!PS2_PROFILE` убраны объявления,
  регистрации и тела `runsoc`, remote-password/login/promote/demote/motd,
  serverchangeteam/mute/unmute/clearscores и соответствующих обработчиков.
  Убраны profile-регистрации `downloads`, `ping`, Lua net commands и вызов
  `AddMServCommands`. `D_SetPassword`/`D_ClearPassword` — пустые compatibility
  APIs на профиле, т.к. внешние callers ещё существуют.
* `src/p_setup.c/.h`: нет `P_LoadAddon`, `FindFolder`, local/folder runtime
  resource replacement. `P_AddWadFile` возвращает false: существующий caller
  `g_demo.c` сохраняет линковку, но replay не может загрузить add-on.
  `P_RunSOC` продолжает грузить **встроенные map SOC lumps**, а external `.soc`
  возвращает false. Исправлен неверный комментарий: TEXTMAP всё ещё хранится
  в паке; загрузка UDMF отвергается через `I_Error`, parser/writer отсутствуют.
* `src/m_menu.c`: profile guards исключают сами multiplayer/server-browser,
  rejoin/room/IP/start-server меню, данные и обработчики, а не только строку
  главного меню. Server options/monitor-toggle submenu также исключены.
  Player setup, single-player menu и выбор Sonic+Tails сохранены.
  `M_SortServerList` — пустой API для оставшихся внешних callers/cvar.
  Убраны лишние tentative server-menu definitions и неиспользуемая
  `serverlistpage`, обнаруженные именно EE object compilation.
* `src/m_misc.c`: server-history APIs не выполняют I/O на профиле;
  совместимые пустые функции сохранены для внешних callers. PNG screenshots
  по уже существующему guard не компилируются; software screenshot остаётся PCX.
* `src/w_wad.c`: `W_InitFile` отвергает `local` и `!startup` до открытия файла.
  Не запрещает `!mainfile`: **`mainwads=3` не включает стартовый MUSIC.PAK**.
  `W_ReadLumpHeaderPwad` на профиле использует только `WPack_ReadLump`;
  folder/file/LZF/DEFLATE fallback целиком под `#else`. Диапазон чтения
  ограничен через `size > lump_size - offset`, без сложения с overflow.
* `tools/ps2/cook.py`: палитра берётся независимо от `--only` (PNG в другом
  архиве больше не приводит к unbound PLAYPAL), проверяются `--only`/`--jobs`,
  obsolete sidecar удаляется при повторном cooking без PNG conversion;
  `--log` сохраняет реальные cooker statistics.
* `tools/ps2/verify_pack.py`: `--require-cooked`, `--log`, before/after SHA256;
  sector layout/reserved bits/empty-entry metadata; duplicates/out-of-range
  picture indices; имена, размеры, SHA256, CRC и offsets/dimensions sidecar;
  запрет raw PNG и пропущенных conversion records для всех четырёх паков.
* `tools/ps2/test_pack_reader.py`: sidecar включён в before/after input hashes.
* `tools/ps2/strip_check.py`: expanded forbidden list для remote/admin/addon/menu
  символов; nm/size failure или пустой nm output теперь завершают проверку ошибкой.
* Новые `tools/ps2/strip_source_check.py`, `strip_pack_test.py`: воспроизводимые
  object/preprocessing audits и cooked-sidecar/cooker негативные контроли.

Существующие изменения `filesrch.*`, `r_picformats.*`, остальных pack/picture
tools сохранены и проверены. Уже присутствовавшие HW renderer-name изменения
в `m_menu.c` не редактировались. Файлы вне ownership не редактировались.
Индекс Git не менялся; завершающий `git diff --cached --stat` пуст.
Все новые outputs находятся в `build/continue-profile*`; `build/pak-a`,
toolchain, assets и golden использованы только для чтения.

## 2. Запущенные команды и результаты

Все команды выполнялись из корня workspace. Python — с `-B`.

### Оригинальный PNG decoder против профильного cooked decoder

```powershell
python -B tools/ps2/strip_pics_test.py --out build/continue-profile-pics --pak build/pak-a --src srb2-assets --negative-controls
```

**Exit 0.** `build/continue-profile-pics/test.log`:

```text
15 PNG lumps in the four pk3 (pk3 order)
original decoder: 0 of 15 images change when the conversion order is reversed, 0 change with an empty memo
independent Python decoder model vs original decoder: 0 differing images of 15
lumps in build/pak-a: 15 compared with the encoder output, 0 differences
cooked pictures run through the engine functions vs the original decoder: 15 images, 1458176 pixels (0 transparent), offsets+matrix+flat byte compare -> 0 differing files
14 synthetic patches (tall, holey, sparse, up to 2048 high): Python round trip and engine decode -> 0 failures
negative control pixel.lmp: comparison detects it
negative control offset.lmp: comparison detects it
negative control top.lmp: comparison detects it
negative control raw PNG into the profile path: exit 2 (rejected)
TOTAL differences/failures: 0
```

Oracle — реальный `src/r_picformats.c`, HAVE_PNG + libpng/zlib, **без**
PS2_PROFILE. Profile check — тот же файл с PS2_PROFILE, **без** libpng/zlib.
Host stubs включают дословные slices NearestPaletteColor, InitColorLUT/GetColorLUT
и Patch_Create* из движка. Оба MSVC build logs не содержат diagnostics:
`oracle-build/strip_png2pic.build.log`, `check-build/strip_cooked_check.build.log`.
Их warning policy — существующая `/W3` с выбранными legacy warnings disabled;
это не заявление о чистой полной native build.

У реальных 15 PNG все offsets равны 0 и нет прозрачных пикселей. Поэтому
проверка ненулевых offsets и прозрачности опирается дополнительно на 14
synthetic tall/holey/sparse fixtures и offset/top негативные контроли.
RGB565 memo order sensitivity не обнаружена именно на этих 15 изображениях.

### Проверка существующего `build/pak-a`

```powershell
python -B tools/ps2/verify_pack.py --src srb2-assets --pak build/pak-a --require-cooked --log build/continue-profile-verify/test.log
python -B tools/ps2/test_pack_reader.py --pak build/pak-a --src srb2-assets --out build/continue-profile-reader-final
```

Обе команды **exit 0**. Результаты по паку:

| Пак | Записей | Raw | LZ4 | Partial reads C reader | Отличий / сбоев |
|---|---:|---:|---:|---:|---:|
| SRB2.PAK | 12614 | 1162 | 11452 | 252200 | 0 |
| ZONES.PAK | 169 | 0 | 169 | 3380 | 0 |
| CHARS.PAK | 2054 | 9 | 2045 | 41060 | 0 |
| MUSIC.PAK | 215 | 109 | 106 | 4300 | 0 |
| **Всего** | **15052** | **1280** | **13772** | **300940** | **0** |

Python verifier сверяет SHA256 каждой декодированной лампы; для converted PNG
дополнительно independent Python model сверяет pixels/transparency/offsets.
C test собирает реальный `src/w_pack.c` с `/DPS2_PROFILE /W3 /WX`, проверяет
full/partial/unaligned reads и red zones. Его вывод:

```text
Validation: 21 rejection cases, 2 independently CRC-checked baselines, 0 failures
Input preservation: 9 packs/archives/sidecars hashed before and after, 0 changed
TOTAL differences/failures: 0
```

Два baselines — synthetic 2- и 65-raw-block LZ4 fixtures, каждый с 20
additional partial reads. Итого с fixtures — **300980** partial reads;
таблица выше относится только к четырём настоящим пакам.
Логи и полные TSV: `build/continue-profile-reader-final/`.
Первичный запуск reader до расширения hashing сохранён отдельно в
`build/continue-profile-reader/` (8 inputs, также 0 changed/0 failures).

### Cooker воспроизводит уже готовый пак

```powershell
python -B tools/ps2/cook.py --src srb2-assets --out build/continue-profile-cook --tool-dir build/continue-profile-cook-tool --jobs 2 --log build/continue-profile-cook/cook.log
python -B tools/ps2/verify_pack.py --src srb2-assets --pak build/continue-profile-cook --require-cooked --log build/continue-profile-cook/verify.log
```

Обе команды **exit 0**. Cooker конвертировал 15 PNG реальным оригинальным
decoder. PNG left = **0** во всех четырёх паках. Encode: SRB2 **21.2 s**,
ZONES **6.0 s**, CHARS **0.4 s**, MUSIC **0.7 s**.
Source archives **170302652 B** → packs **190967808 B**.
Verifier: **15052** записей, **0** discrepancies, **9** inputs, **0** changed.
Размеры и SHA256 scratch outputs совпадают с исходным `build/pak-a`:

| Файл | Байт | SHA256 (оба каталога) |
|---|---:|---|
| SRB2.PAK | 63088640 | `62430d948a59cde0901269e7e857b585707b9c0f2f76ee7f8cb2a223199fb55f` |
| ZONES.PAK | 24444928 | `41a889d6e70c3e95a60cf36de565b91504429fdb8e3f5cd4deea9086357d65fc` |
| CHARS.PAK | 2820096 | `8fb14a91f1cb168912ea3f60a2b0ceef9cbf080b8136d576350bf93b7c67e637` |
| MUSIC.PAK | 100614144 | `4afe8c73020371f7995e92c71f3fdc023bd54919c36f1e50215abf317ddb00f8` |
| SRB2.PAK.pics.json | 6899 | `71a0c738b0ef3006ac2ae963b86059ee777edece8970c6d90117f7dbfbe55f96` |

Source SHA256 (одинаковы во всех input preservation logs):

```text
srb2.pk3       0c5025b70dbd83faea86e59fbe888250ea70d799fa3b8c2f82535d180a3ef5b5
zones.pk3      2bc641793f339d2122ae97b71d23859d3802b726aea9dc9177c13ead3a95aae2
characters.pk3 f787fac81736ddaf168472f44ef78be26f63a475b95723b8cc39bdaa5435222a
music.pk3      42b481e04946089336a98ae25d8b6aee8007ec88f1b2fc9cd8315967b461b96f
```

### Новые контроли кукера/sidecar

```powershell
python -B tools/ps2/strip_pack_test.py --out build/continue-profile-pack-controls
```

**Exit 0.** Disposable two-entry `zones.pk3` содержит настоящий PNG из assets;
`srb2.pk3` fixture содержит PLAYPAL. Это проверяет conversion subset без PNG в
srb2 и без изменения оригинальных архивов. Оригинальный decoder конвертировал
**1** PNG, verifier дал **0** discrepancies. Отвергнуты **8** случаев:
duplicate index, out-of-range index, wrong name, cooked CRC, PNG CRC, offsets,
missing sidecar, raw PNG в require-cooked. Повторное cooking того же disposable
output без conversion удалило stale sidecar. Legacy raw-PNG comparison проходит.
`build/continue-profile-pack-controls/test.log`: **TOTAL failures: 0**.

### EE syntax и объектные/source guards

```powershell
$env:SRB2_PS2_OUT='D:/Ai-Project3/SRB2-PS2-Port/build/continue-profile-syntax-release'
python -B tools/ps2/build.py --syntax --jobs 1 src/filesrch.c src/m_menu.c src/m_misc.c src/netcode/d_netcmd.c src/p_setup.c src/r_picformats.c src/w_wad.c
python -B tools/ps2/strip_source_check.py --out build/continue-profile-audit-handoff --legacy-library-defines
```

Syntax **exit 0**: `compiled 7/7 files in 1.4s, failed 0`;
`build/continue-profile-syntax-release/build.log` — **0 B**.

Финальный audit **exit 1 из-за внешних блокеров**:

```text
SUMMARY owned=0, external=29, diagnostics=0, nonprofile_diff=0, rebound_equal_HEAD=True, changed_inputs=0
```

Каждый audit последовательно компилирует **только 7 owned units**, без линка.
Проверяются actual defined symbols, unresolved forbidden references и bodies/
command registrations в preprocessed sources. Ещё **7 external units**
препроцессируются read-only. Финальный запуск принудительно добавляет
`HAVE_PNG/HAVE_ZLIB`: собственные guards также проходят, что важно для legacy
host CMake definitions. Все **8** compatibility stubs проверены на точное пустое
тело/`return false`; это не исключение из списка за счёт linker GC.

Ранний audit без дополнительных library defines сохранён в
`build/continue-profile-audit-release`: owned=0, diagnostics=0,
nonprofile_diff=0; его более короткий forbidden list обнаружил 19 external
findings. После review D_ClientServerInit список дополнен HTTP-login
registration/helpers, ban helpers и `gamestate.c`: финальные 29 = **14 definitions +
10 registrations + 5 startup conditions**.

Исправлена также изоляция самого audit: GCC `-MMD -E` первоначально оставлял
default dependency files в cwd. Собственные 13 generated `.d` удалены;
теперь compiler cwd = isolated output и каждому вызову задан явный `-MF`.
Финальный запуск не создаёт artifacts в корне workspace.

Non-profile preprocessing семи units с HAVE_PNG/HAVE_ZLIB и NOHW сопоставлен
с HEAD versions owned source/headers при одинаковых прочих includes:
**7/7 equal** после нормализации diagnostic `__FILE__/__LINE__` и сохранения
идентичности autogenerated Tag-loop `ICNT_*` identifiers. Это проверка
исключения profile changes из PC software code, не native executable byte
comparison и не проверка HWRENDER branches.

`src/netcode/d_net.c` совпадает с HEAD целиком после CRLF/LF normalization:
rebound send/receive, node-0 handling, fatal nonzero-node contract не менялись.
В `d_netcmd.c` сохранены loopback map/name/weapon/pause/suicide/randomseed и
generic gameplay handlers. Runtime Sonic+Tails regression предстоит координатору.

Полные команды компилятора, fingerprints **18** source/header inputs,
preprocessed `.i`, object `.o`, defined/undefined `.nm`, stderr, PC diffs и
`report.json` находятся в соответствующих audit directories.

`git diff --check --` по 14 tracked owned files выполнен: **exit 0**,
whitespace errors нет (Git печатает только LF→CRLF working-copy notices).

## 3. Известные blockers и необходимые правки вне ownership

1. **`src/netcode/d_clisrv.c`**: D_ClientServerInit ещё регистрирует
   `kick`, `ban`, `banip`, `clearbans`, `showbanlist`, `reloadbans`, `connect`,
   `set_http_login`, `list_http_logins`, `resendgamestate`; ещё определены
   `Command_set_http_login`/`Command_list_http_logins`.
   Нужны PS2_PROFILE guards на эти регистрации и review remote-only
   connection/login/ban setup. Нельзя выключать D_ClientServerInit целиком:
   single-player server/rebound и AddPlayer используются локальным игроком/ботом.
   Реализация `Command_ResendGamestate` находится в **`src/netcode/gamestate.c`**:
   её также нужно guard-ить, сохранив общие gameplay save/state routines.
2. **`src/netcode/commands.c`**: ещё определены семь соответствующих
   `Command_*` handlers и `Ban_Add`, `Ban_Clear`, `Ban_Load_File`, `D_SaveBan`.
   Guard должен удалить их реализации и remote-only
   ban/connection helpers; review callers из d_clisrv/client_connection/SDL
   перед удалением exports. В `Command_connect` отсутствующий network driver
   лишь пишет alert, после чего код всё равно доходит до сброса
   `botingame`/`botskin` и `CL_ConnectToServer` — текущий fallback неприемлем.
   Не трогать rebound или Tails ради исключения этих UI/console входов.
3. **`src/d_main.c`**: source audit обнаружил `-password`, `-server`,
   `-connect`, `-dedicated`, URL-connect condition. Кроме этого, старт ещё
   создаёт addons directory и вызывает joined-IP APIs (теперь пустые).
   Нужны guards/review profile startup options и URL auto-connect. Существующие
   guards `-file/-folder` и startup add-on list уже присутствуют.
   **29 — число обнаружений данного списка, не исчерпывающий inventory сети.**
4. **`src/CMakeLists.txt` и общие build/run tools**: CMake при наличии PNG
   target добавляет HAVE_PNG, `apng.c` и PNG::PNG linkage; HAVE_ZLIB также
   включается автоматически. Для строгой host-profile сборки нужно исключить
   это под PS2PROFILE, сохранив обычную PC сборку. EE `build.py` уже содержит
   новые defs/libs без PNG/zlib в исходном общем diff; его не редактировал.
   Host regression надо запускать с **`--packs build/pak-a`**, а не default
   `build/pak` с прежними raw PNG. `run_host_profile.py` уже имеет `--packs`.
5. **Общие docs/DEVIATIONS/G1**: координатору уточнить PS2-20 по следующему
   разделу; обновить устаревшие утверждения PS2-10/13 и PACK_FORMAT о fallback,
   внешних addons и хранении UDMF. Сохраняются вложенные binary map WAD data,
   UDMF/MP/MIDI data и Ogg, порядок/index/names; это ещё SRP2 scaffolding,
   не полный target cooked-content/I/O pipeline.
6. **Интеграция**: свежий полный EE ELF, его size/link diagnostics и
   `strip_check.py` по итоговому ELF пока отсутствуют. Нельзя использовать
   clean seven-object audit как доказательство отсутствия библиотек во всём ELF.
   Host four-demo comparison/PCSX2 title+idle после текущих changes не запускались.
   PS2REF наблюдательные paths сохраняются; текущие G1 численные значения
   из старого отчёта сюда не переносились как свежие результаты.

Команды для будущей координаторской проверки (здесь **не выполнялись**):

```powershell
python -B tools/ps2/strip_check.py <fresh-final.ELF> --log <integration-dir>/symbols.log
python -B tools/ps2/run_host_profile.py --exe <fresh-host-profile.exe> --packs build/pak-a --output build/continue-profile-integration-host --timeout 240
python -B tools/ps2/strip_run_ps2.py --run build/continue-profile-integration-title --elf <fresh-final.ELF> --pak build/pak-a --mode title --timeout 300
python -B tools/ps2/strip_run_ps2.py --run build/continue-profile-integration-idle --elf <fresh-final.ELF> --pak build/pak-a --mode idle --idle-seconds 600 --timeout 780
```

PCSX2 должен идти через общий `run_pcsx2.py` lock (wrapper использует его).
Статус D4/hot-double/HW не оценивался этим continuation.

## 4. Будущая запись PS2-20

`docs/DEVIATIONS.md` не редактировался. Там на момент чтения уже был
PS2-20 draft; новый ID не требуется. Предлагаемое содержание для его уточнения:

> **PS2-20 — только PS2_PROFILE, runtime pack/picture/content/entry-point
> ограничения.** Профиль грузит стартовые SRP2 raw/LZ4 packs, без runtime
> PNG/libpng/zlib/ZIP parser/folder/addon fallback. 15 PNG entries srb2.pk3
> заменены на marker `89 SRPIC 0D 0A` + little-endian tall Doom patch;
> conversion выполняет оригинальный engine PNG decoder на хосте с PLAYPAL и
> canonical pk3 order/RGB565 memo. Имена, wad/lump indices и порядок сохранены;
> pixels, прозрачность и grAb offsets представлены native patch. Runtime
> texture/patch callers пользуются cooked aliases, software screenshots PCX.
> TEXTMAP parser/export удалены, UDMF map load — I_Error; TEXTMAP и остальные
> неподдержанные data пока остаются в SRP2. Console/menu/addfile/addfolder/
> saveaddons/runsoc/remote-admin/browser/start-server paths исключены из
> owned units; внешние registrations/startup paths пока blocker. Встроенный
> map SOC и локальный rebound/server/bot gameplay сохранены; compatibility
> APIs пустые/false. Host picture oracle: 15 images, 1458176 pixels,
> 0 matrix/flat/offset differences, 14 synthetic fixtures, 4 negative controls.
> Pack: 15052 records, 300940 partial reads, 21 rejected corrupt inputs,
> 2 synthetic baselines, 0 failures; cooker reproduction 190967808 B,
> SHA256 4 packs+sidecar совпадает с build/pak-a; 8 new sidecar/raw-PNG
> rejection controls PASS. EE owned objects: 7/7, diagnostics 0; PC software
> preprocessing normalized equal 7/7. Общий source audit FAIL (29 external
> findings). Полный ELF/library-symbol audit и новые host/PCSX2 regression
> результаты координатор должен дописать после интеграции.

После внешних правок не оставлять формулировку «внешние paths пока blocker»
без повторного запуска audit и свежих evidence. До этого нельзя записывать
«полностью без remote/addons», «G1 закрыт» или новую ELF/idle/gameplay
эквивалентность как доказанный результат.

# OPT11, агент STAB (стабильность, память, регрессии): рабочий журнал

Сессия 2026-10-07, Linux-контейнер, ветка `worktree-agent-a50b97c2e18e4e8cd`. Реестр: `PS2-170..199`, `PS2-HW-140..159`.
Окружение: `build/out` (полная конфигурация, `SRB2_PS2_NO= SRB2_PS2_HW=1`, LTO), `build/runs/<имя>` — прогоны, `build/runs/sweep/<тег>` — sweep'ы.
Скрипты прогона лежат вне репозитория (scratchpad): обёртки над `tools/ps2/opt_run.py` / `chain_sweep.py`.

## 0. База на старте (HEAD a7998a5, ELF `build/elf-base.ELF`, 10 760 616 Б; проверено запуском)

* Арена 23 801 856 Б (23 244 КиБ) против 24 211 456 в отчёте S: слияние SW+HT+FX отъело ≈ 400 КБ (bss 2.49 МБ: `netcmds` 295 КБ, lwIP 197 КБ, `linkdrawlist` 172 КБ, `ffloor` 104 КБ, `cutbuf` 95 КБ, `H` 70 КБ, `ovq` 65 КБ, `noup_data` 65 КБ ... — около 0.6 МБ bss принадлежит только HW-рендереру и пропадает зря в software).
* **Software, 84 карты, цепочка (`chain_sweep.py --tag sw-base`, 545 с): 82 из 84.** Падают: **MAP11 (CEZ2)** — в цепочке после MAP01..10 `OOM 65536 B PU_RENDERWORK` (73 Б свободно); из чистой загрузки (`-warp MAP11 -zquit 35`) проходит, но свободно всего **175 520 Б**, самый большой блок 52 КБ, вытеснено 170 МБ за 35 кадров, текстура 1530 (256x2048) заменяется «одной колонкой» (`WARNING: R_GenerateTexture: no room for texture 1530`) — картинка деградирует; **MAPMG** (Match UDMF) — `OOM 4194304 B PU_RENDERWORK` (составная текстура CLUDSSSS 2048x2048), `PU_STATIC` к этому моменту 8.7 МБ.
  Самые тесные из прошедших (свободно после 35 кадров): MAPM3 **0.42 МБ** (самый большой блок 96 КБ), MAPM7 1.40, MAP10 1.63, MAPM9 1.77 МБ.
* **Hardware, 84 карты (`--tag hw-base`, 645 с): 83 из 84** (падает только MAP11: `OOM 52 B PU_HWRPLANE` на загрузке уровня; MAPM3/MAPMG проходят благодаря Try-путям PS2-140). Самые тесные: MAP23 1.06 МБ, MAP14 1.15, MAP10 1.34, MAP40 1.93 МБ; пик C-кучи (libc) 413 760 Б (UDMF Match).
* **«DEMO_003 в HW падает OOM на тике ≈ 306» на этом HEAD не воспроизводится**: `-renderer Hardware` — `timed 1051 gametics in 1880 realtics - 1049 frames`, то же с `-zreserve 1536` (1882), то же профильная ELF `out-prof` с `-ps2prof -zreserve 1536` (2092): ни одной строки OOM. (OOM из отчётов HT/OPT10 относится к ветке до слияния PS2-144/146 S.) Защита от него всё равно нужна (раздел 1).
* Карты, которые брифом названы «не помещающимися в HW» (MAP08/10/14/23/31): в HW на HEAD **проходят** 35 кадров; не проходит только MAP11 (его уровень 17.35 МБ: `PU_LEVEL` 17 435 блоков; полигоны плоскостей 15 942 подсекторов + батчи + рабочий набор не помещаются).
* Список владельцев памяти MAP11 (ZDEBUG-ELF, `-zowners`): lines 2.81 МБ (25 100 × 112), segs 2.78 (49 592 × 56), mobj 2.76 (6754 × 408), sides 1.55, sectors 1.33, nodes 0.83, blockmap 0.69+0.28+0.28(`polyblocklinks`), pack-индекс 0.66+0.51, спрайтовые кадры 0.67, HUD-патчи 0.45, spill libc→арена 0.75 МБ.

## 1. Рекуперируемая нехватка памяти вместо I_Error (PS2-170)

**Идея.** Кадр рисуется под «стражем» (`Z_GUARD_TRY`, `z_zone.h`): если `Z_Malloc` не нашёл места после всех вытеснений и хуков, он не вызывает `I_Error`, а прыгает (`longjmp`) обратно в страж. Дальше:
* **Hardware:** недорисованный кадр сбрасывается (`PS2HWD_Abort`: остановка DMA 1/2, `GIF_CTRL.RST`, сброс VIF1, очередь ring, состояние кадра), `currently_batching`/батчи/интерполяторы/wipe/`viewwindowy` приводятся в порядок, дальше обычное переключение рендерера как в Options → Video (`SCR_SetMode` → `HWR_ClearAllTextures`, `HWR_Shutdown`, драйвер выключается, GS берёт software), `PU_HWRPLANE` освобождается; карта запоминается как «не влезает в HW». **Обратно:** в конце загрузки следующего уровня, если пользователь выбрал Hardware (`-renderer Hardware` или cvar), карта не в списке и сбоев < 6, `setrenderneeded = render_opengl` — следующий `D_Display` (под тем же стражем) возвращает HW. Любой сбой при возврате снова даёт software.
* **Software:** кадр бросается, `Z_EmergencyFree` (весь кэш любого возраста, хуки звука/HW, соседние дыры сливаются), кадр рисуется заново; три провала за 12 кадров подряд — прежний отчёт `OOM:` и ошибка.
* Тот же страж ловит `hw_failure` драйвера («PS2 HW resource failure», тупик DMA): раньше `I_Error`.
* Во время вызова Lua (`lua_getstack(gL, 0, …)`, `PS2Lua_InCall`) прыжка нет (состояние Lua нельзя бросить на полпути) — как раньше.
* Уровень: `HWR_LoadLevel` вызывается из `PS2HWFB_BuildLevel` под стражем (то же падение → software с первого кадра уровня).

Файлы: `src/z_zone.c/.h` (`zguard_t`, `Z_OutOfMemory` → `longjmp`, `Z_AddReclaimHook`, `Z_EmergencyFree`, внедрение отказов `-zoomtest`), `src/ps2/ps2_hwfb.c/.h` (политика), `src/ps2/hw/ps2_hwd.c` (`PS2HWD_Abort`), точечно `d_main.c` (`PS2HWFB_Display(D_Display)`), `p_setup.c` (`PS2HWFB_BuildLevel`, `PS2HWFB_LevelLoaded`), `i_video.c` (`Impl_HWFailure`), `lua_script.c`/`lua_stub.c` (`PS2Lua_InCall`), `ps2_spill.c` (`PS2Spill_Reset`).

**Проверка запуском (ELF `build/elf-n1.ELF`, коммит 5965915):**
* `-hwfbtest 60 -zchain 02` (имитация сбоя внутри кадра 60 в HW): `HARDWARE -> SOFTWARE`, MAP01 дошла до 200 кадров, MAP02 — «trying the hardware renderer again», 200 кадров HW (`build/runs/fbt1`).
* `-zoomtest 60,90 -zchain 02,03` (настоящий отказ `Z_Malloc`: кадр 64 `12 B PU_HWRPATCHINFO` в MAP01; кадр 200 `52 B PU_HWRPLANE` при построении плоскостей MAP02 после возврата): оба раза возврат в software, MAP03 снова в HW (`build/runs/fbt2`).

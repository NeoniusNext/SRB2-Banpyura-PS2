# OPT7, агент H: GS Hardware Renderer (отчёт, ведётся инкрементально)

Сессия 2026-10-06. Каталоги: `build/opt7-h/{out,run,pak}`. Скрипты: `build/opt7-h/mk.sh` (HW релиз-сборка через `bh.py`), `run.py NAME --elf ELF [--demo D] -- <args>`, `pair.sh TAG ELF DEMO "vidshot spec"` (парный прогон software/HW), `cmp.py`.
Эмулятор: PCSX2 2.6.3 (`D:\PCSX2-test`, GS Vulkan), запуск только через `tools/ps2/run_pcsx2.py` (общая блокировка). Предыдущая часть: `opt5-H.md` (PS2-HW-21: показ кадра/watchdog); OPT6-H отчёта не оставил (в дереве: PS2-HW-22 — ключ сортировки батчей по идентичности текстуры, `-hwtrace/-hwdbg`).

## 0. Состояние на старте
* Сборка HW релиз (`SRB2_PS2_HW=1 SRB2_PS2_RELEASE=1`, `build/opt7-h/out`) проходит, ELF 10 151 344 Б (`b0.ELF`).

(продолжение — по мере работы)

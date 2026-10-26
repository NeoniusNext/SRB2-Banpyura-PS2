# Инструменты исследования RSYS (одноразовые, не часть игры)

* `ps2_iolog.c` — зонд устройства: `--wrap=_read/_lseek/_open/_close`, печатает каждое чтение, которое C-библиотека делает у IOP (`IO R fd pos запрошено вернуло мкс`, `IO S`, `IO O fd путь`, `IO C`).
  Подключение: добавить в `tools/ps2/build.py` перед `OBJ.mkdir(...)` блок «если `SRB2_PS2_IOLOG=1`: `EXTRA_SOURCES.append('src/ps2/ps2_iolog.c')` и `-Wl,--wrap=` для `_read _lseek _open _close`»;
  также нужно определение `unsigned long long ps2prof_sleep_cyc;` (в HEAD без `--prof` его нет, см. RS-00). Строки видны в `pcsx2.log` запуска (не в `boot.txt`: это `printf`, а не `I_OutputMsg`).
* `srp2.py` — читатель индекса SRP2 (v1/v2). `iosim.py` — модель читателя пака (`w_pack.c` + 64-КБ буфер newlib) и моделей носителей. `iolog2.py` — разбор `IO`-журнала, группировка по файлам.
  `phase.py`, `rdphase.py`, `regions.py`, `alt.py`, `alt2.py`, `alt3.py` — срезы по фазам (старт/карта/игра), уникальные лампы, области индекса, сравнение политик окна (текущая 64 КБ, «точно» 2/4 КБ, гибриды «4 КБ; 16/32/64 КБ, если впереди ≤ T», пакет-«свип»); `alt3.py` строит заново чтения по таблице пака `pak3` и печатает время для 5 моделей носителей.
* Запуск (пример): `python3 iolog2.py build/runs/NAME/pcsx2.log`; `python3 phase.py LOG t_boot_end,t_level_end boot,level,play` (границы — метки времени хоста из `pcsx2.log`).

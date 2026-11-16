# PAK OPT17 — свежая сборка 2026-11-16

Собраны четыре компактных PAK и отдельный парный TEXC из неизменяемого stock набора. Runtime-код не менялся; совместимый ELF — [Hardware OPT17](../hardware-opt17-20261116/README.md), SHA256 `7bf67e21438e697cf40c112b9166c2a3018956fbe909ef34f0780de17750e96d`.

| Файл | Байт |
| --- | ---: |
| SRB2.PAK | 61253632 |
| ZONES.PAK | 24442880 |
| CHARS.PAK | 2682880 |
| MUSIC.PAK | 100573184 |
| Четыре PAK | **188952576** |
| FINEACON.DAT, копия исходного | 524288 |
| Необязательный TEXC.PAK | 15738880 |

Сохранены SRP2 v2, LZ4HC12, decoded blocks 64 KiB, dedup, номера/порядок lump и защищённые startup offsets. `--fill-gaps` сокращает размер на **1150976 Б (0,605%)** без изменения stored/decoded payload. На directory-order обходе эта опция может давать дополнительные seeks; общий выигрыш скорости/FPS не установлен. Это отдельный компактный вариант, прежний append-only default cooker сохранён.

[Верификация PAK](pak-verification.json) сравнивает все **15052 lump / 401801745 decoded bytes**, настоящие C reader full/partial reads и ASan/UBSan. Исходные архивы/PAK не изменены. [TEXC verification](texc-verification.json) подтверждает 3318 records / 3309 keys, 129686189 уникальных decoded bytes и 129989949 pixel bytes в каждом пути fetch.

[Обычная проверка игры](runtime-validation.json) воспроизводит по 3 полных кадра для нового base-набора и парного TEXC относительно штатных ресурсов на 32 МиБ. Все 6 PPM, loader hashes и saved config совпадают. Для сравнения использовалась [фиксированная local-RNG копия](../../docs/research/opt17/final-321c/local-rng-diagnostic-admission.json) итогового ELF; в файлах поставки нет изменённого RNG.

[Дополнительный контроль TEXC](runtime-usage-matched.json) с одинаковыми profiler/con_hudlines0 в обоих запусках даёт ещё 3 побайтно равных полных кадра и 1090 фактических cache hits, 0 missing keys/повреждений. Первый [несогласованный observer](runtime-usage-unmatched.json) сохранён как failed comparison: profiler напечатал console HUD поверх верхних 40 строк относительно reference без profiler. Новый контроль сравнивает полные кадры без cropping/изменения допусков.

TEXC заново экспортирован именно из этих компактных PAK. Его SHA256 `d4dca18293b00af3555e3989ea517274e197f95268a3397bb9ec64a6bfb42eb8` отличается от stock TEXC; для установки нужны совпадающие base PAK, перечисленные в [manifest](build-manifest.json). Для минимального расхода RAM используйте основной набор без TEXC: cache добавляет около 322358 Б fixed metadata/window payload и optional prefetch (default до 1 МиБ). `-texcmem 0` отключает prefetch, сохраняя metadata. Физическая PS2 и общий FPS gain не проверены.

Локальные download-архивы созданы отдельно: `SRB2-PS2-PAKs-OPT17-20261116.zip` (четыре PAK, FINEACON, PNG sidecar, README и SHA256) и `SRB2-PS2-TEXC-for-OPT17-20261116.zip` (TEXC с base identity manifest). Их размеры и SHA256 — в [manifest](build-manifest.json) и [SHA256SUMS](SHA256SUMS). Игровые payload/ZIP и BIOS не добавлены в Git; этот каталог хранит воспроизводимые отчёты и контрольные суммы.

Сборка: `python tools/ps2/cook.py --from-pak build/pak --out <fresh-dir> --jobs 4 --fill-gaps`, затем копия `build/pak/FINEACON.DAT`. Парный TEXC: `tools/ps2/cook_texc.sh <fresh-dir> <optional-dir> <fresh-host-build>` при настроенных host dependencies. Проверки: `pak_layout_verify.py` и `texc_stock_verify.py`; полный invocation/source hash набор сохранён в отчётах.

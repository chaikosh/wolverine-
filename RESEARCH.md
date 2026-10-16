# Порт X2: Wolverine's Revenge на iPhone: исследование

Дата: 16.10.2026
Устройство: iPhone 14 Pro Max (A16, 6 ГБ), iOS 27.0.1
Мак: Apple M1, 8 ГБ, Xcode 26.6 (iOS SDK 26.5)

## 1. Что лежит в репозитории

Своя копия PC-версии игры (с widescreen-фиксом); в репозиторий не входит.

| Что | Детали |
|---|---|
| `Wolvie.exe` | PE32 x86, собран 4 апр 2003, не упакован, RTTI нет |
| API | Direct3D 8, DirectInput 8, DirectSound, Bink (`binkw32.dll`) |
| Видео/музыка | 140 файлов `.bik` (~790 МБ) |
| **Игровые данные** | **ОТСУТСТВУЮТ.** Exe грузит `data\*.all`, `data\*.xpr`, `data\*.cut`, `DATA\VRAM\...` (frontend.all, effects.all, logan_hud.all, img0–7.xpr и др.). В копии их не было (позже выяснилось: данные лежат в `data.pkr`/`wavs.pkr`). |

- `d3d.log` показывает, что игра запускалась 15.12.2022 на Radeon HD 7660G. Значит, `data\` существовала и потерялась при копировании.
- `.xpr` (Xbox Packed Resource) и `dsstdfx.bin` говорят о том, что PC-версия — порт с Xbox.
- В exe сохранилось **170 путей исходников** `C:\Backup\Wolvie\Pc\...`. Модули: frontend (84), ai (44), game (33), render (16), lti (12), mission_ai (10), sound (8), psys (6), collision (5), weapons (4), framework, platform. Это хорошая карта для декомпиляции.

## 2. Методы

| # | Метод | iOS 27 | Скорость | Трудозатраты | Вердикт |
|---|---|---|---|---|---|
| 1 | **GameCube-версия в DolphiniOS + JIT через StikDebug** | Вероятно: beta 6 (16.06.2026) добавила iOS 27 beta, но подтверждений на 27.0.1 + A16 нет | Полная, игра в Dolphin имеет рейтинг «Perfect» | Часы | **Этап 1, основной** |
| 2 | PS2-версия в ARMSX2 + StikDebug | SideStore пишет, что работает на 26.6/27 | Неизвестно для этой игры, PS2 тяжелее | Часы | Запасной |
| 3 | PC-версия через x86-эмуляцию (UTM / Wine) | UTM работает | Единицы FPS: эмуляция x86 + D3D8 без GPU-ускорения | — | Нет |
| 4 | Стриминг (Moonlight / Steam Link) | Да | Зависит от сети | Нужен Windows-ПК | Это не порт |
| 5 | Нативный порт: статическая рекомпиляция GC-версии (DolRecomp / NWiiRecomp) | Не нужен JIT | Нативная | Месяцы+, нужно писать свой рантайм GX→Metal | Инструменты в pre-alpha. Ни одна GC-игра пока не играбельна (Mario Party 7 доходит до интро) |
| 6 | Нативный порт: декомпиляция PC exe (Ghidra) + замена D3D8/DInput/DSound/Bink на Metal/GameController/AVAudio | Не нужен JIT | Нативная | Месяцы – год+ | **Этап 2, долгосрочный.** Нужна полная папка `data\` |

Почему важен JIT: без него Dolphin на iOS работает «неиграбельно медленно». Apple не даёт JIT обычным приложениям. Его включает StikDebug: приложение запускается под отладчиком. Это хрупкая часть, ломается с обновлениями iOS. Нативный порт (5 или 6) обходит эту проблему полностью: ставится как обычное приложение.

## 3. План

**Этап 0: файлы**
- Восстановить папку `data\` PC-версии с исходной машины или с оригинального диска.
- Снять образ своего GameCube-диска (Wii + homebrew CleanRip) в `.iso`/`.rvz`.

**Этап 1: играть сейчас (DolphiniOS)**
1. SideStore на iPhone → источник DolphiniOS → DolphiniOS beta ≥ 5.0.0b6.
2. StikDebug ≥ 2.3.0 → включить JIT для DolphiniOS.
3. Bluetooth-геймпад (DualSense / Xbox / Backbone) или сенсорный оверлей.
4. Проверить: стабильный JIT на 27.0.1, полный FPS, сохранения, видеоролики.

**Этап 2: нативный порт (при желании)**
- Основа: PC exe (x86 + D3D8 проще всего переписать на Metal).
- Ghidra → декомпиляция по модулям из карты путей выше → свой платформенный слой под iOS.
- Нужен Xcode 27 (текущий 26.6 не деплоит на iOS 27).

## Источники
- https://dolphinios.oatmealdome.me/beta
- https://dolphinios.oatmealdome.me/jit-help
- https://docs.sidestore.io/docs/advanced/jit
- https://github.com/Xarber/StikDebug27
- https://en.dolphin-emu.org/compat/X
- https://wiki.dolphin-emu.org/forum/X2: Wolverine's Revenge/embed
- https://github.com/ExpansionPak/DolRecomp
- https://github.com/BlackLineInteractive/NWiiRecomp/blob/master/README.md

## 4. Нейросети и агенты в портах (состояние на октябрь 2026)

### Объём нашего кода
- `.text` 1,8 МБ, ≈ **6 200 функций** (по уникальным целям CALL)
- Математика почти целиком на x87 FPU (~87 тыс. опкодов против ~1,4 тыс. SSE)

### Чего добились агенты (matching-декомпиляция)
| Проект | Результат |
|---|---|
| Snowboard Kids 2 (N64) | Claude Opus: 25% → 58% с первой попытки, ~75% после доработки процесса. Дальше длинный хвост. На одну функцию ушло 87 попыток |
| Snowboard Kids (N64, 2 145 функций) | 84 дня с агентами против 596 дней на сиквел без них |
| Klonoa (GBA) | 51% кода с Claude Code |
| Tales of Symphonia (GC) | ~71% функций (данные автора, не проверены) |

Где агенты буксуют: функции длиннее ~1 000 инструкций, математика (матрицы, векторы), графический код.

### Вывод для нашего порта
- Matching-декомпиляция (код, побайтно совпадающий с оригиналом) нужна для сохранения игры как наследия. Для порта она не обязательна.
- Лучший путь — **статическая рекомпиляция**: x86 → C механически, все 6 200 функций сразу и без угадывания. ИИ пишет сам рекомпилятор и платформенный слой (D3D8 → Metal, DirectSound, DirectInput, Bink, Win32).
- Публичных примеров рекомпиляции x86-игры с ПК в ARM не нашлось. Мы будем первыми, но задача решаемая.

### Оценка с агентами
| Этап | Срок |
|---|---|
| Рекомпилятор x86 → C, весь exe собирается | 1–2 нед |
| Win32-слой (файлы, память, таймеры, окно) | дни |
| D3D8 → Metal | 1–3 нед |
| Звук, Bink, ввод, сенсорное управление | ~1 нед |
| Полное прохождение и отлов багов | недели |
| **Итого до готового порта** | **~1,5–3 мес** |

Источники:
- https://blog.chrislewis.au/the-long-tail-of-llm-assisted-decompilation/
- https://aisignal.day/en/ai-game-decompilation-2026/
- https://macabeus.medium.com/starting-a-decompilation-project-from-zero-claude-code-and-51-of-a-2001-gba-game-a9a75a4c1487
- https://news.ycombinator.com/item?id=47038270
- https://arxiv.org/pdf/2609.05370
- https://heldgames.com/guides/mario-kart-wiicompiled-first-wii-recomp

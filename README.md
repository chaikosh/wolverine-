# X2: Wolverine's Revenge — native iOS/macOS port via static recompilation

The 2003 Windows game's x86 code (`Wolvie.exe`) is translated ahead-of-time into C and compiled for ARM64.
Win32 and DirectX 8 are reimplemented on SDL3, Metal and FFmpeg (no emulator, no JIT). Runs on Apple Silicon Macs
and iPhone (tested on iPhone 14 Pro Max, iOS 27) at 60 fps with touch controls, gamepad support and haptics.

**No game files are included.** You need your own copy of the PC version; put its folder (with `Wolvie.exe`,
`data.pkr`, `wavs.pkr`, `movies/`, `music/`) at `./game` or pass `GAME_DIR=...`. The recompiler generates C from
your own executable at build time. This project is not affiliated with Activision, Microsoft or Marvel; all
trademarks belong to their owners.

---

# По-русски: нативный порт через статическую рекомпиляцию

`Wolvie.exe` (PC, x86, 2003) механически переводится в C и собирается под ARM64 (Mac → iPhone).
Вызовы Windows/DirectX обслуживает свой рантайм: Win32 → SDL3, Direct3D 8 → Metal.

## Структура
- `recomp/` — рекомпилятор на Python: разбор PE, поиск функций и switch-таблиц, перевод x86 → C (`lift.py`).
- `runtime/` — рантайм: гостевая память и диспетчер (`core.c`), куча (`gheap.c`), Win32 (`win32/`), Direct3D 8 → Metal (`d3d8/`), COM (`com.c`).
- `RESEARCH.md` — сравнение способов запуска на iPhone.

## Сборка (macOS, Apple Silicon)
```bash
python3 -m venv .venv && .venv/bin/pip install capstone
brew install sdl3
make            # рекомпиляция + сборка build/wolvie/wolvie
make run        # запуск из папки игры (GAME_DIR, по умолчанию ./game)
```
Переменные для отладки: `TRACE_IMPORTS=1` (файлы), `TRACE_IMPORTS=2` (каждый вызов API), `TRACE_D3D=1`.

## Нужные файлы игры (в git не входят)
Рядом с `Wolvie.exe`: `data.pkr`, `wavs.pkr`, `movies/`, `music/`.

## Управление на Маке (клавиатура)
| Действие | Клавиша |
|---|---|
| Движение | W A S D |
| Камера | стрелки, сброс — R |
| Удар / пинок / действие | J / K / L |
| Когти (двойное нажатие — Feral Rage) | I |
| Прыжок / присесть / скрытность | Пробел / C / Shift |
| Чутьё | F |
| Меню / пропуск роликов | Esc, подтверждение — Enter |

Bluetooth-геймпад (Xbox / DualSense) подхватывается автоматически.
Оригинальная раскладка игры сохранена в `Wolvie.INI.orig`.

## iPhone / iPad
Зависимости (один раз): `deps/build_deps_ios.sh iphoneos` и `deps/build_deps_ios.sh iphonesimulator`.

Симулятор:
```bash
make -f Makefile.ios                      # build/ios-iphonesimulator/Wolverine.app
xcrun simctl install booted build/ios-iphonesimulator/Wolverine.app
xcrun simctl launch booted com.chaikosh.wolverine
```

Настоящий iPhone (через Xcode, подпись своим Apple ID):
```bash
make -f Makefile.ios SDK=iphoneos lib && make -f Makefile.ios xcode
open ios/Wolverine.xcodeproj
```
Проект подписывается командой 33UFZAL6L2 (платный аккаунт) через существующий профиль «iOS Team Provisioning Profile: *».
Сборка из терминала и установка на сопряжённый iPhone (кабель или та же Wi-Fi):
```bash
cd ios && xcodebuild -project Wolverine.xcodeproj -scheme Wolverine -sdk iphoneos -configuration Release \
  -derivedDataPath ../build/xcode-dd CODE_SIGN_STYLE=Automatic DEVELOPMENT_TEAM=33UFZAL6L2 build && cd ..
ios/install_device.sh
```
На телефоне должен быть включён «Режим разработчика». Если Xcode 26.6 откажется ставить на iOS 27 — нужен Xcode 27.

Сохранения и `Wolvie.INI` лежат в «Документах» приложения (видно в «Файлах»).
Экранное управление: левая половина — стик, правая — камера свайпом; P/K/J/A — удар/пинок/прыжок/действие,
X — когти (двойное нажатие — ярость), S — скрытность, C — присесть, E — чутьё, R — камера, ‖ — пауза, O — OK.

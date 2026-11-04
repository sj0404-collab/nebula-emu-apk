# Nebula Emu

Windows x86/x64-эмулятор (wasm64) + **настоящий opencode** на устройстве — без ПК и Termux.

| Слой | Что это |
|------|---------|
| `emu/` + `public/x64/nebula_x64.wasm` | wasm64-эмулятор Windows PE (x86/x64) |
| `src/` | Веб-UI (Vite + TS): эму, TUI-подобный opencode-чат, сессии, workspace |
| `opencode/bridge.ts` | Bun-мост :8787 → локальный opencode :4096 (ПК) |
| `android/` | Capacitor-обёртка: WebView + встроенный opencode (`jniLibs/arm64-v8a`) на 127.0.0.1:4096 |

## Фичи

- **Браузерное хранилище** — IndexedDB: сессии, сообщения, черновики, мета workspace; экспорт/импорт (`nebula-oc-session` / `nebula-oc-backup`) для переноса ПК ↔ телефон.
- **Выбор папки-воркспейса** — кнопка 📁:
  - **ПК (Chromium)**: File System Access (`showDirectoryPicker`) + fallback-путь;
  - **Android**: SAF (`ACTION_OPEN_DOCUMENT_TREE`) через Capacitor-плагин, реальный путь `/storage/...`;
  - **браузер**: prompt абсолютного пути.
  - handle в IDB **не хранится** (функции не сериализуются) — только `handleName` + путь.
- **Список сессий** — сайдбар: параллельные вкладки, ✎ переименование, ✕ удаление; сессии разных папок не мешают друг другу.
- **Без дубликатов** — «+ new» / запуск в папке переиспользует уже привязанную сессию (`findSessionForWorkspace`), а не плодит близнецов.
- **offline** — при недоступном сервере opencode рендерится из кэша IndexedDB.
- **Модели** — дефолт бесплатные, дерево по провайдерам; ключи API в UI не отдаются.

## Требования (ПК)

- Node.js 24+, **bun** (для bridge), opencode CLI (тесты: 1.18.34).
- Для Android-сборки: JDK 21, Android SDK (здесь `E:\AndroidSDK` → `android/local.properties`), `cmdline-tools` для `sdkmanager`.

> **Windows / PowerShell:** `npm` из системного PATH ломается (`CantActivateDocumentInPipeline`).  
> Собирайте через `.\node_modules\.bin\tsc -b`, `.\node_modules\.bin\vite build` или `cmd /c "npm run ..."`.

## Запуск (ПК)

```powershell
# 1) зависимости
bun install

# 2) локальный opencode (если ещё не установлен)
#    npm i -g opencode-ai   (или официальный установщик)

# 3) bridge + UI (два терминала)
bun run opencode          # :8787 → proxy к opencode :4096
.\node_modules\.bin\vite  # :5173
```

Откройте http://127.0.0.1:5173 → вкладка opencode. Статус: `live · версия · proxy`.

Опционально включите сам opencode: `opencode serve` (порт 4096) — bridge ходит в `127.0.0.1:4096`.

## Проверки

```powershell
# typecheck (app + opencode)
.\node_modules\.bin\tsc -b --noEmit
.\node_modules\.bin\tsc -p tsconfig.opencode.json --noEmit

# unit bridge (нужен bridge на :18787 — см. opencode/bridge.test.ts)
bun test opencode/bridge.test.ts

# e2e: picker, штамп directory, reuse сессии, IndexedDB (playwright-core + chromium)
node scripts/e2e-workspace.mjs
# ожидается: [done] ALL OK
```

E2E мокает `showDirectoryPicker`/`prompt`, не трогает реальную ФС, чистит тест-сессии в `test-ws-browser`.

## Сборка фронта

```powershell
.\node_modules\.bin\tsc -b
.\node_modules\.bin\vite build     # dist/
```

`build:wasm` требует zig/cc (на этой машине нет) — wasm в `public/x64/` уже готовый.

## Android (без ПК/Termux)

Образ WebView грузит `dist/`; настоящий opencode лежит в APK как `jniLibs/arm64-v8a/libopencode.so` (+ `libmusl-loader.so`) и поднимается на **127.0.0.1:4096** (cleartext-исключение для loopback).

```powershell
# 1) бинарь opencode для arm64 (муслинг) + ld-musl
node scripts/prepare-android-opencode.mjs            # окружение: OPENCODE_VERSION, MUSL_LOADER_URL, FORCE=1

# 2) web assets → android/
.\node_modules\.bin\vite build
.\node_modules\.bin\cap sync android

# 3) APK
cd android
.\gradlew.bat assembleRelease
# → android/app/build/outputs/apk/release/
```

`android/local.properties`:

```properties
sdk.dir=E:\\AndroidSDK
```

Плагин Capacitor (`OpencodeNativePlugin`):

- `pickDirectory` — SAF tree picker, колбэк `onDirectoryPicked`, преобразование URI → путь;
- `listDirectory` / `openStorageSettings` / `hasAllFilesAccess` — дроверка и настройки «Все файлы»;
- манифест: `MANAGE_EXTERNAL_STORAGE` + read/write.

## Структура

```
src/
  main.ts              # UI, сессии, workspace, параллельные OcView
  opencode-client.ts   # REST/SSE, list/create/delete/rename, findSessionForWorkspace
  store.ts             # IndexedDB (sessions/messages/drafts/meta), экспорт/импорт
  workspace.ts         # pickWorkspace (SAF / FSA / prompt), setWorkspace
  native.ts            # мост к Java-плагину (Android)
opencode/bridge.ts     # :8787 proxy, x-opencode-directory / ?directory=
scripts/
  e2e-workspace.mjs    # e2e workspace + reuse
  diag-ws.mjs          # быстрая диагностика пикера
  prepare-android-opencode.mjs
android/               # Capacitor + gradle
```

## API opencode (важное)

- `GET /session?directory=...` — **фильтр только по query** (заголовок на list игнорируется).
- `POST /session?directory=...` — штампует `session.directory`.
- `PATCH /session/:id` — переименование → 200; `DELETE /session/:id` → 200.
- SSE `/event` — маршрутизация сообщений по `sessionID`.
- Клиент шлёт и `?directory=`, и заголовок `x-opencode-directory`; bridge дополнительно подставляет query на `/session`.

## Известные ограничения

- Без zig/cc нельзя пересобрать wasm (`npm run build:wasm`).
- Edge блокирует CDP админом — e2e только на playwright-chromium.
- README-примеры ниже могут отставать от `package.json`; источник истины — скрипты в `scripts/`.

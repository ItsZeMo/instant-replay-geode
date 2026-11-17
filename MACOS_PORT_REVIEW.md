# Instant Replay: ревью macOS-порта и промпт на исправление багов

> **Для агента-исполнителя** (Claude Code / Antigravity / Codex).
> Ревью покрывает все изменения порта на macOS: диапазон `1166b68..5e367b9`, то есть PR [#1](https://github.com/ItsZeMo/instant-replay-geode/pull/1) (`b621b09`) и коммиты `b00b3be` и `5e367b9`, сделанные прямо в `main`. Номера строк указаны по коммиту `5e367b9`.
> Ревью статическое: код прочитан целиком, но на реальном Mac не запускался. Пункты, которые без железа не подтвердить, помечены **[проверить на Mac]**.

## Задача

Исправь баги ниже в порядке P1 → P2 → P3. Раздел 5 (рефакторинг) делай только отдельным PR и только если тебя об этом попросят. Раздел 6 перечисляет ложные срабатывания, их не трогай.

## 0. Контекст и правила

- База: `main` @ `5e367b9`. Работай в отдельной ветке, не в `main`.
- **AGENTS.md устарел.** Он утверждает, что мод работает только на Windows и что `CMakeLists.txt` падает на не-WIN32. После порта это неверно (см. M21). Как устроена macOS-часть:
  - **Захват кадра.** Swizzle `-[NSOpenGLContext flushBuffer]` (`src/platform/MacPlatformUtils.mm`) вызывает `FrameGrabber::captureCurrentFrame()`. Дальше FBO-blit с переворотом по Y, затем PBO, затем IOSurface-`CVPixelBufferRef` (BGRA, строки сверху вниз).
  - **Энкодер.** `src/encoder/VideoToolboxEncoder.mm`: AVAssetWriter поверх VideoToolbox, AAC через `AVAssetWriterInput`. Склейка сегментов: AVAssetReader → AVAssetWriter в режиме passthrough (`concatenateImpl`).
  - **Микрофон.** `src/capture/AudioGrabberMac.mm`: CoreAudio HAL плюс `AudioQueueNewInput`. Игровой звук идёт через общий FMOD DSP.
  - **Выбор кодека.** `src/platform/MacVideoCapabilities.mm` (`VTCopyVideoEncoderList`) плюс `ReplayConfig::sanitizeCodecSettings()`.
  - **Общий код.** `ReplayManager`, `ReplayConfig` и UI переведены на `std::filesystem::path` и `PlatformEncoder`.
- Локально (Linux) не собрать ни Windows-, ни macOS-таргет. Компиляцию проверяй через GitHub Actions, джобы `build` (Win64) и `build-mac` в `.github/workflows/build.yml`: пушь ветку и смотри CI.
- **Не ломай Windows.** macOS-логика живёт только под `#ifdef GEODE_IS_MACOS` или в `.mm`. Если правишь общий код (`ReplayManager.cpp`, `ReplayConfig.hpp`, `ConcatTimestampMath.hpp`, UI), убедись, что на Windows поведение не меняется. Исключение — M6, это регрессия Windows.
- `.mm` собираются с `-fobjc-arc`. В `.cpp` не подключай заголовки Cocoa и AVFoundation.
- Инварианты из AGENTS.md §4 по-прежнему действуют:
  - UTF-8 пути только через `geode::utils::string::pathToString`;
  - сохранение настроек под `s_isSavingSettings`;
  - не перезаписывать сфокусированный `TextInput`;
  - cocos2d, `Notification` и `FMODAudioEngine` вызывать только на main thread (через `geode::queueInMainThread`).
- Один баг — один коммит: `fix(macos): M<N> <кратко>`, для M6 — `fix(win): M6 ...`. Комментарии в коде и сообщения коммитов пиши на английском.
- Для пунктов **[проверить на Mac]**: сделай безопасный защитный фикс и добавь в описание PR чек-лист ручной проверки.

## 1. Сводка

| ID | Приоритет | Где | Суть |
|---|---|---|---|
| M1 | P1 | `VideoToolboxEncoder.mm:1424-1453` | Склейка может зависнуть навсегда: «сохранение» висит, F6 больше не работает, игра виснет при выходе |
| M2 | P1 | `VideoToolboxEncoder.mm:1007-1030` | Временная неготовность AAC-входа считается ошибкой: рассинхрон, пересоздание энкодера, остановка записи |
| M3 | P1 [проверить на Mac] | `AudioGrabberMac.mm:275-423` | Разрешение на микрофон (TCC) не обрабатывается: тихая дорожка или вылет |
| M4 | P2 | `ReplayManager.cpp:692-707` | Retina: «Native» пишется в пунктах (вдвое меньше пикселей), битрейт и размер считаются по разным высотам |
| M5 | P2 | `AudioGrabberMac.mm:128-163` | Если выбранный микрофон пропал, нет перехода на системный (на Windows есть) |
| M6 | P2 | `ReplayManager.cpp:71, 306` | Регрессия Windows: `path::string()` бросает исключение на не-ANSI именах |
| M7 | P2 | `VideoToolboxEncoder.mm:1315-1550` | Ошибки склейки после `startWriting` оставляют writer и reader без `cancelWriting`/`cancelReading` |
| M8 | P2 [проверить на Mac] | `VideoToolboxEncoder.mm:1186-1195` | У passthrough-входа видео не задан `mediaTimeScale`: таймстемпы округляются на 120/240 FPS |
| M9 | P2 [проверить на Mac] | `VideoToolboxEncoder.mm:1312-1579` | Объекты всех сегментов живут до конца экспорта: дескрипторы и память на часовых реплеях |
| M10 | P2 [проверить на Mac] | `VideoToolboxEncoder.mm:1399-1531` | При ребейзе склейки смешиваются output-PTS и raw-PTS (AAC priming/trim) |
| M11 | P2 | `MacPlatformUtils.mm:188-194` | Хук `flushBuffer` переключает GL-контекст и не возвращает прежний |
| M12 | P2 [проверить на Mac] | `FrameGrabber.cpp:190-225` | При выходе `glDelete*` вызывается без проверки, что контекст GD текущий на этом потоке |
| M13 | P3 | `ReplayManager.cpp:190, 339-357` | Папки `pid_*` от упавших сессий не чистятся при переиспользовании PID; lock-fd открыт без `O_CLOEXEC` |
| M14 | P3 | `ReplayPopup.cpp:1439-1462` | Плашка про AV1 видна всегда и неверно пишет «using HEVC» |
| M15 | P3 | `main.cpp:175-181` | Уведомление с длинным тире (в шрифте GD нет глифа) и текстом «on this Mac» в общем коде |
| M16 | P3 | `ReplayPopup.cpp:745-747`, `ReplayConfig.hpp:545-555` | Выключение HW молча меняет кодек; в SW-режиме выбирается HEVC SW вместо H.264 SW |
| M17 | P3 | `ReplayManager.cpp:2908-2925` | `openFolder` (AppKit/NSWorkspace) вызывается из фонового потока |
| M18 | P3 [проверить на Mac] | `VideoToolboxEncoder.mm:703-713`, `FrameGrabber.cpp:69-89` | Нет цветовой разметки BT.709: в части плееров цвета выглядят блёклыми |
| M19 | P3 | `VideoToolboxEncoder.mm` (вызовы AVFoundation) | Obj-C исключения AVFoundation проходят через C++ и роняют GD |
| M20 | P3 | `MacPlatformUtils.mm:226-236`, `main.cpp:91-97` | Неудачная установка хука только логируется: запись «идёт», а видео нет |
| M21 | P3 | `AGENTS.md`, `README.md`, `about.md`, `mod.json` | Документация и описания утверждают, что мод только для Windows |
| M22 | P3 | `CMakeLists.txt:6-13` | `if (APPLE ...)` стоит до `project()` и поэтому никогда не выполняется |

---

## 2. P1 — критично

### M1. Склейка сегментов может зависнуть навсегда (бесконечный `usleep(500)`)

**Где.** `src/encoder/VideoToolboxEncoder.mm`:
- `:1186-1195` — passthrough-входы с `expectsMediaDataInRealTime = NO`;
- `:1424-1453` — цикл выбора дорожки.

Последствия видны в `src/recorder/ReplayManager.cpp`: `:2879` (`AlreadySaving`), `:599` (`shutdown()` → `joinExportThread()`), `:102` (`~ReplayManager` → `joinExportThread()`).

**Проблема.** У non-realtime входов AVAssetWriter выдерживает чередование дорожек (interleaving). Вход, ушедший вперёд, получает `readyForMoreMediaData == NO` и ждёт, пока отстающий вход получит данные или будет помечен `markAsFinished`.

В непоследнем сегменте дорожка, дошедшая до EOF, не помечается finished: её данные придут в следующем сегменте. Предположим, в сегменте одна дорожка заметно короче другой. Например, звук короче видео на пару секунд после ошибок записи из M2, или mic-дорожка почти пустая. Тогда:
- ни один вход, у которого ещё есть данные, не готов;
- `bestIdx == slots.size()` и `writer.status == Writing`;
- цикл бесконечно крутит `usleep(500); continue;`, потому что нет ни таймаута, ни проверки прогресса.

**Последствия.**
- Поток экспорта висит вечно, `m_exportInFlight` не сбрасывается, и на каждое следующее F6 приходит «Previous replay is still saving...».
- `m_exportLockedSegments` не освобождаются, отложенное удаление сегментов не срабатывает, temp-папка растёт.
- При выходе из игры `shutdown()` ждёт `joinExportThread()`. GD зависает, помогает только Force Quit.

**Что сделать.**
1. Обязательно добавить watchdog прогресса:
   - на каждом успешном `appendSampleBuffer` обновлять `lastProgress`;
   - если в ветке «нет готовых входов» прошло больше 10 с без прогресса, вызвать `releasePeeks()`, `[reader cancelReading]`, `[writer cancelWriting]` и вернуть `fail("remux stalled in segment k/n: writer inputs not ready for 10s")`.
2. Устранить саму причину, на выбор:
   - **(a)** минимальный вариант: выставить `in.expectsMediaDataInRealTime = YES` у passthrough-входов в `concatenateImpl`. Тогда `readyForMoreMediaData` опускается только при реальной нехватке пропускной способности, а не ради чередования дорожек;
   - **(b)** надёжный вариант: обрабатывать каждую дорожку как непрерывный поток через границы сегментов. Дорожка, дошедшая до EOF в сегменте k, сразу открывает свой reader в сегменте k+1. Базы сегментов (`outputBase`) считаются заранее по метаданным (`track.timeRange` / `asset.duration`), без барьера «сегмент целиком».
3. В обоих `finishWritingWithCompletionHandler` (`:1064-1068` и `:1593-1597`) заменить `DISPATCH_TIME_FOREVER` на таймаут (например, 60 с). По таймауту вызывать `cancelWriting` и возвращать ошибку.

**Готово, когда.** Новый host-тест в `tests/host/test_host_mac.mm` укладывается в 20 с. Сценарий: 3 сегмента, у среднего game-audio на 3 с короче видео. Склейка не виснет: она либо успешна (варианты a/b), либо падает с понятной ошибкой (если сделан только watchdog).

### M2. Временная неготовность AAC-входа считается ошибкой записи: рассинхрон и остановка записи

**Где.** `src/encoder/VideoToolboxEncoder.mm`:
- `:1007-1030` — 5 попыток по 1 мс, потом `"audio appendSampleBuffer returned NO"`;
- `:880-898` — то же для видео. Там это реже, потому что раньше срабатывает `isVideoQueueSaturated`.

`src/recorder/ReplayManager.cpp`:
- `:1149-1175` — разрыв заполняется тишиной до 120 с, кусками по 1 с подряд;
- `:1193-1207` — при `ToleratedError` PCM выбрасывается, а `sampleCounter` не растёт;
- `:964-1012` — после 5 ошибок подряд энкодер один раз пересоздаётся, при следующей серии запись останавливается.

**Проблема.** На Windows `IMFSinkWriter` с отключённым throttling складывает сэмплы во внутреннюю очередь. На macOS `AVAssetWriterInput` в realtime-режиме прямо сообщает «не готов», если AAC-энкодер не успевает за пачкой данных.

Такие пачки возникают в штатной работе:
- заполнение разрыва тишиной (до 120 с по 1 с подряд);
- догонка mic-mix;
- добивка тишиной при закрытии сегмента (`:1543-1580`).

Через 5 мс кусок считается ошибкой и выбрасывается. Получается петля:
1. Из-за выброшенного куска на следующей итерации снова разрыв больше 40 мс.
2. Снова идёт заполнение тишиной, снова вход не готов.
3. Звук уезжает раньше видео.
4. После 5 ошибок подряд энкодер пересоздаётся, сегмент рвётся.
5. На следующей пачке запись останавливается с «repeated game audio write failure».

Вдобавок укороченные аудиодорожки провоцируют M1.

**Что сделать.** Исправлять локально в macOS-энкодере, по аналогии с внутренней очередью MF.
1. Добавить в `VideoToolboxEncoder::Impl` очередь отложенных аудио-`CMSampleBuffer`, отдельно для game и mic. Если вход не готов, сэмпл кладётся в очередь, а не выбрасывается. Каждый `writeAudioLocked` и `finishLocked` сначала сливает очередь, пока `readyForMoreMediaData == YES`.
2. Ограничить очередь, например 10 с аудио на дорожку. При превышении ждать готовности с блокировкой: шагами по 1 мс, не дольше ~250 мс на вызов. Worker может себе это позволить: видео тем временем штатно дропается через `canAcceptEncoderFrame`. Ошибку возвращать, только если writer в `Failed` или очередь не сливается дольше 2 с.
3. В `finishLocked` перед `markAsFinished` дождаться опустошения очереди (с таймаутом).
4. Временную неготовность не возвращать как ошибку. Если отдельный статус всё же нужен, добавить в `EncoderOpResult` (`VideoEncoderTypes.hpp`) флаг `retryable` и не учитывать такие результаты в `handleEncoderWriteError`. MF-путь этот флаг не выставляет.
5. Для видео: `kVTVideoEncoderNotAvailableNowErr` («video encoder backpressure») тоже не считать ошибкой записи. Кадр нужно просто отбросить, как в `canAcceptEncoderFrame`.

**Готово, когда.** Host-тест пишет 120 с тишины кусками по 1 с подряд вперемешку с видеокадрами с реальными таймстемпами. Все записи успешны, длительность аудио в итоговом MP4 совпадает с записанной с точностью ±50 мс.

### M3. Разрешение на микрофон (TCC) не обрабатывается [проверить на Mac]

**Где.** `src/capture/AudioGrabberMac.mm:275-423`: перед `AudioQueueStart` (`:379`) авторизация не проверяется.

Описание PR #1 утверждает, что `AudioQueueNewInput` позволяет «избежать TCC-краша `AVCaptureDevice`, если в Info.plist нет `NSMicrophoneUsageDescription`». Это допущение не проверено: TCC (`kTCCServiceMicrophone`) действует на любой аудиовход, включая HAL и AudioQueue.

**Сценарии.**
- **Доступ запрещён (Denied/Restricted).** AudioQueue без ошибки отдаёт нули. В реплеях тихая mic-дорожка, и пользователь об этом ничего не узнаёт.
- **GD запущен не из Steam, а напрямую** (Finder, `.app` в `steamapps`). Для TCC «ответственным» процессом становится сам GD. В его Info.plist, скорее всего, нет `NSMicrophoneUsageDescription`, и TCC может убить процесс («attempted to access privacy-sensitive data without a usage description»). Запись автостартует при загрузке мода, поэтому с включённым «Record Microphone» игра будет падать при каждом запуске.
- **GD запущен из Steam.** Ответственным процессом, вероятно, окажется Steam, у которого ключ есть. Пользователь увидит запрос «Steam хочет доступ к микрофону».

**Что сделать (защитный фикс).**
1. Перед стартом очереди проверить `[AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]`. AVFoundation уже линкуется, а сам запрос статуса краша не вызывает.
   - `Denied` или `Restricted`: AudioQueue не запускать. Записать в лог. Один раз показать уведомление через `geode::queueInMainThread`, например: `Instant Replay: microphone access denied - allow it in System Settings > Privacy & Security > Microphone`. В попапе отметить, что микрофон недоступен.
   - `NotDetermined`: стартовать (система сама покажет запрос) и записать в лог предупреждение. `requestAccessForMediaType` не вызывать: ему тоже нужен purpose string.
   - `Authorized`: работать как сейчас.
2. Добавить детектор «цифровой тишины». Если 3 с подряд приходят буферы, в которых все сэмплы ровно `0.0f`, один раз за сессию предупредить пользователя. Это ловит и запрет TCC, и мёртвое устройство.

**Ручная проверка на Mac** (положить в описание PR):
1. Выполнить `tccutil reset Microphone`, запустить GD через Steam с включённым Record Microphone. Проверить, какой запрос появился, нет ли краша, пишется ли звук.
2. Повторить при запуске напрямую: двойной клик по `Geometry Dash.app` в `steamapps`.
   - Если процесс падает с `Namespace TCC`, нужен гейт: не стартовать захват, когда в `[NSBundle mainBundle]` нет `NSMicrophoneUsageDescription`, и показать уведомление «запустите GD через Steam».
3. Отказать в доступе. Должно появиться уведомление, а mic-дорожка не должна писаться молча.

---

## 3. P2 — важно

### M4. Retina: «Native» пишется в пунктах, а не в пикселях, и размер считается в двух местах по-разному

**Где.**
- `src/recorder/ReplayManager.cpp:692-707`: `startReplay()` берёт `CCEGLView::getFrameSize()`, а на macOS это пункты.
- `src/capture/FrameGrabber.cpp:544-572`: источник берётся из `getMacOpenGLDrawableSize()`, то есть в пикселях backing store. Если целевой размер равен 0, он тоже вычисляется из пикселей.
- `src/config/ReplayConfig.hpp:142-157`: `s_nativeFrameHeight` хранится в пикселях.
- `src/platform/MacPlatformUtils.mm:128-177`.

**Проблема.** Пример: MacBook, окно 1512×982 пт, drawable 3024×1964 px.
- `startReplay` выставляет цель 1512×982, и кадр 3024×1964 даунскейлится вдвое.
- При этом `s_nativeFrameHeight = 1964`. UI показывает битрейт и размер файла для 1964p, а энкодер получает битрейт, посчитанный от `m_videoHeight = 982`.
- Если сессия стартовала до того, как `CCEGLView` готов (автостарт при загрузке мода), цель вычисляет уже `FrameGrabber`, и уже в пикселях. В итоге разрешение «Native» зависит от того, как запустили запись.

Пресеты 720p/1080p/1440p/4K не затронуты: пропорции у пунктов и пикселей одинаковые.

**Что сделать.**
1. Завести один хелпер размера нативного фреймбуфера, например `ReplayConfig::getNativeFramebufferSize(int& w, int& h)`:
   - на macOS — `getMacOpenGLDrawableSize`;
   - на Windows — `CCEGLView::getFrameSize()`.

   Использовать его в `startReplay()`, `refreshNativeFrameHeight()` и `FrameGrabber::captureCurrentFrame()`. На Windows поведение не меняется.
2. **Обязательно вместе с п. 1:** защита от огромных Retina-размеров (iMac 5K 5120×2880, Pro Display XDR 6016×3384).
   - Сейчас, если HEVC-энкодер не тянет такой размер, каскад падает в H.264. H.264 отклоняет всё больше 4096×2304, и сессия не стартует вовсе (`VideoToolboxEncoder.mm:586-605`).
   - Нужно: если пиксельный «Native» больше 4096×2304, при старте сессии прогнать `probeVideoToolboxCodec` для выбранного семейства на этом размере. При отказе ужать до размера не больше 3840×2160 с сохранением пропорций и чётными сторонами, записать это в лог и показать пометку в UI.
3. Проверить падение FPS на Retina: readback 3024×1964 BGRA на 120 FPS — это около 2.8 ГБ/с.

**Готово, когда.**
- На Retina-маке «Native» даёт файл в пикселях backing store (проверить через `ffprobe`).
- Оценка размера в UI совпадает с фактическим размером ±15%.
- На 5K-мониторе запись стартует (с ужатым разрешением), а не падает.

### M5. Если выбранный микрофон пропал, нет перехода на системный

**Где.**
- `src/capture/AudioGrabberMac.mm:128-163`: на `:145` возвращается `kAudioObjectUnknown`.
- `:347-352`: результат `AudioQueueSetProperty(kAudioQueueProperty_CurrentDevice)` игнорируется.
- Эталон на Windows: `src/capture/AudioGrabber.cpp:484-507`.

**Проблема.**
- Пользователь выбрал USB-микрофон или AirPods, а устройство отключено или ещё не подключено. Поток раз в секунду бесконечно пробует снова и пишет пустую mic-дорожку, хотя системный вход исправен. Windows в такой ситуации берёт default endpoint.
- Если `SetProperty(CurrentDevice)` не сработал, очередь молча пишет с системного устройства вместо выбранного.

**Что сделать.**
1. В `resolveTargetInputDevice`: если UID не найден, устройство не живо или у него нет входных каналов, один раз записать в лог и перейти к поиску системного входа. Возвращать признак `usingFallback`.
2. При `usingFallback`:
   - не выставлять `CurrentDevice`;
   - зарегистрировать listener смены системного устройства, как в режиме `"default"`;
   - раз в 2–3 с или по событию `kAudioHardwarePropertyDevices` проверять, не вернулось ли выбранное устройство, и при возвращении перезапускать очередь на него.
3. Проверять `OSStatus` у `AudioQueueSetProperty(CurrentDevice)`. При ошибке писать в лог и считать это переходом на системное устройство.

### M6. Регрессия Windows: `path::filename().string()` бросает исключение

**Где.** `src/recorder/ReplayManager.cpp`:
- `:71` — `isSegmentFile`;
- `:306` — `parsePidFolderName(dirPath.filename().string(), ...)`.

**Проблема.** В MSVC `path::string()` перекодирует путь в ANSI code page и бросает `std::system_error`, если символ в ней непредставим. До порта здесь сравнивался `wstring`, и проблемы не было.

`cleanOrphanedPidDirs` перебирает подпапки базовых temp-каталогов. Если пользователь указал temp-папку с именем `InstantReplay` (тогда суффикс не добавляется), а в ней лежит, например, папка `Клипы` или папка с эмодзи в имени, исключение вылетает из `cleanupTempDir()`. Это происходит в `initialize()` (то есть в `$on_mod(Loaded)`) или в `shutdown()`. Заодно нарушен инвариант AGENTS.md §4.1.

**Что сделать.** В обоих местах заменить вызов на `geode::utils::string::pathToString(path.filename())`: он отдаёт UTF-8 на обеих платформах. Поискать в `src/` другие `.string()` на путях и тоже заменить.

### M7. Ошибки склейки после `startWriting` оставляют writer и reader открытыми

**Где.** `src/encoder/VideoToolboxEncoder.mm`:
- `:1315-1376` — missing track, cannot open, cannot read, cannot attach, cannot start reading;
- `:1492-1550` — ошибки timing, rebase и записи.

`cancelWriting` вызывается только на `:1567` и `:1582`.

**Проблема.** На этих путях writer остаётся в состоянии `Writing`, reader — в `Reading`. Обёртка `concatenateSegments()` удаляет итоговый файл, пока writer его держит и может продолжать запись. Ресурсы освобождаются только при уничтожении объектов.

**Что сделать.** Добавить в `.mm` RAII-гард:

```objc
struct RemuxCleanup {
    AVAssetWriter* writer;
    AVAssetReader* reader;
    bool committed;
    ~RemuxCleanup();
};
```

- В деструкторе при `!committed`:
  - если reader в `Reading`, вызвать `[reader cancelReading]`;
  - если writer в `Writing` или `Unknown`, вызвать `[writer cancelWriting]` (для `Failed`/`Completed` это no-op).
- При открытии каждого нового сегмента обновлять `guard.reader`.
- После успешного `finishWriting` выставлять `committed = true`.

### M8. Склейка: у passthrough-входа видео не задан `mediaTimeScale` [проверить на Mac]

**Где.** `src/encoder/VideoToolboxEncoder.mm:1186-1195`. Для сравнения: при записи сегментов задано `videoInput.mediaTimeScale = 1000000` (`:737`).

**Проблема.** Без явного значения AVAssetWriter выбирает «удобный» таймскейл, вплоть до грубого 600. Сегменты пишутся с шагом 1 мкс. На 120/144/240 FPS и с джиттером захвата округление до 1/600 с даёт неровные длительности кадров (микростаттер). Близкие кадры могут получить одинаковый PTS, и тогда `append` падает или кадр выпадает.

**Что сделать.** Для видео до `addInput` выставить:

```objc
in.mediaTimeScale = tr.naturalTimeScale > 0 ? tr.naturalTimeScale : kVideoMediaTimeScale;
```

Для аудио `mediaTimeScale` задавать нельзя: по документации это ошибка.

**Готово, когда.**
- `ffprobe -show_streams merged.mp4` показывает у видео `time_base=1/1000000`.
- В host-тесте на 240 FPS с джиттером ±1 мс у всех кадров склейки разные PTS.

### M9. Склейка: объекты всех сегментов живут до конца экспорта [проверить на Mac]

**Где.** `src/encoder/VideoToolboxEncoder.mm:1312-1579`. На весь экспорт есть только один autorelease pool: в `concatenateSegments()` (`:1618`) и `MediaThreadScope` в `ReplayManager`.

**Проблема.**
- `AVURLAsset`, `AVAssetReader`, track outputs и `NSURL` каждого сегмента могут дожить до конца экспорта.
- Часовой реплей — это примерно 122 сегмента.
- У GUI-приложений мягкий лимит в 256 открытых файлов, а часть уже занимают GD и Geode. Возможен `EMFILE` посреди экспорта («cannot read segment N»).
- Растёт и потребление памяти.

**Что сделать.**
- Обернуть в `@autoreleasepool { ... }` тело цикла по сегментам и блок сканирования первого сегмента.
- В конце каждой итерации явно вызвать `[reader cancelReading]`, если reader ещё читает, и обнулить `reader` и `segAsset`.

**Проверка на Mac.** Во время экспорта 60-минутного реплея следить за `lsof -p <pid> | wc -l`.

### M10. Склейка: при ребейзе смешиваются output-PTS и raw-PTS [проверить на Mac]

**Где.** `src/encoder/VideoToolboxEncoder.mm`:
- `:1399-1404` — `ptsHns` берётся из `CMSampleBufferGetOutputPresentationTimeStamp` и уже учитывает `TrimDurationAtStart`;
- `:1484-1531` — `sampleTime`, вычисленный из output-PTS, записывается в raw-поле `timings[0].presentationTimeStamp`.

**Проблема.**
- У сэмплов с trim-аттачментами (priming и частично обрезанные AAC-пакеты в начале каждого сегмента) output-PTS = raw-PTS + trim.
- Код записывает в raw-PTS значение, посчитанное от output-PTS, поэтому raw-время сдвигается на величину trim.
- На стыках каждые 30 с соседние AAC-пакеты перекрываются: возможны щелчки и небольшой сдвиг звука относительно видео.
- `recordWrittenSample` этого не замечает, потому что сравнивает output-времена.
- Если AVAssetReader отдаёт полностью обрезанные priming-пакеты с отрицательным output-PTS, условие `ptsHns < segCut` при `segCut = 0` их отбрасывает. Это допустимо, но решение должно быть осознанным.

**Что сделать.**
- Ребейзить через дельту: `delta = newOutputPts - origOutputPts` (`CMTime` в таймскейле дорожки), и прибавлять `delta` ко всем raw PTS/DTS в `timings`, сохраняя аттачменты.
- Для видео (trim там нет) результат совпадёт с текущим; скорректированный `newDts` из `adjustVideoTimestamps` нужно сохранить.
- В комментарии в коде описать политику для priming-пакетов в сегментах > 0.

**Готово, когда.**
- `ffprobe -show_packets -select_streams a:0` на склейке из 10+ сегментов показывает непрерывные PTS аудио на стыках: без перекрытий и без дыр больше одного пакета.
- В конце 10-минутного реплея звук и видео расходятся не больше чем на 1 кадр.

### M11. Хук `flushBuffer` переключает GL-контекст и не возвращает прежний

**Где.** `src/platform/MacPlatformUtils.mm:188-194`.

**Проблема.**
- Если в момент `flushBuffer` контекст GD не текущий (текущим может быть контекст оверлея или ImGui-мода, а может не быть никакого), хук вызывает `[self makeCurrentContext]`, захватывает кадр и выходит, не вернув прежний контекст.
- Контекст GD остаётся текущим, и последующие GL-вызовы чужого кода уходят не в тот контекст.
- Если `flushBuffer` вызван не на потоке рендера, один и тот же контекст становится текущим сразу на двух потоках. Для CGL это неопределённое поведение.

**Что сделать.**
1. Сохранить `prev = CGLGetCurrentContext()`.
2. Если `prev != [self CGLContextObj]`, выполнить: `CGLLockContext(ctx)`, `CGLSetCurrentContext(ctx)`, `cb()`, `CGLSetCurrentContext(prev)`, `CGLUnlockContext(ctx)`.
3. Один раз записать в лог, что переключение понадобилось. Это покажет, как ведёт себя реальный Mac.

Альтернатива: в такой ситуации просто пропускать захват кадра.

### M12. При выходе GL-ресурсы освобождаются без проверки текущего контекста [проверить на Mac]

**Где.**
- `src/recorder/ReplayManager.cpp:601`: `FrameGrabber::get()->cleanup(true)` вызывается из `$on_game(Exiting)`.
- `src/capture/FrameGrabber.cpp:190-225`.

**Проблема.**
- На macOS GL-вызов на потоке без текущего контекста разыменовывает null внутри GL-фреймворка и падает с `EXC_BAD_ACCESS`.
- Если GD рендерит не на том потоке, где срабатывает `Exiting` (например, на потоке CVDisplayLink; комментарий в `MacPlatformUtils.mm:215-216` такое допускает), то `glDeleteBuffers`/`glDeleteFramebuffers` упадут при выходе.
- В тот же момент возможна гонка: поток рендера как раз выполняет `captureCurrentFrame`.

**Что сделать.**
- На macOS в `resetGlResources(true)` удалять GL-объекты только если `CGLGetCurrentContext() == m_glContext`. Иначе пропустить удаление: процесс всё равно завершается.
- Защитить `captureCurrentFrame` и `cleanup` общим мьютексом или флагом «идёт захват», чтобы `cleanup` дожидался окончания текущего кадра.

**Ручная проверка.** Выйти из GD через Cmd+Q и через меню во время записи: crash-лога быть не должно.

---

## 4. P3 — низкий приоритет и UX

### M13. Папки `pid_*` от упавших сессий не удаляются при переиспользовании PID; lock-дескриптор наследуют дочерние процессы

**Где.** `src/recorder/ReplayManager.cpp`:
- `:339-357` — macOS;
- `:311-335` — та же логика на Windows;
- `:190` и `:339` — `::open` без `O_CLOEXEC`.

**Проблема.**
- Успешный `flock(LOCK_EX|LOCK_NB)` уже доказывает, что прежний владелец мёртв: flock снимается вместе со смертью процесса. Но после этого `isPidProcessAlive(otherPid)` отменяет очистку, если тот же PID занят любым другим процессом.
- На macOS PID не больше 99999 и быстро переиспользуются, особенно после перезагрузки. В итоге в `$TMPDIR` остаются гигабайты старых сегментов.
- Lock-fd открыт без `O_CLOEXEC`, поэтому его наследуют дочерние процессы, и лок держится после выхода GD.

**Что сделать.**
- Если flock удался, а PID жив, проверить, что этот процесс — Geometry Dash:
  - на macOS сравнить `proc_pidpath()` (`<libproc.h>`) с путём текущего процесса;
  - на Windows — `QueryFullProcessImageNameW`.
- Если это не GD, папку чистить.
- В оба вызова `open` добавить `O_CLOEXEC`.

### M14. Плашка «AV1 unsupported» видна всегда и вводит в заблуждение

**Где.** `src/ui/ReplayPopup.cpp:1439-1462`.

**Проблема.**
- AV1-энкодера в VideoToolbox нет ни на одном Mac, поэтому условие `!isPresetCodecSupported(AV1, hw)` всегда истинно.
- Плашка висит при любом выбранном кодеке, включая H.264.
- Она пишет «(using HEVC)», даже когда HEVC-железа нет и фактически используется H.264.

**Что сделать.** Убрать постоянную AV1-плашку. Если подсказка нужна, показывать её только у неактивной кнопки AV1. Фактический запасной кодек называть через `ReplayConfig::bestSupportedCodec(hw)`.

### M15. Уведомление о смене кодека: длинное тире и «this Mac» в общем коде

**Где.** `src/main.cpp:175-181`.

**Проблема.**
- Символа U+2014 нет в bitmap-шрифтах GD: глиф пропадает, в логе «character not defined».
- Блок кросс-платформенный, хотя на Windows до него выполнение никогда не доходит.
- В тексте выводится сырое значение `codecToString()` («hevc»).

**Что сделать.**
- Заменить тире на ASCII `-`.
- Обернуть блок в `#ifdef GEODE_IS_MACOS`.
- Имя кодека брать из `getCodecDisplayName()`.

### M16. Выключение HW молча меняет кодек; в SW-режиме выбирается HEVC SW вместо H.264 SW

**Где.** `src/ui/ReplayPopup.cpp:745-747`, `src/config/ReplayConfig.hpp:545-555`.

**Проблема.**
- `sanitizeCodecSettings()` молча переключает кодек, а при повторном включении HW прежний кодек не возвращается.
- `bestSupportedCodec(false)` предпочитает HEVC SW. Кодировать HEVC на CPU в реальном времени при 60+ FPS нереалистично: будут дропы кадров и переполнение очередей (см. M2).

**Что сделать.**
- Если `sanitizeCodecSettings()` вернул `true`, показывать в попапе уведомление с причиной (текст только ASCII).
- На macOS в SW-режиме предпочитать H.264 SW.
- Опционально: запоминать желаемый кодек и возвращать его при включении HW.

### M17. `openFolder` вызывается из фонового потока

**Где.** `src/recorder/ReplayManager.cpp:2908-2925`.

**Проблема.** На macOS `geode::utils::file::openFolder` работает через AppKit/NSWorkspace, а AppKit рассчитан на main thread.

**Что сделать.** На macOS проверку и создание каталога оставить в `spawnBlocking`, а сам `openFolder` вызывать через `geode::queueInMainThread`. Windows-ветку не трогать.

### M18. Нет цветовой разметки BT.709 [проверить на Mac]

**Где.**
- `src/encoder/VideoToolboxEncoder.mm:703-713`: в `videoSettings` нет `AVVideoColorPropertiesKey`.
- `src/capture/FrameGrabber.cpp:69-89`: у `CVPixelBuffer` нет цветовых аттачментов.

**Проблема.** VideoToolbox конвертирует BGRA в YUV по умолчанию. Файл без явной разметки разные плееры (QuickTime, браузеры, Discord) интерпретируют по-разному: цвета выглядят блёклыми или затемнёнными по сравнению с игрой и с записями на Windows.

**Что сделать.**
1. Добавить в `videoSettings` ключ `AVVideoColorPropertiesKey` со значениями `AVVideoColorPrimaries_ITU_R_709_2`, `AVVideoTransferFunction_ITU_R_709_2`, `AVVideoYCbCrMatrix_ITU_R_709_2`.
2. При аллокации пиксельных буферов навешивать такие же аттачменты: `CVBufferSetAttachment(..., kCVAttachmentMode_ShouldPropagate)`.
3. Заново прогнать `canApplyOutputSettings`.
4. Проверить: `ffprobe` показывает `color_primaries=bt709`; визуально сравнить кадр со скриншотом игры.

### M19. Obj-C исключения AVFoundation не перехватываются

**Где.** `src/encoder/VideoToolboxEncoder.mm`: `tryStartLocked`, `writeVideoFrame`, `writeAudioLocked`, `finishLocked`, `concatenateImpl`.

**Проблема.** Часть методов AVFoundation бросает `NSException`, если статус writer или reader сменился между проверкой и вызовом. Это `startSessionAtSourceTime:`, `addInput:`, `markAsFinished`, `addOutput:`, а также `appendSampleBuffer:` при несовпадении типа данных или вызове вне сессии. Исключение, прошедшее через C++-кадры, завершает GD.

**Что сделать.**
- На границе C++ API обернуть тела этих функций в `@try { } @catch (NSException* e) { ... }` и превращать исключение в `EncoderOpResult`/`ConcatResult` с текстом из `e.reason`.
- Для этих файлов можно включить `-fobjc-arc-exceptions`, чтобы ARC не терял объекты при исключении.

### M20. Неудачная установка хука захвата не видна пользователю

**Где.** `src/platform/MacPlatformUtils.mm:226-236`, `src/main.cpp:91-97`.

**Проблема.** Если `replaceObjcMethod` не сработал, это видно только в логе. Запись формально «идёт», но видео нет. Сегменты без видео отбрасываются, и на F6 пользователь получает «replay is empty».

**Что сделать.**
- `installMacGLCaptureHook` должен возвращать `bool`; добавить `isMacGLCaptureHookInstalled()`.
- При неудаче показать уведомление с main thread и не запускать запись (или явно показать этот статус в попапе).

### M21. Документация и описания утверждают, что мод только для Windows

**Где.**
- `AGENTS.md`: §1 (Target Platform), §2 (в файловой карте нет новых файлов), §5 («does not build on Linux… CMakeLists.txt intentionally errors on non-WIN32»).
- `README.md:4,16-22,58`.
- `about.md:4` («Platform: Windows 64-bit»), а также `:21,30,38,81` (NVENC/AMF/QSV, WASAPI, Explorer).
- `mod.json`: описание `hardware-accel` упоминает только NVENC/AMF/QuickSync.

**Что сделать.**
1. Обновить AGENTS.md:
   - список платформ;
   - файловую карту: `src/platform/*`, `VideoToolboxEncoder.*`, `AudioGrabberMac.mm`, `PlatformEncoder.hpp`, `VideoEncoderTypes.hpp`, `tests/host/test_host_mac.mm`;
   - описание data flow на macOS;
   - инварианты macOS:
     - ARC только в `.mm`;
     - autorelease pool на фоновых потоках через `MacAutoreleasePoolScope`/`MediaThreadScope`;
     - на macOS строки в `CVPixelBuffer` идут сверху вниз;
     - захват через swizzle `flushBuffer`;
     - локи через `flock`;
   - команды сборки и запуска mac host-тестов.
2. Обновить README и about.md.
3. В `mod.json` переписать описание `hardware-accel`: «NVENC/AMF/QuickSync on Windows, VideoToolbox on macOS».
4. Добавить запись в changelog.

### M22. Мёртвый блок в CMakeLists.txt

**Где.** `CMakeLists.txt:6-13`.

**Проблема.** `APPLE` определяется только внутри `project()`, поэтому блок до него никогда не выполняется. Универсальная сборка при этом работает:
- в логе CI `build-mac` собираются оба PCH, `cmake_pch_arm64.hxx.pch` и `cmake_pch_x86_64.hxx.pch`;
- `-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0` передаёт `geode-sdk/build-geode-mod`.

Блок только вводит в заблуждение, а PR его ещё и упоминает как фичу.

**Что сделать.** Перед `project()`, без проверки `APPLE`, как в шаблоне Geode:

```cmake
if (NOT "${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" AND NOT IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
```

Или просто удалить блок. После этого убедиться, что CI по-прежнему собирает обе архитектуры: `lipo -archs` на `.dylib` из артефакта.

---

## 5. Опционально: производительность и дублирование кода

Делать отдельным PR, не смешивая с багфиксами.

- **R1.** `VideoToolboxEncoder.mm:631-637`: каждый `tryStartLocked` через `probeVideoToolboxCodec` создаёт, подготавливает и уничтожает лишнюю `VTCompressionSession`. Это происходит на каждой ротации сегмента (раз в 30 с) и на каждом предварительном запуске следующего энкодера. Кешировать результат проверки по ключу (family, hw, w×h, rc) рядом с `s_cachedRequestKey`.
- **R2.** `FrameGrabber.cpp:545`: `getMacOpenGLDrawableSize()` вызывается на каждом захваченном кадре (autorelease pool и 4–5 Obj-C вызовов). Кешировать и обновлять при изменении размера.
- **R3.** `VideoToolboxEncoder::start()` (`:372-606`) почти построчно повторяет каскад фоллбэков из `MediaFoundationEncoder::start()`. Вынести в общий шаблон с колбэком `tryStart`, чтобы правила кеша кодека не разъехались между платформами.
- **R4.** Блок «writer failed → `classifyNSError` → `EncoderOpResult`» скопирован примерно 16 раз. Вынести в хелперы `writerFailure(AVAssetWriter*)` и `nsErrorMessage(NSError*)`.
- **R5.** `writeAudioLocked` (`:971-974`) на каждый вызов создаёт новый `std::vector<float>`. Переиспользовать буфер.
- **R6.** `writeAudioLocked` (`:991-996`) следит за монотонностью только по началу буфера (`lastTicks + 1`). Хранить конец буфера (`pts + numSamples`) и логировать перекрытия.

---

## 6. Проверено — НЕ трогать

- **Универсальный бинарь собирается.** Лог CI `build-mac` подтверждает arm64 + x86_64 и deployment target 11.0. Блок в CMakeLists мёртвый (M22), но сама сборка корректна.
- **MF-путь после рефакторинга.** `fatalStorage` выставляется через `makeMfFailure()` во всех ошибках записи и старта. Новый маппинг принудительно выбранного кодека (`guidForCodecFamily`/`familyForGuid`) эквивалентен старому варианту на `GUID`.
- **Windows-ветка `FrameGrabber`.** `copyBgraRows`/`resizeNearestStrided` с `flip=false` эквивалентны прежним `memcpy`/`resizeNearest`.
- **Формат пикселей на macOS.** `GL_BGRA` + `GL_UNSIGNED_INT_8_8_8_8_REV` на little-endian дают в памяти порядок B,G,R,A, то есть `kCVPixelFormatType_32BGRA`. Перевёрнутый blit даёт строки сверху вниз; это покрыто Test 2 в `test_host_mac.mm`.
- **Переиспользование `CVPixelBuffer`.** Пул выдаёт буфер, только когда `CFGetRetainCount == 1`. Кадры, которые ещё держит AVAssetWriter, учитываются в `m_inFlightFrames`.
- **`flushTrailingSilenceForClose`** пишет с `ts = 0` только в пустую дорожку, перекрытий нет.
- **`m_micResampler.reset()`** вызывается только после уничтожения AudioQueue (`AudioQueueDispose(..., true)`), гонки с её колбэком нет.

---

## 7. План проверки

1. После каждого коммита пушить ветку. Джобы `build` (Win64) и `build-mac` должны быть зелёными.
2. Добавить в `tests/host/test_host_mac.mm`:
   - **T-M1.** Склейка 3 сегментов, у среднего game-audio на 3 с короче видео. Тест завершается быстрее 20 с. Поставить `alarm()` или watchdog-поток, чтобы тест падал, а не висел.
   - **T-M2.** 120 с тишины кусками по 1 с подряд вперемешку с видеокадрами. Все записи `ok`, длительность аудио в файле ±50 мс.
   - **T-M8.** 2 сегмента по 2 с на 240 FPS с джиттером таймстемпов. В склейке у всех кадров разные PTS, у видео `naturalTimeScale == 1000000`.
   - **T-M10.** Склейка 10 сегментов по 1 с. Аудиопакеты на стыках не перекрываются (больше чем на 1 сэмпл) и не имеют дыр (больше 1 пакета).
3. Описать в AGENTS.md команду сборки mac host-тестов и добавить CI-джобу `host-tests-mac` (`runs-on: macos-latest`). На раннерах-ВМ нет аппаратных энкодеров, поэтому проверки, завязанные на железо, должны корректно уходить в SW-фоллбэк или пропускаться по результату `getMacVideoEncoderCaps()`. Предлагаемая команда (поправь, если линковке понадобятся ещё исходники или фреймворки):
   ```bash
   mkdir -p build && clang++ -std=c++23 -fobjc-arc -DGL_SILENCE_DEPRECATION \
     -Isrc -Itests/host/mock_geode \
     tests/host/test_host_mac.mm src/encoder/VideoToolboxEncoder.mm src/platform/MacVideoCapabilities.mm \
     -framework Foundation -framework AVFoundation -framework VideoToolbox -framework CoreMedia \
     -framework CoreVideo -framework CoreAudio -framework AudioToolbox -framework CoreFoundation -framework OpenGL \
     -o build/test_host_mac && ./build/test_host_mac
   ```
4. Ручной чек-лист на реальном Mac: Apple Silicon и, если есть, Intel. Положить в описание PR.
   - **Retina «Native».** Разрешение файла равно пикселям backing store. На 5K-мониторе запись стартует (M4).
   - **Стыки сегментов.** Реплей 10+ минут с периодическим звуком и вспышкой. На стыках раз в 30 с нет щелчков, в конце рассинхрон звука и видео не больше 1 кадра (M10).
   - **240 FPS.** Кадры в склейке идут ровно (M8).
   - **Микрофон.**
     - отказ в доступе → появляется уведомление;
     - отключение выбранного USB-микрофона → переход на системный;
     - запуск GD через Steam и напрямую (M3, M5).
   - **Выход.** Cmd+Q во время записи и во время сохранения: нет зависаний и падений (M1, M12).
   - **Длинный реплей.** Сохранение 60-минутного реплея проходит успешно, число открытых файлов по `lsof` в норме (M9).
   - **Оверлеи.** Мод-оверлеи (если есть на macOS) попадают в запись так же, как на Windows. На Windows порядок явно задан через `setHookPriorityAfter("absolllute.megahack")`, а на macOS он зависит от порядка установки swizzle.

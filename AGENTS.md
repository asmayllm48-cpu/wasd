# WASD / NEXPLAY — Agent Notes

## Build & Test
- Build: `cmd //c build.cmd` from repo root (configures + builds + runs CTest).
- Output: `build\wasd.exe`. Close a running `wasd.exe` before building or linking fails.
- Tests: doctest suite via `wasd_tests.exe` / CTest (`1/1 wasd_tests`).
- Bench binary: `wasd-bench.exe` builds alongside tests.

## Architecture
- `src/core/` — input engine, automation model (`automation.h/.cpp`), profile (de)serialization, coords, imgmatch. No ImGui here.
- `src/core/studio_*` — NEXPLAY Automation Studio core (pure C++20, no ImGui): `studio_graph` (graph model, ports, JSON + migration), `studio_engine` (`GraphRunner`, tick-driven, non-blocking; `GraphHost` sinks wire it to the app), `studio_converter` (`macroToGraph` / `graphToMacroSteps`, non-destructive + review notes).
- `src/app/` — `app.cpp/.h` (frame loop, input thread), `ui.cpp` (mapping/settings panels + widget helpers), `editor.cpp` (capture/discard/profile ops), `automation_ui.cpp` (rules/zones/images/macro editor), `automation_runtime.cpp` (scan + dispatch + graph trigger keys), `studio_ui.h` + `studio_canvas.cpp` + `studio_panels.cpp` (the Studio workspace; `StudioEditor` talks to the engine only through `runtime` std::function hooks).
- `src/render/` — D3D11 renderer + async region readback used by zone/image scanning (`kRegionSlots = 17`; slots 0-9 zones, 10-13 images, 14 color pick, 15 image capture, 16 studio graph probe).
- `src/session/` — scrcpy socket + control queue (coalesces MOVE, drops stale).
- `tests/` — one doctest file per subsystem; `test_main.cpp` hosts the runner.

## Conventions
- i18n: `i18n::tr("...")` / `i18n::trf(fmt, ...)`; `T()`/`V()` are local shorthands. New UI text must go through them (Arabic + English, RTL).
- UI helpers (`S()` scaling, `cardBg()`, `buttonRow`, `title`, `sliderNum*`, `intField`, `keyName`) live in the anonymous namespace of `ui.cpp` — they are NOT visible from other .cpp files. `automation_ui.cpp` has its own small set.
- `inputMu_` MUST be held for every `input_` (InputEngine) access on the UI thread — the dedicated input thread ticks it concurrently.
- Profile persistence: only explicit Save writes `rr.json`. `profile_` is the live draft; `saved_` the last-saved copy. Guarded navigation goes through `requestDiscard(std::function<void()>)` → `drawDiscardPrompt()` (Save / Don't save / Cancel). "Don't save" restores `profile_ = saved_` via `discardChanges()`.
- Automation: `Monitor` = a "rule" (zone/image watch → action). Runtime switch per rule: `MonitorRuntime::switchedOn` (armed by `Monitor.toggleKey`, works even when the global `automationOn_` is off). `setAutomation(false)` resets all runtimes (emergency stop disarms rules).
- Monitor else-branch: `elseMacro/elseKey` fire every `elseCooldownMs` after `absentMs` of confirmed absence; `elseStop` disarms the rule. `MonitorRuntime::onAbsent` implements the timing; the app calls it only when `onSample` did not fire.
- Region picking: `pickMode_` 1=color(s), 2=draw zone, 3=point→`pickTarget_`, 4=image crop, 5=image search region, 6=rect→`pickRectTarget_`.
- Studio graphs live in `Automation::studioGraphs` (saved with the profile, omitted from JSON when empty so old files stay byte-identical). Graph trigger keys (`metadata.triggerKey/triggerKey2`) dispatch in `automationKey` with the same dual-key semantics as macros.
- `addNode()` returns a reference that dangles after `nodes` vector growth — always keep node ids and reacquire via `findNode` before connecting.

## Schema
- Automation JSON serializes inside the profile (`automationToJson`/`automationFromJson` in `automation.cpp`). New fields must be written AND read with `o.value(name, default)` so older files load unchanged; validate in `validateAutomation`.

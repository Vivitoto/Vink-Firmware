# Vink v0.3.2-rc runtime scaffold

This directory is the new PaperS3 firmware line. The goal is to abandon the v0.2.x monolithic `App::run()` architecture and rebuild around ReadPaper's proven low-level model:

- `runtime/` starts the new services.
- `display/` owns the physical e-paper push queue.
- `input/` polls touch and converts raw events into semantic messages.
- `state/` owns state transitions through a FreeRTOS queue.
- `ui/` draws Vink UI to a full-screen canvas only.
- `sync/` will host Legado remote reading progress sync as a service.

Do not add direct `M5.Display.display()` / `pushSprite()` calls in UI or state code. Physical display writes belong in `display/DisplayService`.

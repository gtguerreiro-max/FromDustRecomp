# PC settings validation — 2026-11-01

The Clang Windows build completed successfully. `RuntimeSettingsInput` passed
through CTest, covering settings reload and invalid values, key swaps and reserved
shortcuts, keyboard button/axis translation, bounded diagonal movement, blocked
input, controller-index errors, the Xbox big-endian layout and an empty event
queue.

Interactive checks in the running game window:

- F1 opened and closed the Portuguese overlay over the animated title screen.
- Showing FPS wrote `ShowFPS=1` to the isolated test INI.
- Start was changed from Enter to P. The new assignment survived a restart;
  pressing P with the panel closed reached the game's local-profile request.
  That original profile/sign-in flow is still a stub, so no authentication or
  gameplay progression was tested.
- F11 switched to borderless fullscreen and back to the original window size.
- The restart button launched a new process and restored saved settings.
- The corrected 30 FPS limit produced steady 30 FPS samples in the title scene.
- FSR presentation changed immediately. The live window size changed from
  1280x720 to 1600x900 without interrupting rendering.
- The controls list scrolled while the tab bar stayed visible.

Evidence: `build/settings-panel.jpg`, `build/validation-settings.ini`,
`build/validation-settings-ui.log` and `build/validation-settings-ui-gpu.log`.
The test INI was separate from the user's normal `build/user/settings.ini`.

A separate 65-second offscreen run with Scale=2 produced a visually verified
2048x1200 title scene in `build/settings-scale2.png`, with no recorded crashes.
Logs: `build/validation-settings-scale2.log` and the matching `-gpu.log` file.
The 3× scale and physical controller behavior still need testing; audio and full
gameplay remain outside the completed settings-panel work.

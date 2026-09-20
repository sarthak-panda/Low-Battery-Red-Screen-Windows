LOW BATTERY RED
===============
Screen turns red when the battery is LOW and NOT CHARGING.

INSTALL   Double-click LowBatteryRed.exe (or Install.bat). Done.
          - If Windows SmartScreen appears: More info -> Run anyway
            (the file is unsigned, that is the only reason for the warning).
TRY IT    Double-click Test.bat  -> red screen for 6 seconds.
REMOVE    Double-click Uninstall.bat

WHAT IT DOES
  * Battery <= 20% and charger NOT connected -> whole screen (all monitors) turns red.
  * Charger plugged in, or battery above 20% -> red goes away instantly.
  * Never takes focus, never blocks the mouse/keyboard: clicks and typing go
    straight to the app underneath. Not shown in taskbar / Alt-Tab.
  * Starts automatically at every sign-in.
  * If it crashes or freezes it is restarted automatically within ~1-2 seconds.
    If someone kills one of its two processes, the other revives it.
  * Desktop PCs without a battery: never shows anything.
  * Tiny: ~34 KB, ~1-2 MB RAM, no CPU use.

SETTINGS  (edit, save - applied within 2 seconds, no restart)
  %LOCALAPPDATA%\LowBatteryRed\config.ini
      Threshold = 20    battery % at/below which the screen goes red (1-99)
      Opacity   = 55    tint strength 10-100  (100 = solid red, no see-through)

QUICK REAL-WORLD TEST
  Set Threshold=99 in config.ini, unplug the charger -> screen goes red.
  Plug it back in -> red disappears. Then set Threshold back to 20.

LIMITS
  * Games/videos in true "exclusive fullscreen" mode can draw above any overlay.
    (Borderless / windowed fullscreen is fine.)
  * Windows' own lock screen and UAC prompts are separate secure desktops.

Source code, build instructions and tests:
  https://github.com/sarthak-panda/Low-Battery-Red-Screen-Windows

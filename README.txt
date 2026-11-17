===============================================================================
 TR2013 HUD Fix+ 1.4.5 - Tomb Raider (2013) / GOTY - HUD position fix
 Unofficial community fix. Not affiliated with Crystal Dynamics or Square Enix.
===============================================================================

ENGLISH
-------
Supported game version:
  Tomb Raider (2013) / GOTY, current PC version: Patch 838,
  Steam build 9573671 (27 Sep 2022). Tested on Steam.
  - Epic Games Store: same version and same bugs; should work (not tested).
  - Patch 829 / 838 before Sep 2022: same HUD behaviour; should work.
  - Steam beta branch "build743.0": does not have these bugs and does not
    need this fix.
  - Quick check: if EOSSDK-Win32-Shipping.dll is in your game folder, you
    have an affected version. (The launcher shows v1.01.0.0 for all builds.)

What it fixes (current game build, Steam and Epic):
  1. Notifications at the bottom right (weapon upgrade available, skill point,
     "find a base camp", ...) are pushed partly off-screen.
  2. A dark "stain" sometimes appears at the top of the screen when you pick
     up ammo or salvage (its background is drawn at the wrong position).
  3. The salvage counter is shown in the top-left corner without its "SALVAGE"
     label; it is moved back to the top right (can be disabled in the menu).
  4. Notifications and "Press [E]" prompts sometimes appear in the top-left
     corner instead of their normal place; they are moved back.
  Previously the only workaround was downgrading to Steam build 743, which is
  not possible on Epic.

How it works:
  Fixes are applied during gameplay only; menus are never modified.
  The fix only changes how a few HUD elements are drawn (DirectX 11 graphics
  data). TombRaider.exe, steam_api.dll and EOSSDK-Win32-Shipping.dll are never
  modified. No game files are patched.

Install:
  1. Open the game folder (the folder containing TombRaider.exe).
     Steam: right-click the game > Manage > Browse local files.
     Epic:  Library > ... > Manage > Installation folder (open folder).
  2. Copy d3d11.dll and tr_hud_fix.ini into that folder.
  3. Start the game.

Uninstall:
  Delete d3d11.dll and tr_hud_fix.ini from the game folder.

In-game controls:
  INSERT  open/close the settings menu (mouse cursor shown while open)
  F9      quick toggle of the fix on/off (for comparison)
  ESC     close the menu
  The menu has tabs:
    FIX          Enable fix, Backdrop (Off/Move/Hide), Fix prompt position,
                 Fix popup in top-left, Salvage counter top-right
                 (with ShowAdvanced=1 in the ini also: POPUP Offset X/Y, Scale)
    PERFORMANCE  FPS limit, FPS overlay, frame time statistics and graph
    DISPLAY      Borderless, Flip model, Allow tearing (need a game restart)
    HOTKEYS      change keys (Change / Clear)
    INFO         version, resolution, statistics
  "Reset to defaults" and "Save to ini" are always visible at the bottom.

Performance / FPS:
  - Two tear-free setups for a steady 60 FPS:
    a) Exclusive fullscreen + in-game VSync (double buffer) + FPS limit 60
       (best at 120 Hz: 60 divides the refresh rate evenly).
    b) Borderless window (launcher: Fullscreen on, Exclusive off) + VSync off
       + FPS limit 60. Windows synchronises the image, so no tearing.
  - Exclusive fullscreen with VSync off always tears - the FPS limiter does
    not replace VSync.
  - In borderless mode the Windows desktop refresh rate is used (the in-game
    refresh rate setting has no effect).
  - The menu option "Borderless (experimental)": the game still minimizes on
    Alt-Tab. Prefer the launcher setting from b).

Notes:
  - The popup offset is resolution-relative (tested resolutions: see
    "Compatibility"). On ultrawide (21:9/32:9, untested) or unusual setups,
    set ShowAdvanced=1 in tr_hud_fix.ini, adjust "Offset X" in the menu if
    needed and press "Save to ini" (or edit ShiftNdcX in the ini).
  - Windows display scaling (DPI) does not cause this bug; no DPI settings are
    required.
  - Hotkeys can be changed in the menu (HOTKEYS) or in tr_hud_fix.ini
    (MenuHotkey, ToggleHotkey) as key names (F9, INSERT, PAGE UP, NUM 5 ...)
    or virtual-key codes. MenuHotkey=0 disables the menu completely.

Using it together with DXVK, ReShade or another d3d11.dll:
  - Rename the existing d3d11.dll to d3d11_orig.dll (or dxvk_d3d11.dll for
    DXVK) and then copy this d3d11.dll into the folder. It is loaded
    automatically ("chain loading"). You can also set ProxyDll=<file name> in
    tr_hud_fix.ini.
  - ASI loaders (version.dll / winmm.dll / dinput8.dll) do not work with this
    game: it loads those DLLs from the Windows folder. Use d3d11.dll.

Compatibility:
  Tested:            Steam version; 16:9 (2560x1440, 1920x1080, 1280x720),
                     16:10 (1680x1050), 5:4 (1280x1024), 4:3 (1024x768,
                     800x600); exclusive fullscreen; Steam overlay.
  Should work:       Epic version; ultrawide 21:9 / 32:9; ReShade / DXVK via
                     chain loading (see above); overlays (Discord,
                     RTSS/Afterburner, NVIDIA/AMD, OBS). Not tested yet -
                     feedback welcome.
  Not recommended:   Special K (hooks Direct3D 11 itself; may conflict).
  Other HUD/widescreen tools may change the layout: adjust "Offset X"
  (ShowAdvanced=1).
  If a future game update changes the HUD shaders, the fix simply does
  nothing (no crash).

Troubleshooting:
  - Menu does not open / problems with the menu: set MenuHotkey=0 in
    tr_hud_fix.ini. The fix keeps working.
  - Logging is off by default (EnableLogging=0: no log file, no extra work).
  - For bug reports: preferably set EnableLogging=2 in tr_hud_fix.ini
    (watchdog: small log that records HUD errors and crashes automatically).
    EnableLogging=1 writes a full analysis log. The game folder will then
    contain tr_hud_fix.log (size limit MaxLogMB, default 50 MB; set it back
    to 0 afterwards). The log of the previous start is kept as
    tr_hud_fix.prev.log - after a crash, please send that file.
  - EnableLogging=2 also records the memory usage once per minute ([MEM]
    lines) - useful for reports about "Out of memory" errors in long sessions
    (a known engine issue of the game since 2014, not caused by this fix).
  - EnableLogging=2 also writes a list of the HUD elements seen ([INV] lines)
    - this helps a lot with reports about misplaced HUD elements.
  - Microsoft Store / Game Pass version: not tested.

Credits / licences:
  TR2013 HUD Fix+ - MIT licence (licenses\TR_HUD_Fix_LICENSE.txt)
  Dear ImGui by Omar Cornut - MIT licence (licenses\DearImGui_LICENSE.txt)
  The menu uses the Windows system font "Bahnschrift" if available (not
  included).


DEUTSCH
-------
Unterstuetzte Spielversion:
  Tomb Raider (2013) / GOTY, aktuelle PC-Version: Patch 838,
  Steam-Build 9573671 (27.09.2022). Getestet mit Steam.
  - Epic Games Store: gleiche Version, gleiche Fehler; sollte funktionieren
    (nicht getestet).
  - Patch 829 / 838 vor Sept. 2022: gleiches HUD-Verhalten; sollte
    funktionieren.
  - Steam-Beta-Zweig "build743.0": hat diese Fehler nicht und braucht den Fix
    nicht.
  - Schnelltest: Liegt EOSSDK-Win32-Shipping.dll im Spielordner, hast du eine
    betroffene Version. (Der Launcher zeigt bei allen Builds v1.01.0.0.)

Was behoben wird (aktueller Spiel-Build, Steam und Epic):
  1. Meldungen unten rechts (Waffenmodifizierung verfuegbar, Fertigkeitspunkt,
     "Finde ein Basislager", ...) werden teilweise aus dem Bild geschoben.
  2. Beim Aufsammeln von Munition oder Bergungsgut erscheint oben im Bild
     manchmal ein dunkler "Fleck" (sein Hintergrund wird falsch platziert).
  3. Der Bergungsgut-Zaehler wird oben links ohne seine Beschriftung
     "BERGUNGSGUT" angezeigt; er wird wieder nach oben rechts verschoben
     (im Menue abschaltbar).
  4. Meldungen und "Druecken Sie [E]"-Hinweise erscheinen manchmal oben links
     statt an ihrem normalen Platz; sie werden zurueckgesetzt.
  Bisher half nur das Downgrade auf Steam-Build 743, das auf Epic nicht
  moeglich ist.

Funktionsweise:
  Korrekturen wirken nur im Spiel; Menues werden nie veraendert.
  Der Fix aendert nur, wie einige HUD-Elemente gezeichnet werden (DirectX-11-
  Grafikdaten). TombRaider.exe, steam_api.dll und EOSSDK-Win32-Shipping.dll
  werden nie veraendert. Es werden keine Spieldateien gepatcht.

Installation:
  1. Spielordner oeffnen (der Ordner mit TombRaider.exe).
     Steam: Rechtsklick auf das Spiel > Verwalten > Lokale Dateien durchsuchen.
     Epic:  Bibliothek > ... > Verwalten > Installationsordner oeffnen.
  2. d3d11.dll und tr_hud_fix.ini in diesen Ordner kopieren.
  3. Spiel starten.

Deinstallation:
  d3d11.dll und tr_hud_fix.ini aus dem Spielordner loeschen.

Tasten im Spiel:
  EINFG   Einstellungsmenue oeffnen/schliessen (Mauszeiger wird angezeigt)
  F9      Fix schnell ein/aus (zum Vergleichen)
  ESC     Menue schliessen
  Das Menue (englisch) hat Reiter:
    FIX          Enable fix, Backdrop (Off/Move/Hide), Fix prompt position,
                 Fix popup in top-left, Salvage counter top-right
                 (mit ShowAdvanced=1 in der ini auch: POPUP Offset X/Y, Scale)
    PERFORMANCE  FPS-Begrenzung, FPS-Anzeige, Frametime-Statistik und Graph
    DISPLAY      Randlos, Flip-Modell, Tearing (erst nach Spiel-Neustart)
    HOTKEYS      Tasten aendern (Change / Clear)
    INFO         Version, Aufloesung, Statistik
  "Reset to defaults" und "Save to ini" sind unten immer sichtbar.

Leistung / FPS:
  - Zwei Varianten fuer gleichmaessige 60 FPS ohne Tearing:
    a) Exklusives Vollbild + VSync im Spiel (Doppelpuffer) + FPS limit 60
       (am besten bei 120 Hz: 60 teilt die Bildwiederholrate glatt).
    b) Randloses Fenster (Launcher: Vollbild an, Exklusiv aus) + VSync aus
       + FPS limit 60. Windows synchronisiert das Bild, also kein Tearing.
  - Exklusives Vollbild mit VSync aus reisst immer - die FPS-Begrenzung
    ersetzt kein VSync.
  - Im randlosen Modus gilt die Bildwiederholrate des Windows-Desktops (die
    Hz-Einstellung im Spiel wirkt dann nicht).
  - Menue-Option "Borderless (experimental)": das Spiel minimiert sich bei
    Alt-Tab weiterhin. Besser die Launcher-Einstellung aus b) nutzen.

Hinweise:
  - Der Popup-Versatz ist relativ zur Aufloesung (getestete Aufloesungen:
    siehe "Kompatibilitaet"). Bei Ultrawide (21:9/32:9, nicht getestet) oder
    ungewoehnlichen Setups ggf. ShowAdvanced=1 in tr_hud_fix.ini setzen,
    "Offset X" im Menue anpassen und "Save to ini" druecken (oder ShiftNdcX
    in der ini aendern).
  - Die Windows-Anzeigeskalierung (DPI) ist nicht die Ursache dieses Fehlers;
    es sind keine DPI-Einstellungen noetig.
  - Tasten koennen im Menue (HOTKEYS) oder in tr_hud_fix.ini geaendert
    werden (MenuHotkey, ToggleHotkey) - als Tastenname (F9, EINFG/INSERT,
    BILDAUF/PAGE UP, NUM 5 ...) oder Virtual-Key-Code. MenuHotkey=0 schaltet
    das Menue ab.

Zusammen mit DXVK, ReShade oder einer anderen d3d11.dll:
  - Vorhandene d3d11.dll in d3d11_orig.dll umbenennen (bei DXVK auch
    dxvk_d3d11.dll moeglich) und dann diese d3d11.dll in den Ordner kopieren.
    Sie wird automatisch nachgeladen ("Chain-Loading"). Alternativ
    ProxyDll=<Dateiname> in tr_hud_fix.ini setzen.
  - ASI-Loader (version.dll / winmm.dll / dinput8.dll) funktionieren bei
    diesem Spiel nicht: es laedt diese DLLs aus dem Windows-Ordner. Bitte
    d3d11.dll verwenden.

Kompatibilitaet:
  Getestet:          Steam-Version; 16:9 (2560x1440, 1920x1080, 1280x720),
                     16:10 (1680x1050), 5:4 (1280x1024), 4:3 (1024x768,
                     800x600); exklusives Vollbild; Steam-Overlay.
  Sollte gehen:      Epic-Version; Ultrawide 21:9 / 32:9; ReShade / DXVK per
                     Chain-Loading (siehe oben); Overlays (Discord,
                     RTSS/Afterburner, NVIDIA/AMD, OBS). Noch nicht getestet -
                     Rueckmeldungen willkommen.
  Nicht empfohlen:   Special K (hakt sich selbst in Direct3D 11 ein; moegliche
                     Konflikte).
  Andere HUD-/Widescreen-Tools koennen das Layout veraendern: "Offset X"
  anpassen (ShowAdvanced=1). Aendert ein kuenftiges Spiel-Update die HUD-Shader, tut der Fix
  einfach nichts (kein Absturz).

Problembehebung:
  - Menue oeffnet nicht / Probleme mit dem Menue: MenuHotkey=0 in
    tr_hud_fix.ini setzen. Der Fix funktioniert weiter.
  - Logging ist standardmaessig aus (EnableLogging=0: keine Log-Datei, kein
    Mehraufwand).
  - Fuer Fehlerberichte bevorzugt EnableLogging=2 in tr_hud_fix.ini setzen
    (Waechter: kleines Log, das HUD-Fehler und Abstuerze automatisch
    festhaelt). EnableLogging=1 schreibt ein vollstaendiges Analyse-Log. Im
    Spielordner entsteht dann tr_hud_fix.log (Groessenlimit MaxLogMB, Standard
    50 MB; danach wieder auf 0 setzen). Das Log des vorherigen Starts bleibt als
    tr_hud_fix.prev.log erhalten - nach einem Absturz bitte diese Datei senden.
  - EnableLogging=2 zeichnet zusaetzlich einmal pro Minute die
    Speicherbelegung auf ([MEM]-Zeilen) - hilfreich bei Berichten zu
    "Out of memory"-Fehlern in langen Sitzungen (bekanntes Engine-Problem des
    Spiels seit 2014, nicht durch diesen Fix verursacht).
  - EnableLogging=2 schreibt ausserdem eine Liste der gesehenen HUD-Elemente
    ([INV]-Zeilen) - das hilft sehr bei Berichten zu verschobenen HUD-Elementen.
  - Microsoft-Store-/Game-Pass-Version: nicht getestet.

Credits / Lizenzen:
  TR2013 HUD Fix+ - MIT-Lizenz (licenses\TR_HUD_Fix_LICENSE.txt)
  Dear ImGui von Omar Cornut - MIT-Lizenz (licenses\DearImGui_LICENSE.txt)
  Das Menue nutzt, falls vorhanden, die Windows-Systemschrift "Bahnschrift"
  (nicht enthalten).

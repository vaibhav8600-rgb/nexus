goal here is to turn that concept into a **new reusable standalone NEXUS dongle project**, without breaking your existing Sofle setup. ([GitHub][1])
---

# NEXUS SMART ZMK DONGLE

## Comprehensive Software & Firmware Requirements Specification

**Document version:** 1.0
**Project type:** ZMK reusable dongle / firmware module
**Target MCU:** nRF52840
**Primary display:** ST7789 240×240 color TFT
**Firmware:** ZMK + Zephyr
**UI:** Modern glassmorphism + subtle neumorphism
**Primary game:** Tetris
**Architecture:** Reusable / configurable / ZMK Studio compatible

---

# 1. PROJECT OBJECTIVE

Build a new standalone, reusable **NEXUS Smart ZMK Dongle Platform**.

NEXUS is a dedicated nRF52840-based ZMK central/dongle with:

* ZMK split central functionality
* USB host connectivity
* Bluetooth host connectivity
* ZMK Studio support
* 240×240 ST7789 color display
* modern animated graphical UI
* glassmorphism/neumorphism visual design
* keyboard status dashboard
* left/right battery information
* WPM information
* active layer information
* modifier/lock indicators
* Bluetooth/USB connection status
* physical Action button
* physical hardware Reset button
* passive buzzer
* UI/game sound effects
* configurable splash screen
* user-configurable branding
* game center
* playable Tetris
* architecture allowing future games
* configuration from an integrating ZMK config repository

The project must be designed as a **platform**, not as a one-off Sofle implementation.

---

# 2. IMPORTANT EXISTING PROJECT CONTEXT

The existing repository is:

`vaibhav8600-rgb/zmk-sofle-main`

The existing implementation already demonstrates:

* dedicated central dongle
* ZMK split BLE topology
* ST7789 display
* custom splash
* buzzer
* status UI
* battery fetching
* WPM
* layer display
* connectivity display
* game functionality
* custom dongle action behavior
* GitHub Actions firmware builds

The current repository uses a nice!nano-based central dongle and currently integrates a Snake module. ([GitHub][1])

**Do not destroy or unnecessarily modify the existing working configuration.**

The new NEXUS implementation should be developed as a **separate reusable dongle platform/module/shield**, allowing an existing ZMK configuration to consume it.

---

# 3. CORE DESIGN PRINCIPLE

The most important architectural rule is:

> **Separate NEXUS from the user's keyboard configuration.**

The NEXUS firmware should contain:

```text
NEXUS
├── hardware abstraction
├── display engine
├── UI engine
├── status engine
├── game engine
├── sound engine
├── input engine
├── configuration system
├── splash system
└── ZMK integration
```

The user's ZMK configuration should contain:

```text
USER CONFIG
├── keyboard/keymap
├── keyboard-specific settings
├── NEXUS configuration
├── splash asset
├── branding
├── theme overrides
└── enabled games
```

Do not require users to modify NEXUS source code to customize normal features.

---

# 4. HARDWARE

## 4.1 MCU

Target:

```text
nRF52840
```

The implementation should avoid assuming a specific commercial board such as nice!nano.

The first hardware target is an **nRF52840 ProMicro-compatible controller**.

The architecture should allow another nRF52840 board to be supported later.

---

# 5. HARDWARE PIN ASSIGNMENTS

Use the supplied hardware wiring diagram as the source of truth for the intended pin mapping.

## Display

| Function         | MCU pin |
| ---------------- | ------- |
| ST7789 SCK       | P0.17   |
| ST7789 MOSI      | P0.20   |
| ST7789 RESET     | P0.22   |
| ST7789 DC        | P0.24   |
| ST7789 CS        | P0.10   |
| ST7789 Backlight | P0.11   |

## Buzzer

| Function       | MCU pin |
| -------------- | ------- |
| Passive buzzer | P0.02   |

## Action button

| Function      | MCU pin |
| ------------- | ------- |
| Action button | P0.31   |

## Reset

| Function       | Connection |
| -------------- | ---------- |
| Hardware reset | MCU RST    |

---

# 6. HARDWARE VALIDATION REQUIREMENT

The coding agent must **verify the actual board's GPIO numbering and Zephyr pin naming before implementing the overlay**.

Do not blindly assume:

```text
P0.02
P0.10
P0.11
...
```

map directly to the same symbolic identifiers on every ProMicro-compatible board.

Create a board/shield abstraction so physical pin mappings are isolated.

---

# 7. DISPLAY

The primary display is:

```text
ST7789
240 × 240
RGB565
SPI
```

The display must support:

* initialization
* reset
* orientation
* backlight control
* frame rendering
* partial redraw where practical
* sleep
* wake
* brightness control if hardware permits
* UI animations

---

# 8. DISPLAY ORIENTATION

Display orientation must be configurable.

Example:

```text
NEXUS_DISPLAY_ROTATION=90
```

Supported values should be determined by the display driver.

The UI must use a **logical coordinate system**.

Never scatter physical rotation assumptions throughout the UI code.

---

# 9. DISPLAY PERFORMANCE

The nRF52840 is resource constrained.

Therefore:

* avoid unnecessary full-screen redraws
* avoid allocating large buffers repeatedly
* avoid heap-heavy architecture
* avoid blocking UI operations
* use incremental updates where possible
* avoid expensive alpha compositing if it cannot run smoothly
* keep animations lightweight

Target:

```text
UI target: ~30 FPS
```

where practical.

Games may target:

```text
30–60 FPS
```

depending on actual performance.

The implementation must prioritize **responsive input and ZMK keyboard functionality over visual effects**.

---

# 10. BACKLIGHT

The backlight must be controlled through a dedicated abstraction:

```c
nexus_display_backlight_set(...)
```

Support:

```text
ON
OFF
BRIGHTNESS
AUTO
```

If PWM is electrically supported by the hardware, brightness should use PWM.

If only GPIO is supported:

```text
ON/OFF
```

is sufficient.

Do not claim analog brightness control if the hardware cannot support it.

---

# 11. PHYSICAL ACTION BUTTON

The Action button is connected to:

```text
P0.31
```

It is a normal user input.

It must support:

* short press
* long press
* optional double press
* debounce

Recommended debounce:

```text
20–50 ms
```

The exact value should be tuned during hardware testing.

---

# 12. ACTION BUTTON BEHAVIOR

The button must be context-sensitive.

### HOME

Short press:

```text
HOME → GAME CENTER
```

Long press:

```text
HOME → QUICK MENU / SETTINGS
```

### GAME CENTER

Short press:

```text
Select highlighted game
```

Long press:

```text
Return HOME
```

### GAME

Short press:

```text
Pause / context action
```

Long press:

```text
Exit game → GAME CENTER
```

### PAUSED

Short press:

```text
Resume
```

Long press:

```text
Exit
```

### GAME OVER

Short press:

```text
Restart
```

Long press:

```text
Return GAME CENTER
```

The exact mappings must be implemented through an action/state system, not hard-coded separately inside each screen.

---

# 13. ACTION API

Create a central action dispatcher:

```text
nexus_action_dispatch(action)
```

Possible actions:

```text
NEXUS_ACTION_SELECT
NEXUS_ACTION_BACK
NEXUS_ACTION_PAUSE
NEXUS_ACTION_RESUME
NEXUS_ACTION_HOME
NEXUS_ACTION_GAME_CENTER
NEXUS_ACTION_RESTART
NEXUS_ACTION_NEXT
NEXUS_ACTION_PREVIOUS
```

The physical Action button and ZMK keymap behaviors should be able to invoke the same logical actions.

---

# 14. RESET BUTTON

The Reset button is a **hardware reset**.

It must remain independent of the software UI.

Do not convert the reset button into a normal ZMK key.

It must work even if:

* LVGL/UI crashes
* game crashes
* Bluetooth is stuck
* USB state is abnormal
* application state becomes corrupted

---

# 15. PASSIVE BUZZER

The hardware contains a **passive buzzer**.

Signal:

```text
P0.02
```

The firmware must generate the appropriate waveform/PWM.

Do not treat it as an active buzzer.

---

# 16. SOUND ENGINE

Create:

```text
NEXUS SOUND ENGINE
```

with API similar to:

```c
nexus_sound_play(NEXUS_SOUND_STARTUP);
```

Sound identifiers:

```text
STARTUP
SELECT
BACK
CONNECT
DISCONNECT

MENU_OPEN
MENU_SELECT

GAME_START
GAME_PAUSE
GAME_RESUME
GAME_OVER

TETRIS_MOVE
TETRIS_ROTATE
TETRIS_DROP
TETRIS_LINE
TETRIS_LEVEL
TETRIS_GAME_OVER
```

---

# 17. SOUND CONFIGURATION

Allow:

```text
CONFIG_NEXUS_SOUND=y
CONFIG_NEXUS_UI_SOUNDS=y
CONFIG_NEXUS_GAME_SOUNDS=y
```

or an equivalent clean configuration mechanism.

At minimum provide:

```text
Sound ON
Sound OFF
```

The physical volume potentiometer remains the hardware volume control.

---

# 18. BOOT / STARTUP

Startup sequence:

```text
POWER ON
    ↓
MCU initialization
    ↓
Display initialization
    ↓
NEXUS splash
    ↓
Optional startup sound
    ↓
ZMK initialization
    ↓
BLE/split initialization
    ↓
Host connection
    ↓
HOME
```

The startup screen must not unnecessarily delay keyboard usability.

Splash duration must be configurable.

Default:

```text
3000–5000 ms
```

or another sensible value selected by the implementation.

Allow:

```text
0 ms
```

to disable the splash.

---

# 19. CUSTOM SPLASH SCREEN

This is a **mandatory requirement**.

The user must be able to change the splash screen from their own ZMK config repository.

They must **not** need to modify NEXUS source code.

Support:

* custom image
* custom title
* custom subtitle
* custom duration
* optional sound

Example:

```text
VAIBHAV TECH

NEXUS

SMART ZMK DONGLE
```

---

# 20. SPLASH ASSET SYSTEM

The architecture should support a build-time asset mechanism.

Example conceptual structure:

```text
config/
└── nexus/
    ├── nexus.conf
    ├── splash/
    │   └── splash.png
    └── theme.conf
```

The exact mechanism can be chosen based on ZMK/Zephyr build constraints.

The important requirement:

> User assets must be converted into firmware-compatible data during the build process.

Do not require users to manually generate giant C arrays.

---

# 21. SPLASH FALLBACK

If the user doesn't provide a custom splash:

```text
NEXUS DEFAULT SPLASH
```

must be used.

Priority:

```text
USER CONFIG
      ↓
NEXUS DEFAULT
```

---

# 22. USER BRANDING

Allow configuration of:

```text
brand name
product name
subtitle
```

Example:

```text
Brand:
VAIBHAV TECH

Product:
NEXUS

Subtitle:
SMART ZMK DONGLE
```

Do not hard-code "VAIBHAV TECH" into the core NEXUS platform.

It should only be the default/example configuration.

---

# 23. NEXUS HOME SCREEN

The home screen should resemble the provided UI reference.

Visual direction:

* dark translucent panels
* glassmorphism
* subtle neumorphism
* rounded cards
* soft highlights
* subtle gradients
* thin borders
* depth/shadow illusion
* modern futuristic appearance
* high readability

Avoid:

* excessive gradients
* unreadable text
* too many animations
* visual clutter
* excessive glow
* tiny typography

---

# 24. HOME SCREEN INFORMATION

The dashboard should display:

```text
┌──────────────────────────────┐
│ NEXUS              Bluetooth │
│                              │
│       LAYER                  │
│       DEFAULT                │
│                              │
│  MODIFIERS                   │
│                              │
│ ┌────────────┐ ┌────────────┐│
│ │ LEFT       │ │ RIGHT      ││
│ │ BATTERY    │ │ BATTERY    ││
│ │  51%       │ │ 53%       ││
│ └────────────┘ └────────────┘│
│                              │
│          WPM 72              │
└──────────────────────────────┘
```

---

# 25. STATUS DATA

The UI should expose:

### WPM

Display:

```text
WPM 72
```

Update smoothly without excessive redraw.

### Active layer

Example:

```text
LAYER
DEFAULT
```

### Left battery

Example:

```text
LEFT
51%
```

### Right battery

Example:

```text
RIGHT
53%
```

### Connection

Display:

```text
USB
BLE
DISCONNECTED
CONNECTING
```

as appropriate.

### Bluetooth profile

Display current active profile if available.

### Modifiers

Show:

```text
CTRL
SHIFT
ALT
GUI
```

with active/inactive states.

### Lock indicators

Support:

```text
CAPS
NUM
SCROLL
```

when data is available.

---

# 26. BATTERY

The central/dongle should retrieve peripheral battery data using the existing ZMK split mechanisms where supported.

Your existing implementation already demonstrates central battery fetching for both peripheral halves. ([GitHub][1])

The NEXUS implementation should retain this architecture.

Battery states:

```text
0–10%     critical
11–25%    low
26–50%    medium
51–75%    good
76–100%   full
```

Exact visual thresholds may be configurable.

If battery information is unavailable:

```text
--%
```

rather than:

```text
0%
```

This distinction is important.

---

# 27. WPM

The UI should consume ZMK WPM information rather than implementing an independent keyboard scanner.

Display:

```text
WPM
072
```

Use a smoothed display value.

Avoid visual jitter such as:

```text
72
69
75
70
73
```

changing every frame.

---

# 28. LAYER DISPLAY

Layer names should come from ZMK where possible.

Example:

```text
DEFAULT
LOWER
RAISE
GAMES
```

The UI must not assume specific layer names.

---

# 29. CONNECTION STATE MACHINE

Implement:

```text
DISCONNECTED
CONNECTING
CONNECTED
RECONNECTING
```

For split:

```text
LEFT CONNECTED
RIGHT CONNECTED
LEFT DISCONNECTED
RIGHT DISCONNECTED
```

The dashboard can show:

```text
● LEFT
● RIGHT
```

with clear state changes.

---

# 30. ZMK STUDIO

ZMK Studio support is mandatory.

Current ZMK Studio uses RPC over USB serial/CDC-ACM and BLE GATT. ([ZMK Firmware][2])

The project must use the **official ZMK Studio integration**.

Do not create a custom fake Studio protocol.

Do not replace ZMK Studio with a proprietary protocol.

---

# 31. ZMK STUDIO USB

The central/dongle firmware should support:

```text
studio-rpc-usb-uart
```

and:

```text
CONFIG_ZMK_STUDIO=y
```

as appropriate for the selected build architecture. ZMK's current documentation specifies the Studio RPC USB snippet and `CONFIG_ZMK_STUDIO=y` for Studio-enabled builds. ([ZMK Firmware][3])

---

# 32. ZMK STUDIO UNLOCK

Provide a way to unlock the device for Studio.

Use the standard:

```text
&studio_unlock
```

behavior rather than inventing another mechanism. ([ZMK Firmware][4])

The unlock mechanism should be accessible through the keyboard configuration.

---

# 33. ZMK STUDIO KEYMAP COMPATIBILITY

The NEXUS project must not make assumptions that prevent ZMK Studio from managing the user's keymap.

ZMK Studio currently supports runtime keymap changes, predefined/user-defined behaviors, layer renaming and other functions. ([ZMK Firmware][3])

The implementation should follow current ZMK Studio requirements rather than creating its own keymap management layer.

---

# 34. ZMK STUDIO MEMORY

The coding agent must monitor:

```text
Flash usage
RAM usage
stack usage
```

Studio requires additional memory.

Do not sacrifice stability just to add visual features.

If necessary:

```text
disable unnecessary logging
reduce framebuffer size
reduce animation buffers
reduce game memory
```

before removing core ZMK functionality.

---

# 35. ZMK STUDIO + NEXUS UI

ZMK Studio changes must not break:

* UI
* games
* buzzer
* Action button
* status monitoring
* split connectivity

If the keymap changes at runtime, NEXUS should continue functioning.

---

# 36. GAME CENTER

Replace the existing Snake-first concept with a general:

```text
GAME CENTER
```

architecture.

The UI should look like a small handheld arcade launcher.

Example:

```text
┌────────────────────────────┐
│        GAME CENTER         │
│                            │
│     ┌──────────────┐       │
│     │   TETRIS     │       │
│     │      ▣       │       │
│     └──────────────┘       │
│                            │
│   ◀                 ▶      │
│                            │
│       HIGH SCORE           │
│          12450             │
└────────────────────────────┘
```

---

# 37. GAME ARCHITECTURE

Do not implement Tetris directly inside the UI screen.

Create:

```text
game_engine
```

with:

```text
games/
├── game_manager
├── game_interface
├── tetris
├── snake [optional future]
├── maze [future]
└── ...
```

Each game should implement a common interface.

Conceptually:

```c
game_init()
game_start()
game_update()
game_input()
game_pause()
game_resume()
game_draw()
game_stop()
```

---

# 38. GAME MANAGER

The Game Manager should handle:

```text
game discovery
game selection
game launch
game pause
game resume
game exit
game over
high scores
```

The UI should not know game-specific internals.

---

# 39. TETRIS — VERSION 1

Tetris is the primary game.

It must be a **real playable game**, not an animation.

Requirements:

* falling blocks
* tetromino pieces
* movement left/right
* rotation
* soft drop
* hard drop
* collision detection
* line clearing
* scoring
* levels
* increasing speed
* pause
* resume
* game over
* restart
* high score

---

# 40. TETRIS BOARD

Standard Tetris board:

```text
10 columns
20 visible rows
```

Optionally include hidden spawn rows if required by implementation.

The board must be optimized for nRF52840 memory.

Do not store the board as an expensive graphical object structure.

Use a compact logical representation.

---

# 41. TETRIS PIECES

Support all seven standard tetrominoes:

```text
I
O
T
S
Z
J
L
```

Pieces should have deterministic collision and rotation behavior.

---

# 42. TETRIS INPUT

The game engine should support logical actions:

```text
MOVE_LEFT
MOVE_RIGHT
ROTATE
SOFT_DROP
HARD_DROP
PAUSE
```

Do not hard-code physical keyboard keys into the game.

This is important.

---

# 43. TETRIS INPUT SOURCES

Tetris may receive input from:

### Keyboard

ZMK key bindings.

### Action button

Context-specific actions.

### Future

Encoder / external controller.

The game engine should only receive:

```text
GAME_INPUT_MOVE_LEFT
GAME_INPUT_MOVE_RIGHT
...
```

---

# 44. TETRIS GAME LOOP

Use a timer/work queue appropriate for Zephyr.

Do not use:

```c
while (1) {
    k_sleep(...);
}
```

inside the main UI thread.

The game must not block:

* ZMK scanning
* BLE
* USB
* Studio RPC
* display management

---

# 45. TETRIS SPEED

Start at a comfortable speed.

Example:

```text
Level 1 → 800 ms
Level 2 → 700 ms
Level 3 → 600 ms
...
```

Exact timings can be tuned.

The speed must increase as the player clears lines.

---

# 46. TETRIS SCORING

Suggested:

```text
1 line  → 100 × level
2 lines → 300 × level
3 lines → 500 × level
4 lines → 800 × level
```

Hard drops may award additional points.

Scoring must be centralized in the game engine.

---

# 47. TETRIS HIGH SCORE

High score should persist using Zephyr/ZMK settings where practical.

At minimum:

```text
current score
high score
```

must work during runtime.

Persistent high score is preferred.

---

# 48. GAME SOUND

Tetris should use the sound engine.

Example:

```text
move       → short click
rotate     → short tone
hard drop  → stronger tone
line       → musical effect
4 lines    → special effect
game over  → descending tone
```

Do not directly access the buzzer from Tetris.

---

# 49. GAME PAUSE

Pause screen:

```text
┌──────────────────────┐
│                      │
│       PAUSED         │
│                      │
│    PRESS ACTION      │
│      TO RESUME       │
│                      │
└──────────────────────┘
```

No gameplay updates while paused.

---

# 50. GAME OVER

Game-over screen:

```text
┌──────────────────────┐
│     GAME OVER        │
│                      │
│      SCORE           │
│       8420           │
│                      │
│      BEST            │
│      12450           │
│                      │
│   ACTION = RESTART   │
└──────────────────────┘
```

---

# 51. FUTURE GAME SUPPORT

Architecture must allow:

```text
Tetris
Snake
Pac-Man-style maze game
Breakout
Space shooter
2048
Pong
```

without rewriting the UI framework.

Only Tetris is mandatory for v1.

---

# 52. PAC-MAN-STYLE GAME

Do **not** implement a direct copyrighted Pac-Man clone using proprietary assets.

Instead, the future architecture may support an original:

```text
maze chase game
```

with:

* maze
* player
* enemies
* collectibles
* score
* levels
* power-ups

Use original graphics and naming.

---

# 53. GAME CENTER NAVIGATION

Game selection should support:

```text
NEXT GAME
PREVIOUS GAME
SELECT
BACK
```

Even if only Tetris exists initially, the launcher should be designed for multiple games.

---

# 54. UI SCREEN ARCHITECTURE

Create screens:

```text
SplashScreen
HomeScreen
GameCenterScreen
GameScreen
PauseScreen
GameOverScreen
SettingsScreen
DiagnosticsScreen
AboutScreen
```

Use a central navigation manager.

Example:

```text
nexus_screen_push()
nexus_screen_pop()
nexus_screen_replace()
```

---

# 55. UI STATE MACHINE

Suggested:

```text
BOOT
 ↓
SPLASH
 ↓
HOME
 ├── GAME_CENTER
 │     └── GAME
 │          ├── PAUSE
 │          └── GAME_OVER
 │
 ├── QUICK_MENU
 │
 ├── SETTINGS
 │
 └── DIAGNOSTICS
```

---

# 56. UI INPUT

The UI must use logical events:

```text
UI_UP
UI_DOWN
UI_LEFT
UI_RIGHT
UI_SELECT
UI_BACK
UI_ACTION
```

Do not bind UI logic directly to GPIO.

---

# 57. GLASSMORPHISM DESIGN

The visual language should be inspired by the supplied reference.

Use:

* rounded rectangles
* translucent-looking cards
* layered backgrounds
* subtle borders
* soft highlights
* depth
* dark base
* bright accent colors
* large typography
* clean spacing

Because actual alpha blending may be expensive, the implementation may simulate glass visually using:

```text
dark translucent-style colors
gradient-like bands
border highlights
soft shadows
```

rather than requiring expensive real-time blur.

---

# 58. NEUMORPHISM

Use subtle neumorphic effects for:

* buttons
* toggles
* status indicators
* selected game
* action button UI

Do not make every component heavily embossed.

---

# 59. ANIMATIONS

Support lightweight animations:

```text
splash fade
screen transition
card slide
battery bar animation
connection pulse
WPM transition
game selection
button press
```

Animations must never block ZMK.

---

# 60. FPS / FRAME MANAGEMENT

Implement a central UI update scheduler.

Example:

```text
UI_REFRESH_FAST
UI_REFRESH_NORMAL
UI_REFRESH_IDLE
```

Fast:

```text
games
animations
```

Normal:

```text
status
```

Idle:

```text
static screen
```

This reduces power and CPU usage.

---

# 61. STATUS UPDATE OPTIMIZATION

Do not redraw the entire display when only WPM changes.

Example:

```text
battery changed
    ↓
update battery widget only
```

instead of:

```text
redraw entire screen
```

where practical.

---

# 62. SETTINGS SCREEN

Provide:

```text
SETTINGS
├── Sound
├── Brightness
├── Animation
├── Theme
├── Splash
├── Game Settings
└── About
```

Not every setting needs runtime editing in v1.

Build-time configuration is acceptable initially.

---

# 63. DIAGNOSTICS SCREEN

Provide a developer diagnostic screen.

Show:

```text
NEXUS
Firmware version
ZMK version
Board
Display
BLE
USB
Left connection
Right connection
Battery
Free RAM
Uptime
```

Optional:

```text
FPS
game state
sound state
```

This screen is extremely useful during hardware development.

---

# 64. DEBUG MODE

Support a build-time debug option:

```text
CONFIG_NEXUS_DEBUG
```

When enabled:

* logging
* diagnostics
* optional FPS
* input debugging
* display diagnostics

When disabled:

* minimal logging
* lower power
* production behavior

---

# 65. LOGGING

Do not leave verbose debug logging enabled in production.

ZMK documentation notes that USB logging can increase power consumption. ([ZMK Firmware][5])

Use:

```text
ERROR
WARN
INFO
DEBUG
```

appropriately.

---

# 66. POWER MANAGEMENT

The dongle should minimize unnecessary CPU/display activity.

Implement:

```text
display sleep
backlight timeout
animation pause
```

when appropriate.

However, do not aggressively sleep while:

* ZMK Studio is connected
* a game is active
* keyboard status requires updates

---

# 67. KEYBOARD FUNCTIONALITY PRIORITY

The priority order is:

```text
1. ZMK keyboard functionality
2. BLE split reliability
3. USB functionality
4. ZMK Studio
5. display status
6. game
7. animations
8. sound
```

A game must never compromise keyboard operation.

---

# 68. SPLIT CENTRAL ROLE

NEXUS is a dedicated split central.

Conceptually:

```text
                  USB/BLE HOST
                       │
                       ▼
                 NEXUS DONGLE
                       │
                ┌──────┴──────┐
                │             │
                ▼             ▼
             LEFT           RIGHT
           PERIPHERAL     PERIPHERAL
```

The dongle should handle:

* central role
* host connection
* split communication
* status aggregation
* display
* game
* sound

The keyboard halves remain primarily responsible for:

* matrix scanning
* local input
* encoders
* peripheral BLE

---

# 69. EXISTING SOFLE COMPATIBILITY

NEXUS must not require redesigning the existing Sofle keymap.

Integration should ideally look conceptually like:

```text
ZMK CONFIG
     │
     ├── Sofle keyboard
     │
     └── NEXUS dongle
```

The existing left/right firmware should remain functional.

---

# 70. REUSABLE SHIELD / MODULE

NEXUS should be implemented as a reusable ZMK component.

Possible architecture:

```text
modules/
└── nexus/
    ├── CMakeLists.txt
    ├── module.yml
    ├── Kconfig
    ├── Kconfig.nexus
    ├── dts/
    ├── include/
    ├── src/
    ├── boards/
    └── shields/
```

Exact structure can be adapted to ZMK conventions.

---

# 71. HARDWARE ABSTRACTION LAYER

Create:

```text
hal/
├── nexus_display
├── nexus_buzzer
├── nexus_button
├── nexus_backlight
└── nexus_board
```

The UI/game layer must never contain:

```text
P0.02
P0.31
P0.10
```

or other board-specific information.

---

# 72. DISPLAY DRIVER ABSTRACTION

Do not make the game engine depend on ST7789.

Game:

```text
game_draw()
```

should render through a generic display/UI API.

This allows a future:

```text
ST7789
ILI9341
OLED
other TFT
```

implementation.

---

# 73. SOUND ABSTRACTION

Similarly:

```text
Game
 ↓
Sound API
 ↓
Buzzer implementation
```

not:

```text
Game
 ↓
PWM P0.02
```

---

# 74. CONFIGURATION SYSTEM

All user-facing options should be configurable through Kconfig, Devicetree, or user assets.

Examples:

```text
CONFIG_NEXUS_ENABLE
CONFIG_NEXUS_DISPLAY
CONFIG_NEXUS_GAME_CENTER
CONFIG_NEXUS_TETRIS
CONFIG_NEXUS_SOUND
CONFIG_NEXUS_UI_SOUND
CONFIG_NEXUS_GAME_SOUND
CONFIG_NEXUS_SPLASH
CONFIG_NEXUS_SPLASH_DURATION
CONFIG_NEXUS_THEME
CONFIG_NEXUS_DEFAULT_SCREEN
CONFIG_NEXUS_DEBUG
```

Exact names may be changed if they conflict with existing ZMK/Zephyr symbols.

---

# 75. DEFAULT CONFIGURATION

Default firmware should provide:

```text
Display: ON
UI: ON
Game Center: ON
Tetris: ON
Sound: ON
Splash: ON
Default screen: HOME
Theme: NEXUS
Debug: OFF
```

---

# 76. USER CONFIGURATION EXAMPLE

Provide documentation showing something conceptually like:

```text
CONFIG_NEXUS=y
CONFIG_NEXUS_DISPLAY=y
CONFIG_NEXUS_GAME_CENTER=y
CONFIG_NEXUS_TETRIS=y
CONFIG_NEXUS_SOUND=y
CONFIG_NEXUS_SPLASH=y
CONFIG_NEXUS_SPLASH_DURATION_MS=4000
CONFIG_NEXUS_THEME="CYBER"
```

Do not promise exact Kconfig symbols until implemented.

---

# 77. THEME SYSTEM

Provide themes:

```text
NEXUS
CYBER
AMOLED
RETRO
MINIMAL
```

Theme abstraction:

```text
background
surface
surface_secondary
accent
text
text_secondary
success
warning
error
```

The exact colors should be centrally defined.

---

# 78. CUSTOM THEME

Architecture should allow users to override theme values.

Example:

```text
accent = ...
background = ...
panel = ...
```

This can initially be build-time only.

Runtime theme editor is optional.

---

# 79. CONFIGURABLE DEFAULT SCREEN

Support:

```text
HOME
GAME_CENTER
```

Potential future:

```text
CUSTOM
DIAGNOSTICS
```

---

# 80. BUILD SYSTEM

The project must produce separate firmware artifacts for:

```text
NEXUS DONGLE
LEFT HALF
RIGHT HALF
SETTINGS RESET
```

where applicable.

The NEXUS dongle build should include:

```text
ZMK Studio
display
sound
games
central functionality
```

The halves should not unnecessarily include NEXUS game/display code.

---

# 81. BUILD OPTIMIZATION

Do not compile:

```text
Tetris
UI
ST7789
sound engine
```

into the peripheral firmware unless explicitly required.

This saves flash/RAM.

---

# 82. GITHUB ACTIONS

Provide automated builds.

At minimum:

```text
push
pull_request
manual workflow dispatch
```

Build artifacts should clearly identify:

```text
nexus_dongle
left
right
settings_reset
```

---

# 83. LOCAL BUILD

Documentation must provide a local `west build` procedure.

The coding agent must verify that the documented command actually works.

Do not write theoretical build commands.

---

# 84. CI VALIDATION

CI should verify:

```text
build success
DT validation
Kconfig validation
formatting
warnings where practical
```

No broken firmware artifact should be released.

---

# 85. FIRMWARE VERSION

Display:

```text
NEXUS v1.0.0
```

The version should come from a central version definition.

Do not duplicate version strings throughout source files.

---

# 86. ABOUT SCREEN

Example:

```text
NEXUS

Smart ZMK Dongle

Firmware
v1.0.0

nRF52840
ST7789

Powered by ZMK
```

---

# 87. ERROR HANDLING

If display initialization fails:

```text
ZMK must continue working
```

If buzzer fails:

```text
ZMK must continue working
```

If game fails:

```text
ZMK must continue working
```

If UI fails:

```text
ZMK must continue working
```

The UI/game system must be treated as a **non-critical subsystem**.

---

# 88. WATCHDOG / DEADLOCK SAFETY

Avoid long blocking operations.

If a watchdog is used, ensure:

* UI cannot starve it
* game cannot starve it
* Studio cannot starve it
* BLE cannot starve it

---

# 89. THREADING

Use Zephyr-native:

```text
work queues
timers
events
message queues
```

where appropriate.

Avoid creating unnecessary threads.

Each additional thread consumes RAM.

---

# 90. EVENT ARCHITECTURE

Use a central event model.

Example:

```text
ZMK event
   ↓
NEXUS event adapter
   ↓
NEXUS event bus
   ├── UI
   ├── status
   ├── game
   ├── sound
   └── diagnostics
```

This is preferred over every subsystem directly calling every other subsystem.

---

# 91. EVENT TYPES

Examples:

```text
NEXUS_EVENT_LAYER_CHANGED
NEXUS_EVENT_WPM_CHANGED
NEXUS_EVENT_BATTERY_CHANGED
NEXUS_EVENT_BT_CHANGED
NEXUS_EVENT_USB_CHANGED
NEXUS_EVENT_MODIFIER_CHANGED
NEXUS_EVENT_ACTION_BUTTON
NEXUS_EVENT_GAME_STARTED
NEXUS_EVENT_GAME_OVER
```

---

# 92. UI DATA MODEL

Create a status model:

```text
NexusStatus
├── layer
├── wpm
├── left_battery
├── right_battery
├── usb_state
├── ble_state
├── bt_profile
├── caps_lock
├── num_lock
├── scroll_lock
└── modifiers
```

UI reads this model.

It should not directly query ZMK internals from every widget.

---

# 93. STATUS REFRESH

When the status changes:

```text
update model
 ↓
notify UI
 ↓
update affected widget
```

Avoid continuous polling where event-based updates are available.

---

# 94. ACTION BUTTON DEBOUNCE

Button processing must distinguish:

```text
press
release
short press
long press
```

Avoid interpreting a long press as dozens of short presses.

---

# 95. GAME INPUT PRIORITY

When a game is active:

```text
game input
```

must receive appropriate mapped events.

But:

```text
RESET
```

remains hardware independent.

ZMK keyboard safety must remain intact.

---

# 96. KEYMAP INTEGRATION

The NEXUS platform may provide optional behaviors such as:

```text
&nexus_action
&nexus_game
&nexus_home
&nexus_next
&nexus_prev
```

if useful.

These behaviors should be optional and documented.

---

# 97. STUDIO BEHAVIOR COMPATIBILITY

Do not create behaviors that cannot be represented correctly in ZMK Studio unless they are purely dongle UI controls.

ZMK Studio currently supports predefined and user-defined behaviors, but not arbitrary new behaviors outside the device's declared devicetree. ([ZMK Firmware][3])

---

# 98. PHYSICAL LAYOUT / STUDIO METADATA

If the NEXUS hardware definition requires a keyboard/shield metadata definition for Studio, configure the required physical layout and metadata according to current ZMK requirements.

Current ZMK documentation states that Studio-compatible keyboard definitions need an appropriate physical layout and metadata, including the `studio` hardware feature when applicable. ([ZMK Firmware][6])

---

# 99. DO NOT REINVENT ZMK

Do not implement custom replacements for:

```text
BLE
USB HID
split transport
keymap storage
Studio RPC
settings
ZMK behaviors
```

Use ZMK/Zephyr functionality wherever possible.

NEXUS should sit **on top of ZMK**, not fork and replace ZMK.

---

# 100. MEMORY BUDGET

The coding agent must continuously monitor:

```text
Flash
RAM
stack
heap
```

The target MCU is nRF52840.

Prioritize:

```text
ZMK + BLE + Studio
```

over:

```text
visual effects
```

If memory becomes constrained, reduce:

1. animation buffers
2. image size
3. font count
4. sound data
5. game assets
6. debug code

before compromising core keyboard functionality.

---

# 101. IMAGE ASSETS

Do not store unnecessarily large PNG/JPEG assets directly in firmware.

Convert images into an efficient embedded format.

For splash:

```text
240×240
RGB565 or compressed format
```

should be considered.

---

# 102. FONTS

Use a small number of carefully selected fonts.

At minimum:

```text
Large display font
Normal UI font
Small status font
```

Avoid embedding a complete Unicode font if unnecessary.

Only include required glyph ranges.

---

# 103. TYPOGRAPHY

Use typography similar to the supplied reference:

* bold numbers
* clean uppercase labels
* compact status text
* high contrast
* large WPM
* large battery percentages

Avoid fonts that are difficult to read on a 240×240 display.

---

# 104. HOME SCREEN ANIMATION

Suggested:

```text
Battery bars
```

can animate smoothly when values change.

WPM:

```text
72 → 73 → 74
```

can transition visually.

Connection indicator:

```text
●
```

can gently pulse while connecting.

Do not animate static elements unnecessarily.

---

# 105. GAME CENTER VISUAL

Make Tetris feel like the featured game.

Example:

```text
┌──────────────────────────┐
│ GAME CENTER       01/03  │
│                          │
│       ┌──────────┐       │
│       │ ▓▓       │       │
│       │   ▓      │       │
│       │  ▓▓      │       │
│       └──────────┘       │
│                          │
│        TETRIS            │
│                          │
│     HIGH  12,450         │
└──────────────────────────┘
```

---

# 106. TETRIS GRAPHICS

Use original minimalist graphics.

No external copyrighted assets.

Pieces should be visually distinguishable through:

* shape
* brightness
* accent
* optional pattern

Color should not be the only differentiator.

---

# 107. GAME SCORE PERSISTENCE

Use ZMK/Zephyr settings where practical.

Suggested keys:

```text
nexus/tetris/highscore
```

Do not write settings on every frame.

Only write when:

```text
new high score
```

or another controlled event occurs.

---

# 108. SOUND DATA

If sound samples are used, carefully consider flash usage.

Prefer synthesized tones using PWM for simple arcade sounds.

This is especially suitable for a passive buzzer.

---

# 109. TESTING

The project must include testing at multiple levels.

### Unit tests

Test:

```text
Tetris collision
rotation
line clearing
score
level
game over
```

### Hardware tests

Test:

```text
display
backlight
button
reset
buzzer
BLE
USB
Studio
split
battery
```

---

# 110. TETRIS TEST CASES

Minimum:

```text
spawn piece
move left
move right
rotate
wall collision
floor collision
piece collision
line clear
multiple line clear
Tetris
level increase
hard drop
pause
resume
game over
restart
high score
```

---

# 111. HARDWARE TEST PROCEDURE

Create a documented sequence:

```text
1. Power on
2. Splash
3. Display test
4. Buzzer test
5. Action button test
6. BLE central test
7. Left split test
8. Right split test
9. USB test
10. ZMK Studio test
11. Game test
12. Reset test
```

---

# 112. RESET TEST

During testing:

1. Start game.
2. Trigger animations.
3. Connect Studio.
4. Press hardware reset.
5. Confirm MCU restarts.
6. Confirm firmware boots.
7. Confirm ZMK remains functional.
8. Confirm split reconnects.

---

# 113. FAILURE MODE

If display is disconnected:

```text
keyboard must still work
```

If buzzer is disconnected:

```text
keyboard must still work
```

If one half disconnects:

```text
other half + host should remain operational where ZMK permits
```

---

# 114. DOCUMENTATION

Create:

```text
README.md
docs/
├── hardware.md
├── installation.md
├── configuration.md
├── splash.md
├── themes.md
├── games.md
├── zmk-studio.md
├── development.md
├── troubleshooting.md
└── architecture.md
```

---

# 115. HARDWARE DOCUMENTATION

Document exact:

```text
MCU
display
pins
buzzer
buttons
power
USB
```

Include the wiring diagram supplied for the project.

---

# 116. USER INSTALLATION

The README should explain:

```text
What is NEXUS?
What hardware is required?
How to add NEXUS to ZMK?
How to configure it?
How to build?
How to flash?
How to use Studio?
How to customize splash?
How to enable Tetris?
```

---

# 117. USER CONFIG EXAMPLE

Provide a minimal example repository configuration.

Something like:

```text
my-zmk-config/
├── config/
│   ├── west.yml
│   ├── keyboard.keymap
│   ├── keyboard.conf
│   └── nexus/
│       ├── nexus.conf
│       └── splash/
│           └── splash.png
└── build.yaml
```

Exact structure should follow ZMK conventions.

---

# 118. INTEGRATION GOAL

The final integration should feel like:

```text
Existing ZMK Config
       │
       ├── Keyboard configuration
       │
       └── NEXUS Dongle
               │
               ├── Display
               ├── UI
               ├── Games
               ├── Sound
               └── Studio
```

not:

```text
copy 5000 lines of NEXUS code
into keyboard keymap
```

---

# 119. SOURCE CODE ORGANIZATION

Recommended:

```text
nexus/
├── CMakeLists.txt
├── Kconfig
├── Kconfig.nexus
├── module.yml
│
├── boards/
│
├── shields/
│   └── nexus_dongle/
│       ├── Kconfig.shield
│       ├── Kconfig.defconfig
│       ├── nexus_dongle.overlay
│       ├── nexus_dongle.conf
│       └── nexus_dongle.zmk.yml
│
├── include/
│   └── nexus/
│       ├── nexus.h
│       ├── display.h
│       ├── status.h
│       ├── sound.h
│       ├── game.h
│       ├── action.h
│       └── config.h
│
├── src/
│   ├── nexus.c
│   ├── display/
│   ├── ui/
│   ├── status/
│   ├── sound/
│   ├── input/
│   ├── games/
│   │   ├── game_manager.c
│   │   └── tetris.c
│   └── hal/
│
├── assets/
│
├── tests/
│
└── docs/
```

This is a recommended architecture, not a requirement to follow every filename exactly.

---

# 120. SEPARATION OF RESPONSIBILITIES

### ZMK

Responsible for:

```text
keyboard
BLE
USB
split
Studio
keymap
behaviors
```

### NEXUS

Responsible for:

```text
UI
display
status aggregation
games
sound
physical action button
branding
splash
```

### Hardware HAL

Responsible for:

```text
GPIO
SPI
PWM
display
buttons
buzzer
```

---

# 121. NO GLOBAL SPAGHETTI STATE

Avoid global variables that are modified from every subsystem.

Use structured state:

```c
struct nexus_context
```

containing references/state for:

```text
UI
display
game
sound
status
input
```

---

# 122. THREAD-SAFE DESIGN

If multiple Zephyr contexts can access shared state:

* use appropriate synchronization
* avoid race conditions
* avoid blocking critical ZMK paths

---

# 123. ZMK EVENT INTEGRATION

Use ZMK's existing event system where appropriate.

The coding agent must inspect current ZMK APIs rather than inventing event structures.

---

# 124. NO ASSUMPTIONS ABOUT CURRENT ZMK APIs

Because ZMK evolves, the coding agent must verify:

```text
display APIs
Studio APIs
split APIs
battery APIs
WPM APIs
settings APIs
physical layout APIs
```

against the exact ZMK revision being used.

Do not blindly copy old examples.

---

# 125. ZMK VERSION

The project must pin or clearly define the intended ZMK revision.

Avoid silently depending on:

```text
latest main
```

unless the project explicitly intends to track it.

---

# 126. COMPATIBILITY

The coding agent must document:

```text
ZMK version
Zephyr version
nRF52840 board
ST7789 driver
Studio support
```

---

# 127. EXISTING SNAKE CODE

The existing Snake implementation may be used as a reference for:

* game architecture
* buzzer integration
* display integration
* status UI
* dongle action behavior

but it must **not dictate the final NEXUS architecture**.

The new architecture should support multiple games.

---

# 128. MIGRATION STRATEGY

Do not attempt to rewrite everything at once.

Implement in stages:

```text
Phase 1
Hardware bring-up

Phase 2
ZMK central

Phase 3
Display

Phase 4
Status dashboard

Phase 5
Action button

Phase 6
Sound

Phase 7
Splash/configuration

Phase 8
ZMK Studio

Phase 9
Game framework

Phase 10
Tetris

Phase 11
Polish/animations
```

---

# 129. PHASE 1 — HARDWARE

First prove:

```text
nRF52840 boots
USB works
BLE works
display works
Action button works
reset works
buzzer works
```

Do not start with the fancy UI.

---

# 130. PHASE 2 — ZMK CENTRAL

Prove:

```text
dongle central
+
left half
+
right half
```

work reliably.

Only after this is stable should UI/game development continue.

---

# 131. PHASE 3 — DISPLAY

Display:

```text
NEXUS
Hello
```

Then test:

```text
colors
text
rotation
backlight
FPS
```

---

# 132. PHASE 4 — DASHBOARD

Implement:

```text
layer
WPM
left battery
right battery
connection
modifiers
```

before implementing complex animations.

---

# 133. PHASE 5 — ACTION BUTTON

Implement:

```text
short press
long press
debounce
context dispatch
```

---

# 134. PHASE 6 — SOUND

Implement:

```text
startup
button
connection
```

before game sound.

---

# 135. PHASE 7 — SPLASH

Implement user-configurable:

```text
image
title
subtitle
duration
```

and verify the asset mechanism from a **separate user config repository**.

This test is mandatory.

---

# 136. PHASE 8 — ZMK STUDIO

Verify:

```text
USB Studio connection
unlock
read keymap
change keymap
save keymap
lock
reconnect
```

Do not consider Studio complete merely because the firmware compiles.

---

# 137. PHASE 9 — GAME ENGINE

Implement generic:

```text
game manager
game interface
input interface
render interface
score interface
```

---

# 138. PHASE 10 — TETRIS

Implement the complete playable game.

Test it independently where possible.

---

# 139. PHASE 11 — UI POLISH

Only after all functional requirements work:

```text
glass
neumorphism
animations
transitions
sounds
visual effects
```

---

# 140. DEFINITION OF DONE

The project is complete only when:

### Hardware

* [ ] nRF52840 boots
* [ ] ST7789 works
* [ ] Action button works
* [ ] Reset button works
* [ ] buzzer works
* [ ] backlight works

### ZMK

* [ ] central works
* [ ] left half connects
* [ ] right half connects
* [ ] USB works
* [ ] BLE host works
* [ ] keyboard remains stable

### Studio

* [ ] Studio build works
* [ ] USB RPC works
* [ ] unlock works
* [ ] keymap can be changed
* [ ] runtime changes work

### UI

* [ ] splash
* [ ] home
* [ ] WPM
* [ ] layer
* [ ] battery
* [ ] connection
* [ ] modifiers
* [ ] animations
* [ ] themes

### Sound

* [ ] startup
* [ ] UI
* [ ] game
* [ ] disable option

### Games

* [ ] Game Center
* [ ] Tetris
* [ ] controls
* [ ] score
* [ ] high score
* [ ] pause
* [ ] game over
* [ ] restart

### Configuration

* [ ] custom splash from user config
* [ ] custom branding
* [ ] theme configuration
* [ ] game enable/disable
* [ ] sound enable/disable
* [ ] default screen configuration

### Engineering

* [ ] no blocking game loop
* [ ] no unnecessary RAM usage
* [ ] no unnecessary peripheral code
* [ ] clean HAL
* [ ] documentation
* [ ] CI
* [ ] reproducible build

---

# 141. CRITICAL NON-FUNCTIONAL REQUIREMENTS

The following are **hard requirements**:

### Requirement A

**Never compromise ZMK keyboard operation for the UI/game.**

### Requirement B

**Never hard-code user branding into the NEXUS core.**

### Requirement C

**Never require editing NEXUS source to change splash artwork.**

### Requirement D

**Never directly access hardware pins from game/UI code.**

### Requirement E

**Never implement a custom replacement for ZMK Studio RPC.**

### Requirement F

**Never block the main ZMK/BLE/USB processing path with game rendering.**

### Requirement G

**Hardware reset must always remain independent from software UI.**

### Requirement H

**NEXUS must be reusable with more than one keyboard configuration.**

---

# 142. IMPORTANT: EXISTING HARDWARE VS NEW HARDWARE

The existing `zmk-sofle-main` repository currently documents the dongle as a nice!nano v2 central with ST7789V and a custom Snake/status UI. ([GitHub][1])

The **new NEXUS hardware target**, however, is the nRF52840 ProMicro-style hardware shown in the supplied wiring diagram.

Therefore:

> Do not silently replace the existing nice!nano implementation in the existing repository.

Instead, create a new board/shield/module implementation for the new hardware.

If the actual ProMicro-compatible board uses a different Zephyr board target, determine the correct target during hardware bring-up.

---

# 143. CODING AGENT WORKING RULES

The coding agent must:

1. Inspect the existing repository before modifying anything.
2. Understand existing ZMK architecture.
3. Preserve working keyboard functionality.
4. Identify reusable code.
5. Identify code that should be rewritten.
6. Verify current ZMK APIs.
7. Verify actual board pin mappings.
8. Build after each major subsystem.
9. Keep commits logically separated.
10. Document architectural decisions.
11. Never invent unavailable ZMK APIs.
12. Never assume a display driver is compatible without testing.
13. Never add large assets without checking flash usage.
14. Never add an RTOS thread without checking RAM impact.
15. Never make UI code responsible for hardware control.

---

# 144. EXPECTED DEVELOPMENT OUTPUT

The coding agent should ultimately produce:

```text
NEXUS repository
│
├── reusable NEXUS module
├── NEXUS dongle shield
├── hardware configuration
├── ST7789 support
├── buzzer support
├── Action button
├── ZMK central
├── ZMK Studio
├── modern UI
├── status dashboard
├── splash system
├── configuration system
├── Game Center
├── Tetris
├── sound engine
├── tests
├── CI
└── documentation
```

---

# 145. FINAL PRODUCT EXPERIENCE

When a user powers on the completed dongle, the intended experience should be:

```text
             POWER ON
                 │
                 ▼
       ┌──────────────────┐
       │                  │
       │    USER LOGO     │
       │                  │
       │      NEXUS       │
       │                  │
       │  SMART DONGLE    │
       │                  │
       └──────────────────┘
                 │
                 ▼
       ┌──────────────────┐
       │ NEXUS       ●BLE │
       │                  │
       │      LAYER       │
       │      DEFAULT     │
       │                  │
       │  WPM             │
       │  072             │
       │                  │
       │ ┌──────┐ ┌──────┐│
       │ │ LEFT │ │RIGHT ││
       │ │  51% │ │  53% ││
       │ └──────┘ └──────┘│
       └──────────────────┘
                 │
           ACTION BUTTON
                 │
                 ▼
       ┌──────────────────┐
       │   GAME CENTER    │
       │                  │
       │      TETRIS      │
       │                  │
       │    HIGH SCORE    │
       │      12450       │
       └──────────────────┘
                 │
           ACTION BUTTON
                 │
                 ▼
       ┌──────────────────┐
       │                  │
       │      TETRIS      │
       │                  │
       │     ▓▓           │
       │      ▓           │
       │     ▓▓           │
       │                  │
       │ SCORE  820       │
       └──────────────────┘
```

The result should feel like a **small premium hardware product**, while underneath it remains a reliable ZMK split-keyboard central.

---

## One important implementation note

Your current repo already has several pieces of this vision — including ST7789 status UI, custom splash, buzzer events, central battery fetching, and a dongle action behavior — so the coding agent should **reuse proven pieces where appropriate rather than starting every subsystem from zero**. ([GitHub][1])

For ZMK Studio specifically, the agent should follow the current official Studio architecture rather than the older assumptions in some existing configs: current ZMK uses the `studio-rpc-usb-uart` snippet and `CONFIG_ZMK_STUDIO=y`, with Studio RPC using USB serial and BLE transports. ([ZMK Firmware][2])

**Official ZMK Studio reference:** [ZMK Studio documentation](https://zmk.dev/docs/features/studio?utm_source=chatgpt.com)

**Official Studio RPC reference:** [ZMK Studio RPC protocol](https://zmk.dev/docs/development/studio-rpc-protocol?utm_source=chatgpt.com)

This should give your coding agent enough direction to treat the project as a **proper reusable NEXUS platform**, rather than simply modifying the existing Snake dongle.

[1]: https://github.com/vaibhav8600-rgb/zmk-sofle-main "GitHub - vaibhav8600-rgb/zmk-sofle-main · GitHub"
[2]: https://zmk.dev/docs/development/studio-rpc-protocol?utm_source=chatgpt.com "ZMK Studio RPC Protocol | ZMK Firmware"
[3]: https://zmk.dev/docs/features/studio?utm_source=chatgpt.com "ZMK Studio | ZMK Firmware"
[4]: https://zmk.dev/docs/keymaps/behaviors/studio-unlock?utm_source=chatgpt.com "ZMK Studio Unlock Behavior | ZMK Firmware"
[5]: https://zmk.dev/docs/development/usb-logging?utm_source=chatgpt.com "USB Logging | ZMK Firmware"
[6]: https://zmk.dev/docs/hardware-integration/hardware-metadata-files?utm_source=chatgpt.com "Hardware Metadata Files | ZMK Firmware"

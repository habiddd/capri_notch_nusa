---
name: Capri Notch
description: A near-black glass notch at the top of the screen that comes alive with light, motion and a pair of eyes.
colors:
  capri-periwinkle: "#7C9CFF"
  glass-top: "#121317"
  glass-core: "#08090C"
  glass-base: "#010203"
  settings-ground: "#0E0F12"
  sidebar-top: "#16171D"
  sidebar-base: "#101115"
  sidebar-hover: "#1C1E24"
  surface: "#16171B"
  tab-well: "#222530"
  art-tile: "#2C303A"
  chip: "#2C2E34"
  chip-hover: "#45474C"
  divider: "#303137"
  text: "#F2F2F5"
  text-dim: "#8E9198"
  off-grey: "#5A5C62"
  disc-white: "#F5F5F7"
  ink: "#08090C"
  signal-green: "#4ADE80"
  signal-amber: "#FBBF24"
  signal-red: "#EF4444"
  signal-red-text: "#F87171"
  pomodoro-ember: "#962D2D"
  glow-classic-blue: "#2856C8"
  glow-classic-gold: "#BE9628"
  cat-general: "#60A5FA"
  cat-trabajo: "#A78BFA"
  cat-personal: "#4ADE80"
  cat-salud: "#F87171"
  cat-estudio: "#FBBF24"
  cat-social: "#F472B6"
typography:
  display:
    fontFamily: "Segoe UI"
    fontSize: "30pt"
    fontWeight: 700
  headline:
    fontFamily: "Segoe UI"
    fontSize: "22pt"
    fontWeight: 700
  lyric:
    fontFamily: "Segoe UI"
    fontSize: "15pt"
    fontWeight: 700
  settings-title:
    fontFamily: "Segoe UI"
    fontSize: "15pt"
    fontWeight: 700
  title:
    fontFamily: "Segoe UI"
    fontSize: "12.5pt"
    fontWeight: 700
  title-small:
    fontFamily: "Segoe UI"
    fontSize: "11pt"
    fontWeight: 700
  body:
    fontFamily: "Segoe UI"
    fontSize: "10pt"
    fontWeight: 400
  label:
    fontFamily: "Segoe UI"
    fontSize: "9pt"
    fontWeight: 400
  label-strong:
    fontFamily: "Segoe UI"
    fontSize: "9pt"
    fontWeight: 700
  caption:
    fontFamily: "Segoe UI"
    fontSize: "7.5pt"
    fontWeight: 400
  micro:
    fontFamily: "Segoe UI"
    fontSize: "7pt"
    fontWeight: 400
  icon:
    fontFamily: "Segoe Fluent Icons, Segoe MDL2 Assets"
    fontSize: "11pt"
    fontWeight: 400
rounded:
  hairline: "1.5px"
  xs: "5px"
  tile: "6px"
  sm: "8px"
  row: "9px"
  md: "10px"
  toggle: "11px"
  lg: "12px"
  capsule: "14px"
  card: "16px"
  notch-collapsed: "16px"
  notch-expanded: "22px"
spacing:
  hair: "3px"
  chip-gap: "6px"
  inset: "10px"
  shoulder: "12px"
  gutter: "20px"
  tab-step: "38px"
  settings-row: "56px"
components:
  notch-collapsed:
    backgroundColor: "{colors.glass-core}"
    textColor: "{colors.text}"
    typography: "{typography.label}"
    rounded: "{rounded.notch-collapsed}"
    height: "34px"
    width: "220px"
  notch-expanded:
    backgroundColor: "{colors.glass-core}"
    textColor: "{colors.text}"
    rounded: "{rounded.notch-expanded}"
    width: "460px"
  tab-active:
    backgroundColor: "{colors.tab-well}"
    textColor: "{colors.capri-periwinkle}"
    rounded: "{rounded.md}"
    width: "34px"
    height: "30px"
  tab-idle:
    textColor: "{colors.text-dim}"
    width: "34px"
    height: "30px"
  play-disc:
    backgroundColor: "{colors.disc-white}"
    textColor: "{colors.ink}"
  chip:
    backgroundColor: "{colors.chip}"
    textColor: "{colors.text}"
    typography: "{typography.label}"
    rounded: "{rounded.sm}"
    height: "26px"
    padding: "0 10px"
  chip-hover:
    backgroundColor: "{colors.chip-hover}"
  chip-selected:
    backgroundColor: "{colors.tab-well}"
    textColor: "{colors.capri-periwinkle}"
  toggle-off:
    backgroundColor: "{colors.chip}"
    rounded: "{rounded.toggle}"
    width: "40px"
    height: "22px"
  toggle-on:
    backgroundColor: "{colors.capri-periwinkle}"
    rounded: "{rounded.toggle}"
    width: "40px"
    height: "22px"
  stepper:
    backgroundColor: "{colors.chip}"
    textColor: "{colors.text}"
    typography: "{typography.label}"
    rounded: "{rounded.sm}"
    width: "124px"
    height: "26px"
  card:
    backgroundColor: "{colors.surface}"
    textColor: "{colors.text}"
    rounded: "{rounded.lg}"
    padding: "14px"
  settings-nav-active:
    backgroundColor: "{colors.chip}"
    textColor: "{colors.text}"
    typography: "{typography.body}"
    rounded: "{rounded.row}"
    width: "176px"
    height: "36px"
  settings-nav-hover:
    backgroundColor: "{colors.sidebar-hover}"
---

# Design System: Capri Notch

## Overview

**Creative North Star: "The Living Bezel"**

Capri Notch is not a window that sits on the screen; it is the top edge of the screen that has come alive. The body is near-black glass that runs flush into the bezel: no top hairline, concave shoulders (12 px) where the screen edge curves down into it, and corners that soften as it opens (16 px collapsed, 22 px expanded). At rest it is a 34 px sliver holding one line of truth, either the next event, the running timer or the song, beside a clock and a pair of white eyes that blink. Open, it grows on a damped spring into a panel where light pools at the base, tinted by the album cover or the chosen glow colors.

The system is dark, dense and quiet. Depth comes from tone and light, never from drop shadows: surfaces step up from glass (#08090C) to surface (#16171B) to chip (#2C2E34), and the only saturated color in steady use is Capri Periwinkle, which marks what is active. Everything alive (glow, gradients, eased motion, the equalizer, the eyes) has a switch, and the plain path still looks finished.

The settings window is the same material laid flat: a 780×540 dark ground with a gradient sidebar, sections that rise 12 px into place, and toggles that slide between grey and periwinkle.

**Key Characteristics:**
- Near-black glass that melts into the top screen edge; the top line is never stroked.
- Tonal layering instead of shadows; light is the only "elevation".
- One accent (Capri Periwinkle) for active state; status colors only for status.
- Segoe UI and Segoe Fluent Icons, small and bold: system-native, not branded type.
- Motion is physical (spring) for size and eased cubic-out for everything else, all of it optional.
- The mascot's eyes are the signature of the idle state.

## Colors

A near-black, slightly blue-cool neutral ramp with one periwinkle voice, a small status trio, and the cover art or wallpaper supplying the only other color.

### Primary
- **Capri Periwinkle** (`capri-periwinkle`): the active tab's icon and outline (outline at ~24 % alpha), the settings sidebar's accent bar and active icon, toggles that are on, focus edges on fields, progress bars and the CPU sparkline. If it is on screen, it means "this one" or "on".

### Neutral
- **Glass** (`glass-top` → `glass-core` → `glass-base`): the notch body, a diagonal three-stop gradient from the top-left; flat `glass-core` when gradients are off. `ink` is the same value, used for text on white.
- **Settings Ground** (`settings-ground`): the settings window body and its Windows 11 caption color.
- **Sidebar** (`sidebar-top` → `sidebar-base`, hover `sidebar-hover`): the settings sidebar's vertical gradient; flat `#141519` without gradients.
- **Surface** (`surface`): cards and fields inside the panel and settings.
- **Tab Well** (`tab-well`): the active tab's rounded square and the Sistema tile in the pill.
- **Chip** (`chip`, hover `chip-hover`): chips, steppers, toggle tracks when off, the active settings row.
- **Art Tile** (`art-tile`): the music tile with no cover; with gradients on it becomes a `#7888B4` → `#1E2438` diagonal.
- **Divider** (`divider`): 1 px rules between settings rows. Card edges use white at ~10 % alpha instead.
- **Text** (`text`) and **Dim Text** (`text-dim`): primary and secondary text. `off-grey` is for a disconnected sync dot.
- **Disc White** (`disc-white`): the play disc, the selected chip and today's mark. It is the loudest neutral; only one thing per view gets it.

### Status
- **Signal Green** (`signal-green`): connected, done, charging.
- **Signal Amber** (`signal-amber`): syncing, in progress.
- **Signal Red** (`signal-red`): errors and the pomodoro progress line; `pomodoro-ember` is the pomodoro tile in the pill.

### Ambient light
- **Classic glow** (`glow-classic-blue` left, `glow-classic-gold` right): two radial pools at the base of the expanded panel when nothing is playing (alpha ~72 and ~80). With a cover playing, the cover's two dominant colors replace them; the other sources are the wallpaper, the Windows accent or two custom colors.

### Categories
Six calendar categories, as dots, event blocks and filters: General (`cat-general`), Trabajo (`cat-trabajo`), Personal (`cat-personal`), Salud (`cat-salud`), Estudio (`cat-estudio`), Social (`cat-social`).

### Named Rules
**The One Voice Rule.** Capri Periwinkle is the only accent. Category and status colors label data; they never decorate chrome.

**The Light Not Paint Rule.** Color that is not data or state enters only as light (the base glow, the cover tint on the equalizer), never as filled panels.

## Typography

**Body Font:** Segoe UI
**Icon Font:** Segoe Fluent Icons (falls back to Segoe MDL2 Assets on Windows 10)

**Character:** Plain system type set small and often bold, so the notch reads as part of Windows rather than a branded overlay. All sizes are in points on a 96-DPI layout; the renderer scales to the monitor.

### Hierarchy
- **Display** (700, 30pt): one huge figure per view, such as the weather temperature.
- **Headline** (700, 22pt): the pomodoro countdown and other big numbers.
- **Lyric** (700, 15pt): the current lyric line; the previous line is 9.5pt regular and the next 10pt regular, both dimmed.
- **Settings Title** (700, 15pt): the section name at the top of the settings content.
- **Title** (700, 12.5pt): the track title, card headings.
- **Title Small** (700, 11pt): sub-headings inside a tab.
- **Body** (400, 10pt): settings row names and sidebar labels.
- **Label** (400, 9pt; 700 for the clock and right-hand values): everything in the collapsed pill, chips, list rows. Done tasks use strikeout.
- **Caption** (400, 7.5pt): settings row descriptions, time stamps.
- **Micro** (400 or 700, 7pt): the smallest marks, such as calendar hour labels and the G/O source badges.

### Named Rules
**The One Line Rule.** Text in the notch never wraps: it truncates with an ellipsis, and the collapsed pill widens (up to 560 px) before it truncates. It never grows taller.

## Layout

All layout is in fixed 96-DPI units, scaled to the monitor's real pixel density at paint time.

- **Collapsed pill:** 220×34 content (130 minimum for views that hug their content, 560 maximum), plus the 12 px shoulders on each side. Three slots: a 20×20 tile at the left (inset 10 px, 7 px from the top), centered text, and a right-aligned value in a 50 px slot.
- **Expanded panel:** 460 px wide by 250 px tall; 740 px with lyrics (controls 300, lyrics the rest); 760×400 for the calendar, and 524 tall while the event window is open. The tab row sits 8 px from the top, its tabs 34 px wide on a 38 px step, starting 16 px in, with the gear at the far right. Content starts at a 20 px gutter.
- **Settings window:** 780×540. A 200 px sidebar with 176×36 rows; content runs from x 232 to 756, between y 84 and 524, in 56 px rows (name at +9, description at +29, controls centered vertically and right-aligned to 756).
- **Rhythm:** a 3 px hairline gap, 6 px between chips, 10 px insets, 20 px gutters. Density is high on purpose: this is a glance surface.

## Elevation & Depth

Flat tonal layering with light. There are no drop shadows anywhere. Depth comes from three things: surfaces that step lighter as they come forward (glass, surface, tab well, chip), a 1 px white hairline at low alpha on edges (a diagonal gradient from ~26 % to ~4 % to ~13 % on the notch, flat ~12 % without gradients, ~10 % on cards), and radial pools of colored light at the base of the open panel.

### Named Rules
**The Melted Edge Rule.** The notch's top edge is never stroked or lit; the clip starts 1 px down, so the body reads as continuous with the screen bezel.

**The Plain Path Rule.** With glow and gradients off (Rendimiento, or battery saver), every gradient falls back to its middle solid and the glow disappears. Design the solid version first; light is a bonus on top of it.

## Shapes

Soft, continuous corners everywhere, sized to the element: 1.5 px for thin bars, 6 px for the pill's tile, 8 px for chips, steppers and shelf tiles, 9 px for settings rows, 10 px for tabs and small cards, 11 px for toggle tracks (a full capsule at 22 px tall), 12 px for the album art and large cards, 14 px for 28 px tall capsule buttons, 16 px for the event window card.

The notch's own outline is a custom path: concave Bézier shoulders at the top (control at 0.38 of the shoulder) and squircle-like bottom corners (Bézier handle at 0.448 × r), so it flows rather than snapping to circular arcs.

## Components

### Notch (signature)
- **Collapsed:** a glass sliver showing, in priority order: the charger notice (3 s), the running pomodoro (ember tile, a 2 px red progress line along the bottom), Sistema figures, music (cover tile, lyric or title, a four-bar equalizer in a tint lifted halfway to white), or idle.
- **Idle:** Nusa's eyes, two white 5×9 ellipses that blink every 4.5 s (2 px for 140 ms) and close to a line with a dim "z" after the sleep timeout; then today's next event, the open task count or the date; then the clock in label-strong.
- **Open/close:** width and height follow a damped spring (stiffness 320, damping 30, sub-stepped at 1/120 s), with the overshoot clamped to the buffer.

### Tab Row
- One 34×30 tab-well square with a 10 px radius and a periwinkle hairline glides between tabs over 220 ms (cubic ease-out); the active icon is periwinkle, the others dim. The content slides 28 px in from the direction of the switch and fades in over the same 220 ms.

### Buttons
- **Primary (play disc):** a disc-white circle with an ink glyph; the skip buttons beside it are bare white glyphs.
- **Capsule:** a 28 px tall pill (14 px radius) on chip or surface, used for quick actions (IA prompts, weather retry).
- **Hover:** the fill lifts to chip-hover, or the glyph turns periwinkle (stepper arrows).
- **Destructive:** chip fill with signal-red-text (#F87171) label, always confirmed.

### Chips
- **Style:** 26 px tall, 8 px radius, chip fill, label type, 10 px side padding, 6 px apart, right-aligned in settings rows.
- **States:** hover = chip-hover; selected = tab-well fill, periwinkle hairline and periwinkle text. Multi-select chips (alerts) follow the same rule for each chip that is on.

### Toggles
- 40×22 capsule. The track blends from chip to Capri Periwinkle and the 16 px knob slides 18 px while blending from dim to white, over 140 ms.

### Steppers
- 124×26 chip with ‹ › glyph hit zones 30 px wide at each end and the value plus its unit in the middle.

### Cards / Containers
- **Corner Style:** 10-12 px (16 px for the event window).
- **Background:** surface (`#16171B`).
- **Shadow Strategy:** none; see Elevation & Depth.
- **Border:** 1 px white at ~10 % alpha.
- **Internal Padding:** about 14 px.

### Inputs / Fields
- **Style:** a surface or chip fill with an 8 px radius, label or body type.
- **Focus:** a 1 px Capri Periwinkle edge. Secrets show as dots once a key is stored.

### Navigation (settings sidebar)
- 176×36 rows, 9 px radius, Fluent icon plus a body label. Active: a chip-colored row that slides from the old section to the new one (160 ms), with a 3×18 periwinkle bar on its left edge and a periwinkle icon. Hover: sidebar-hover fill.

### Category Dot
- 12 px circle in the category color; selected gets an 18 px ring of white at ~59 % alpha.

## Do's and Don'ts

### Do:
- **Do** start every surface from `glass-core` / `settings-ground` and step lighter with surface → tab-well → chip for anything that comes forward.
- **Do** use Capri Periwinkle only for active, on and focused states, and for live data lines (progress, sparkline).
- **Do** give every new glow, gradient or animation a solid, static fallback that looks finished, under the existing Rendimiento switches.
- **Do** keep collapsed-pill content on one line in 9pt Segoe UI; widen the pill, never heighten it.
- **Do** use Segoe Fluent Icons glyphs for icons, falling back to MDL2 Assets.
- **Do** use cubic ease-out (140-220 ms) for state changes and the spring only for the notch's size.

### Don't:
- **Don't** stroke or light the notch's top edge; it must melt into the bezel.
- **Don't** add drop shadows; depth is tone and light.
- **Don't** introduce a second accent color or use status/category colors as decoration.
- **Don't** give more than one element per view the disc-white fill.
- **Don't** wrap text inside the notch; truncate with an ellipsis.

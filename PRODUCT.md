# Product

<!-- impeccable:product-schema 1 -->

## Platform

windows

Native Windows 11 desktop app (C++, Win32 + GDI+, layered `WS_EX_LAYERED` window with per-pixel alpha). Not web, not a web wrapper: browser-only tooling (live mode, the HTML detector) does not apply.

## Users

One user for now: the author, on their own Windows 11 desktop. It is a personal tool; wider distribution is not a current goal and nothing should be designed around strangers' onboarding yet.

## Product Purpose

A Dynamic Island-style notch for Windows 11: a small pill at the top center of the screen that shows the most relevant live thing at a glance (charger notice, running pomodoro, music, next event or tasks, clock) and expands into a panel with tabs for music and lyrics, pomodoro, calendar and tasks, AI, weather, system stats and a file shelf. Success: it is the one place the author checks during the day, and it never makes the PC feel heavier.

## Positioning

All four, together:

- **Featherweight**: native C++, under 25 MB RAM; heavy work (Whistle, Mirage) runs in a separate process that dies when its tab closes, sensors and network run only while their tab is open.
- **Private by default**: local AI models, secrets encrypted with DPAPI, no shared keys in the binary, nothing leaves the PC unless a feature that needs the network is in use.
- **All-in-one hub**: calendar with Google/Outlook/ICS sync, tasks, pomodoro, music with lyrics, files, weather and system in one pill.
- **Feels alive**: the mascot eyes, the ambient light, 60 fps animation. Personality is part of the product, not decoration.

## Operating Context

Always on screen, glanced at between other work. Opened by click, hover or voice; closed by clicking away. Settings live in a separate window opened from the tray icon, the pill's context menu or the gear in the panel's tab row. Laptop use on battery is real: there is a battery saver that drops animation and lights and runs at 30 fps.

## Capabilities and Constraints

- Collapsed pill 220x34 px, expanded panel 460x250 px, large calendar 760x400 px (current values in code, not frozen by the user).
- RAM budget under 25 MB is the one hard limit.
- UI copy is Spanish.
- Every visual effect already has a settings toggle under Rendimiento; keep that pattern for new ones.
- No other constraints set by the user ("nada en específico").

## Brand Commitments

Name: Capri Notch. Mascot with eyes in the idle pill. Dark, fluid look in the current build (incumbent, not yet documented in DESIGN.md).

## Evidence on Hand

README.md documents every feature and the sync setup. No users, testimonials, metrics or screenshots beyond the author's own use; do not invent any.

## Product Principles

1. Weight is a feature: if something costs memory or CPU while unused, it is wrong.
2. Glance first: the pill answers "what matters right now" without a click.
3. Nothing leaves the machine by surprise.
4. Personality earns its keep only when it can be switched off.

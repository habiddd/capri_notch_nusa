---
target: src/settings_ui.h
total_score: 24
max_score: 40
na_heuristics: 
p0_count: 0
p1_count: 3
target_identity: "file:c:\\Users\\WinterOS\\Desktop\\capri_notch\\src\\settings_ui.h"
target_fingerprint: "sha256:f45b97afa27687b5e1bd69c8f59b2f50e281198cdca06d2f3178f5fed262ac4b"
target_path: "c:\\Users\\WinterOS\\Desktop\\capri_notch\\src\\settings_ui.h"
timestamp: 2026-11-17T04-48-38Z
slug: src-settings-ui-h
---
Method: dual-agent (A: design review · B: detector + grep scan)

## Design Health Score
| # | Heuristic | Score | Key Issue |
|---|---|---|---|
| 1 | Visibility of System Status | 3 | Failed connect shows error text with a grey dot (settings_ui.h:688 checks !connected before error) |
| 2 | Match System / Real World | 3 | "Compacta:" jargon; compact pill options live outside Píldora |
| 3 | User Control and Freedom | 2 | Esc in a field commits it (377); no cancel |
| 4 | Consistency and Standards | 2 | Selected chips all disc-white (3 at once in Rendimiento) vs DESIGN.md rule; hand cursor on fields |
| 5 | Error Prevention | 2 | Open() discards typing (43-44); Desconectar confirms only with pending changes |
| 6 | Recognition vs Recall | 3 | Category dots color-only; Modelo shows preset as typed text |
| 7 | Flexibility and Efficiency | 1 | WM_CHAR only; sidebar/toggles/chips/buttons mouse-only |
| 8 | Aesthetic and Minimalist | 3 | Info-only rows styled like control rows |
| 9 | Error Recovery | 2 | Invalid port / empty host dropped silently (321-325); errors truncated at 220 px |
| 10 | Help and Documentation | 3 | Ver guía opens README that only exists beside a dev build |
| Total | | 24/40 | Acceptable |

## Design Specificity Verdict
Visually specific (tonal layers, sliding accent chip, blending toggles, live RAM figure, specific privacy copy); structurally a generic flat toggle list. Detector: exit 0, [] (does not parse C++). Grep: text contrast passes (6.07 dim on bg); non-text fails (off track ~1.4:1, grey dot <3:1); toggle hit 22 px; no WM_KEYDOWN/VK_/UIA.

## Priority Issues
- [P1] Edits lost or committed wrongly: Open() discards draft, Esc commits, invalid port/host silently dropped. Fix: CommitFocus in Open, Esc restores, red caption. /impeccable harden
- [P1] Mouse-only, no accessibility: add kbFocus over hits, Tab/Shift+Tab/Space/Enter/arrows/Ctrl+Tab, focus ring, WM_GETOBJECT names. /impeccable harden
- [P1] Cuentas unstructured: provider headers, numbered steps with ID before Conectar, scroll thumb, notes as helper text. /impeccable layout
- [P2] Destructive actions look benign: red text, always confirm Desconectar with event count, confirm model Borrar. /impeccable clarify
- [P2] Pill settings scattered, no disabled state: move into Píldora per view, drop duplicate lyric toggle, dim dependents. /impeccable distill

## Persona Red Flags
Alex: no keyboard section switch; append-only text; Tab focuses off-screen field; only toggle track clickable; slow scroll.
Sam: zero accessibility exposure; 7.5pt captions, no text scaling; off track 1.4:1, fields near-invisible; color-only categories; no high contrast.

## Minor Observations
Title/sidebar misalignment; city placeholder "Sin definir" vs "Automática"; Letras description truncates; Salir redundant; key field doesn't name its provider.

## Questions to Consider
Organize sidebar by pill view? Cuentas as one card per provider with setup behind Conectar? A moment of mascot personality in settings?

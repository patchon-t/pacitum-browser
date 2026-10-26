# Pacitum 2.0 design system

Source of truth: the **Pacitum 2.0 Browser UI** design canvas (12 screens: new tab light/dark,
macOS light/dark, More menu, Settings, Themes, Extensions, Extension detail, Permissions,
Profiles, Web pop-ups). In code, every token lives in `PacitumPalette` (`include/ThemeManager.h`)
and reaches the widgets through the stylesheets in `src/ThemeManager.cpp` as `@name`.

## Audit (1.9.1 → 2.0)

**Token coverage**

| Category | In tokens | Hardcoded found | Notes |
|---|---|---|---|
| Colours | 25 palette roles | ~55 hex values in 8 files (DownloadManager 11, ProfileManager 10, BrowserWindow 5, …) | Most are file-type / avatar swatches (allowed: content colours). |
| Type | 1 family (Inter), 13 px base | 1 monospace | 2.0 switches to **Rubik**. |
| Radii | ad hoc (4, 5, 6, 8, 9, 10, 12, 13, 14, 15, 16, 17, 18, 22 px) | 14 distinct values | 2.0 scale: 8 · 10 · 12 · 14 · 18 · 22. |
| Motion | `Motion::` tokens (Quick/Base/Medium/Slow, OutCubic/OutBack) | — | Kept; reduced-motion respected. |

**Naming:** legacy aliases (`frame`=chrome, `tab`=elevated, `toolbar`=card) stay for callers.
**Gap found:** one `accent` was doing two jobs (brand colour *and* "primary action"). 2.0 splits it:

- `accent` — Pacitum red. Indicators only: active-tab bar, caret, selected-nav bar, focus on the omnibox (dark), the logo.
- `primary` / `onPrimary` — ink. Primary buttons, switches/checkboxes "on", selection, progress.

## Tokens

### Colour

| Token | Light | Dark | Used for |
|---|---|---|---|
| `frame` (chrome) | `#E8E2DE` | `#0E0B0C` | Tab strip, window frame |
| `tab` / `toolbar` | `#FFFFFF` | `#1B1617` | Active tab, toolbar, cards |
| `page` | `#F7F4F1` | `#141011` | Page and window ground |
| `surface` | `#EFE9E5` | `#2A2324` | Hover |
| `surfaceHover` / `pressed` | `#E2DBD6` | `#322B2C` | Pressed, active |
| `chip` (in `omnibox`) | `#F1ECE8` | `#141011` | Omnibox, segmented controls |
| `border` | `#E2DBD6` | `#322B2C` | Dividers, card outlines |
| `borderStrong` | `#CFC7C2` | `#3D3536` | Menus, switch "off" |
| `text` (ink) | `#151112` | `#F6F1EF` | Body text, icons |
| `textSecondary` | `#6E6561` | `#A89E9B` | Secondary text |
| `textTertiary` | `#9C928D` | `#7A7071` | Hints |
| `disabled` | `#B8AFAA` | `#5E5556` | Disabled icons/text |
| `accent` | `#D9141F` | `#FF3B47` | Pacitum red (indicators) |
| `primary` / `onPrimary` | `#151112` / `#F7F4F1` | `#F6F1EF` / `#141011` | Primary actions |
| `focusRing` | `#151112` | `#F6F1EF` | 2 px focus outline |
| `secure` | `#1F7A57` | `#4CC38A` | Lock, "secure" |
| `danger` | `#A30F19` (soft `#FBE4E6`) | `#FF6B73` | Destructive |
| `warning` | `#B8470F` | `#F2A65A` | Warnings |

Private windows tint the frame: light `#E6E1EC`, dark `#16121C`.

### Type
**Rubik** (bundled, OFL; variable 300–900). Fallbacks: Segoe UI, Helvetica Neue, Arial.

| Role | Size / weight |
|---|---|
| Page title (h1) | 26–28 px / 600, −0.01 em |
| Section title | 15–16 px / 600 |
| Body / rows | 14–14.5 px / 400–500 |
| UI default, tabs | 13 px / 400 (active tab 500) |
| Secondary, meta | 12.5 px / 400 |
| Chip label | 12 px / 500 |

Numbers use tabular figures.

### Radius
`8` small chips, close buttons · `10` small buttons · `12` icon buttons, rows, menu items · `14` omnibox, tab top corners, inputs · `18` new-tab search · `22` cards. Avatars and switches are pills.

### Size and spacing
Toolbar 60 px, tab strip 50 px, active tab 40 px tall (236 px wide), inactive 36 px; icon button 40×40, icon 18–19 px with 2.2 px strokes; omnibox 42 px; rows 42–58 px; base gap 4 px, content padding 14/40/56 px.

### Motion
Hover/press colour changes 150 ms. Menus and pop-ups zoom from where they were opened (`Motion::popIn`, OutBack overshoot); reduced motion turns movement off and keeps ≤120 ms fades.

## Components

**Icon button** — 40×40, radius 12, transparent; hover `surface`, pressed `surfaceHover`, disabled icon `disabled`. Toolbar toggles (Extensions, Downloads, Profile, More): **a second click on the same button closes its pop-up** (2.0 behaviour; before, it reopened it and could blink).

**Tab** — active: `tab` fill, radius 14 14 0 0, a 28×3 px `accent` bar centred on the top edge, weight 500; inactive: transparent, `textSecondary`, hover `surface`.

**Omnibox** — `omnibox` fill, radius 14, 42 px; focused: `toolbar` fill and a 2 px ring (`focusRing` light / `accent` dark); internal pages show a "Pacitum" chip (`primary` on `onPrimary`).

**Button** — primary: `primary` fill, `onPrimary` text, radius 12; secondary: `toolbar` fill, 1 px `border`; danger: `danger` text, 1 px soft-red border, hover `#FBE4E6`.

**Switch** — 40×24 (48×28 large), on `primary`, off `borderStrong`, white 18 px knob.

**Sidebar nav (Settings, Extensions)** — rows 42 px radius 12; selected: `toolbar` fill, weight 500, a 4×20 px `accent` bar on the left edge.

**Menu** — `toolbar` fill, 1 px `borderStrong`, radius 16, items 34 px radius 10.

## Brand

- App icon: `resources/brand/icon_light.svg` (black P + red bar on white) and `icon_dark.svg` (white P on black).
- Wordmark: `Logo_full_light.svg` / `Logo_full_dark.svg` ("PACITUM", the C and T bars in red).
- **NEXT PROJECT** mark (`next_project.svg`): shown in About Pacitum.

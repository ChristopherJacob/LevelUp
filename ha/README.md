# LevelUp Bubble Card

A clean, modern spirit-level display for Home Assistant. Shows real-time roll and pitch from
the LevelUp van leveling system.

![screenshot](../images/ha/screenshot.png)

## Installation

1. Copy `levelup-bubble-card.js` to your HA `www` folder:
   ```
   /config/www/levelup-bubble-card.js
   ```

2. Add as a resource:  
   **Settings → Dashboards → ⋮ → Resources → Add Resource**
   ```
   URL: /local/levelup-bubble-card.js
   Type: JavaScript Module
   ```

3. Add to dashboard as a manual card:
   ```yaml
   type: custom:levelup-bubble-card
   roll_entity: sensor.levelup_roll
   pitch_entity: sensor.levelup_pitch
   tolerance: 1.5
   title: Van Level
   ```

## Options

| Option | Default | Description |
|--------|---------|-------------|
| `roll_entity` | *required* | Entity ID for roll (degrees) |
| `pitch_entity` | *required* | Entity ID for pitch (degrees) |
| `tolerance` | `1.5` | Degrees within which the bubble turns green |
| `title` | `''` | Title shown above the ring |
| `multiplier` | `5` | Sensitivity of bubble movement |
| `show_values` | `true` | Show numeric roll/pitch readout |
| `size` | `280` | Width in pixels |

## Design

- Dark-mode friendly with glass-morphism aesthetic
- Smooth 60fps CSS transitions on bubble movement
- Green glow when within tolerance (LEVEL state)
- Minimal crosshair target + ring indicator
- Monospace numeric readout
- Inter font stack throughout

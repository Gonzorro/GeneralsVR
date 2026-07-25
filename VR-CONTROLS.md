# GeneralsVR controls

Launch from the **GeneralsVR** desktop shortcut (Quest Link running).

## Menus

The game's interface floats on a screen in front of you. Point the **right controller** and pull
the **right trigger** to click. **B** or **Y** skips the intro movie.

Mouse and keyboard still work at all times, the monitor mirrors everything. Moving the mouse
takes control instantly. To hand control back to the lasers, either leave the mouse alone for a
few seconds while using the controllers, or simply squeeze the trigger three times quickly.

## In a battle

The **right hand** casts a **laser**, and it is your **reticle**: it takes the colour of whatever
the game would do if you pulled the trigger now. The left hand has no beam. It moves you, gives
orders, and holds the HUD.

| Beam colour | Meaning |
|---|---|
| 🔵 Cyan | Nothing selected / nothing to do |
| 🟢 Green | Move here |
| 🔴 Red | Attack this |
| 🟠 Amber | Capture / enter / repair this |

### Commanding

| Action | Control |
|---|---|
| Select | **Right trigger** |
| **Select many** | Hold the **right trigger** and sweep across the ground. A box is drawn as you go; release to take everything of yours inside it |
| Order (move / attack / capture / enter / repair) | **A**, or the **left trigger** |

Selected units carry a **green bead** with a **health bar** under it.

### Moving and resizing

| Action | Control |
|---|---|
| Move | **Left thumbstick**, relative to where you are *looking* |
| Turn | **Right thumbstick ←/→** |
| **Resize yourself** | **Right thumbstick ↑** = grow (the map falls away, you see the whole battle) · **↓** = shrink (down among the tanks) |
| Resize by hand | Hold **both grips** and pull your hands apart / push them together |
| Recenter | The **≡ menu button** (left controller) |

The right stick does one thing at a time, whichever way you pushed it hardest.

### The HUD

| Action | Control |
|---|---|
| Summon / dismiss the panel on a hand | That hand's **secondary button** (**Y** left, **B** right) |

It carries the game's real interface: minimap, command bar, whole menus. Point at it with the
**other** hand (the holding hand's laser is hidden so it doesn't lie across what you're reading).

Under it sits a bar of **ten control-group slots**: point and **trigger** to recall a squad, or
hold the **left hand's button** while triggering to save the current selection into it.

## Tuning

The launcher stores its settings in `generalsvr.json` inside `%LOCALAPPDATA%\GeneralsVR`. Edit
the `flags` line:

- `-vrscale 500`: starting size (world units per real metre). Bigger = you are bigger and the map
  looks smaller. The sticks and grips change this live, between 80 and 4000.
- `-vrres 1.0`: per-eye render resolution. Drop to `0.7` if the frame rate feels choppy.

## Known gaps

- The laser does not draw **over** the HUD panel (the panel is a compositor layer, which has no
  depth). The panel shows the game's own cursor instead.
- The panel looks slightly washed out; its alpha is not forced to full where the UI painted.
- Unit **shadows** do not appear at all distances yet.

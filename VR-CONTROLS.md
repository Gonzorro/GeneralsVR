# GeneralsVR controls

Launch from the **GeneralsVR** desktop shortcut (Quest Link running).

## Menus

The game's interface floats on a screen in front of you. Point the **right controller** and pull
the **right trigger** to click. **B** or **Y** skips the intro movie.

Mouse and keyboard still work at all times, the monitor mirrors everything. Moving the mouse
takes control instantly. To hand control back to the lasers, either leave the mouse alone for a
few seconds while using the controllers, or simply squeeze the trigger three times quickly.

## VR settings

Every in game menu carries a **[VR] badge** in its top left corner. Click it (ray or mouse) to
open the VR settings: world scale, smooth motion, input mode, stick speed, left handed mode,
unit shadows, sky, assets, audio. Right trigger changes a setting, left trigger closes the menu.
Everything is remembered between sessions.

**Left handed?** Controls tab, Handedness. It mirrors the whole scheme: beam on the left hand,
panel on the right, every button follows.

## In a battle

The **right hand** casts a **laser**, and it is your **reticle**: it takes the colour of whatever
the game would do if you pulled the trigger now. The left hand has no beam. It moves you, gives
orders, and holds the HUD.

| Beam colour | Meaning |
|---|---|
| 🔵 Cyan | Nothing selected / nothing to do |
| 🟢 Green | Move here |
| 🔴 Red | Attack this |
| 🟠 Amber | Capture / enter / repair, or a special power waiting for a target |
| 🟣 Violet | Attack move armed, release to send |
| 🟦 Teal | Guard armed, release to place |

### Commanding

| Action | Control |
|---|---|
| Select | **Right trigger** |
| **Select many** | Hold the **right trigger** and sweep across the ground. A box is drawn as you go; release to take everything of yours inside it |
| Select all of a type | **Double tap** one of your units |
| Cancel a pending order | **Left trigger** |

Selected units carry a **green bead** with a **health bar** under it.

### The command dial

**Tap the left stick** (click it in, no holding) and a dial opens: **Stop, Attack move, Guard,
Scatter, Cheer, Idle worker, Menu**. Flick the stick towards a slice and let it return to fire,
or point the ray at a slice and pull the trigger. Attack move and guard arm the beam; the order
goes where you release the trigger. Cancel the dial with the left trigger, by tapping the stick
again, or just wait.

### The group dial

**Hold B** and a second dial opens with your **ten control groups**. Flick to recall a group.
Hold the **right trigger** while flicking to save the current selection into that slot. Flick
the same group twice quickly and the camera jumps to it.

### Moving and resizing

| Action | Control |
|---|---|
| Move | **Left thumbstick**, relative to where you are *looking* |
| Turn | **Right thumbstick ←/→** |
| **Resize yourself** | **Right thumbstick ↑** = grow (the map falls away, you see the whole battle) · **↓** = shrink (down among the tanks) |
| Resize by hand | Hold **both grips** and pull your hands apart / push them together |
| Jump to selection | **Click the right stick** (double click = last radar event) |
| Recenter | The **≡ menu button** (left controller), short press |
| Follow camera | The **≡ menu button**, hold. Hold again to release |

### The HUD

| Action | Control |
|---|---|
| Summon / dismiss the panel on a hand | That hand's **secondary button** (**Y** left, **B** right, short press) |

The left panel opens by itself when a battle starts, and whenever the game opens a menu. It
carries the game's real interface: minimap, command bar, whole menus. Point at it with the
**other** hand.

Under it sits a bar of **ten control-group slots**: point and **trigger** to recall a squad, or
hold the **left hand's button** while triggering to save the current selection into it.

## Tuning

Use the in VR settings menu, it covers everything and remembers your choices
(`%LOCALAPPDATA%\GeneralsVR\vr-settings.ini`). The launcher's `-vrscale 500` flag in
`generalsvr.json` only sets the starting size for a fresh install; the menu's world scale slider
(80 to 900) wins after that.

## Known gaps

- **Attack move** from the dial can still act like a plain move on some targets. Being hunted;
  send me your `Debug` folder if you hit it.
- Unit **shadows** can blink at some head angles. The game's shadow volumes were built for one
  fixed camera; accepted for now, or turn shadows off in the settings menu.
- Don't leave the game **paused with the headset off** for long: if the Quest goes to sleep
  while the game sits paused, the game can crash. Take the pause off first or keep the
  headset awake.

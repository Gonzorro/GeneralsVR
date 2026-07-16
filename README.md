# GeneralsVR

I'm turning **Command & Conquer: Generals — Zero Hour** into a VR game.

Not a remake, not a port to another engine — the **original game**, running natively in a VR
headset. You stand over the battlefield like a general at a war table: look around the map,
point a laser to select your units, give orders with the motion controllers, build your base,
and resize yourself from "the whole war on a table" down to "standing among the tanks".

> **Early alpha — looking for testers.** It's playable end to end (skirmish), but expect rough
> edges. Found a bug? [Open an issue](https://github.com/Gonzorro/GeneralsVR/issues/new/choose).

## What you need

- **Your own copy of Zero Hour** from Steam
  ([C&C Ultimate Collection](https://store.steampowered.com/bundle/39394/)). This project
  contains **none of the game's assets** — it does not work without a real installation.
- **Meta Quest 3 with Quest Link** (cable or Air Link). Other PC VR headsets are untested.
- A Vulkan-capable gaming PC.

## Install

**Before you start:** install Zero Hour from Steam and launch it once, then install the
[Meta Quest Link app](https://www.meta.com/quest/setup/), connect the headset, and set Meta as
the active OpenXR runtime (Quest Link app → Settings → General).

Then:

1. Download **`GeneralsVR-v*.zip`** from the [latest release](https://github.com/Gonzorro/GeneralsVR/releases/latest).
2. **Extract it into its own folder** (right-click → Extract All, or drag it out of the zip).
   Anywhere is fine — your Desktop or Downloads.
3. Open that folder and double-click **`START-GeneralsVR`**.
   - Windows SmartScreen may warn about an unrecognized app — that's normal for an unsigned
     community mod; click **More info → Run anyway**.
   - It'll ask for administrator permission once (to write the registry entries the game
     needs) and put a **GeneralsVR** shortcut on your desktop.
4. Put the headset on, press **Enter** in the launcher, start a **Skirmish**, and look around.

From then on just use the **GeneralsVR** desktop shortcut.

> **Tip:** the headset only shows the battlefield — it stays dark in the menus. You won't see
> anything until you're actually in a skirmish.

**Your Zero Hour install is never touched.** Everything lives in its own folder
(`%LOCALAPPDATA%\GeneralsVR`) — no game files are copied or changed. To uninstall, delete that
folder and the desktop shortcut (or run `GeneralsVR.ps1 -Uninstall`).

The launcher auto-updates to my newest build every time you play. If an update ever breaks
something for you, press **V** in the launcher and pick the previous version — you stay on it
until you switch back.

### Prefer one click? (optional)

Instead of the zip, you can download **`GeneralsVR-Setup.cmd`** from the release and run it — it
downloads and sets everything up for you, no extracting. The zip above is the manual route and
does exactly the same thing.

## Controls

See **[VR-CONTROLS.md](VR-CONTROLS.md)**.

## Open source this stands on

- **EA's official source release** of Generals/Zero Hour (GPL v3) — the game code itself.
- **[TheSuperHackers/GeneralsGameCode](https://github.com/TheSuperHackers/GeneralsGameCode)** —
  the community project that modernized that code so it builds and runs today. This repo is a
  fork of it; everything VR is mine, everything else is theirs and upstream's.
- **[DXVK](https://github.com/doitsujin/dxvk)** — translates the game's ancient DirectX 8
  rendering to Vulkan, which is what makes a modern VR pipeline possible at all.
- **[OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK)** — the open standard that talks
  to the headset.

## Is this legal?

Yes. EA released this game's source code under the GPL v3 in 2025, and GeneralsVR is a
modification of that source, published under the same license. No EA assets, art, audio or data
are distributed — all of that stays inside your own Steam installation.

## License

GPL v3 — see [LICENSE.md](LICENSE.md).

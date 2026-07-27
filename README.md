# GeneralsVR

I'm turning **Command & Conquer: Generals - Zero Hour** into a VR game.

Not a remake, not a port to another engine. This is the **original game**, running natively in a VR
headset. You stand over the battlefield like a general at a war table: look around the map,
point a laser to select your units, give orders with the motion controllers, build your base,
and resize yourself from "the whole war on a table" down to "standing among the tanks".

> **Early alpha, looking for testers.** It's playable end to end (skirmish), but expect rough
> edges. Found a bug? [Open an issue](https://github.com/Gonzorro/GeneralsVR/issues/new/choose)
> and attach the files from `%LOCALAPPDATA%\GeneralsVR\Debug`. That folder is exactly why it
> exists: it makes your report ten times more useful to me.

## What you need

- **Your own copy of Zero Hour.** Steam
  ([C&C Ultimate Collection](https://store.steampowered.com/bundle/39394/)), EA app, Origin, GOG,
  or a retail disc all work. This project contains **none of the game's assets**, so it does not
  work without a real installation. (The launcher finds Steam/EA/GOG automatically; for anything
  else it asks you to point it at your game folder once.)
- **Meta Quest 3 with Quest Link** (cable or Air Link). Other PC VR headsets are untested.
- A Vulkan-capable gaming PC.

## Install

**Before you start:** install Zero Hour from Steam and launch it once, then install the
[Meta Quest Link app](https://www.meta.com/quest/setup/), connect the headset, and set Meta as
the active OpenXR runtime (Quest Link app → Settings → General).

Then:

1. Download **`GeneralsVR-v*.zip`** from the [latest release](https://github.com/Gonzorro/GeneralsVR/releases/latest).
2. **Extract it into its own folder** (right-click → Extract All, or drag it out of the zip).
   Anywhere is fine, your Desktop or Downloads both work.
3. Open that folder and double-click **`START-GeneralsVR`**.
   - Windows SmartScreen may warn about an unrecognized app. That's normal for an unsigned
     community mod; click **More info → Run anyway**.
   - It'll ask for administrator permission once (to write the registry entries the game
     needs) and put a **GeneralsVR** shortcut on your desktop.
4. Put the headset on, press **Enter** in the launcher, start a **Skirmish**, and look around.

From then on just use the **GeneralsVR** desktop shortcut.

> **Tip:** the headset only shows the battlefield. It stays dark in the menus. You won't see
> anything until you're actually in a skirmish.

**Your Zero Hour install is never touched.** Everything lives in its own folder
(`%LOCALAPPDATA%\GeneralsVR`), and no game files are copied or changed. To uninstall, delete that
folder and the desktop shortcut (or run `GeneralsVR.ps1 -Uninstall`).

### What's in the folder

Since v0.2.0 the install is organized so you always know where things are:

- **`START-GeneralsVR`** at the top: the only thing you ever need to double-click.
- **`Data\`**: the game build itself (`generalszhv.exe` and everything it needs). Don't start
  it directly; it only works when the launcher points it at your game.
- **`Debug\`**: every log the game writes lands here. **Reporting a bug? Attach the files from
  `%LOCALAPPDATA%\GeneralsVR\Debug`**, mainly `DebugLogFile.txt` and `xrhost_log.txt`. They
  contain no personal data, just what the game and the VR host were doing.
- **`Data\Enhanced\`**: drop an upscaled texture pack in here (keeping its folder structure)
  and flip **Assets** to **Enhanced** in the VR settings menu to use it. Ships empty; I can't
  bundle EA-derived packs.

The launcher auto-updates to my newest build every time you play. If an update ever breaks
something for you, press **V** in the launcher and pick the previous version. You stay on it
until you switch back.

### Prefer one click? (optional)

Instead of the zip, you can download **`GeneralsVR-Setup.cmd`** from the release and run it. It
downloads and sets everything up for you, no extracting. The zip above is the manual route and
does exactly the same thing.

## Controls

See **[VR-CONTROLS.md](VR-CONTROLS.md)**.

## Open source this stands on

- **EA's official source release** of Generals/Zero Hour (GPL v3): the game code itself.
- **[TheSuperHackers/GeneralsGameCode](https://github.com/TheSuperHackers/GeneralsGameCode)**:
  the community project that modernized that code so it builds and runs today. This repo is a
  fork of it; everything VR is mine, everything else is theirs and upstream's.
- **[DXVK](https://github.com/doitsujin/dxvk)**: translates the game's ancient DirectX 8
  rendering to Vulkan, which is what makes a modern VR pipeline possible at all.
- **[OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK)**: the open standard that talks
  to the headset.

## Is this legal?

Yes. EA released this game's source code under the GPL v3 in 2025, and GeneralsVR is a
modification of that source, published under the same license. No EA assets, art, audio or data
are distributed. All of that stays inside your own Steam installation.

## Support the project

I build this as a solo developer, and I fund the time for it between freelance gigs. If you're
enjoying GeneralsVR and you're able to, [**sponsoring me**](https://github.com/sponsors/Gonzorro)
makes a real difference: the more support this gets, the more freelance work I can turn down and
the more time goes straight into VR.

And the bigger plan: once Zero Hour is solid, I want to bring the **other Command & Conquer
games into VR** too. Every sponsor gets me closer to making that the main thing I do. No pressure
at all if you can't; starring the repo and sending bug reports helps just as much. 🫡

## License

GPL v3. See [LICENSE.md](LICENSE.md).

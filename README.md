# GeneralsVR

I'm turning **Command & Conquer: Generals — Zero Hour** into a VR game.

Not a remake, not a port to another engine — the **original game**, running natively in a VR
headset. You stand over the battlefield like a general at a war table: look around the map,
point a laser to select your units, give orders with the motion controllers, build your base,
and resize yourself from "the whole war on a table" down to "standing among the tanks".

> **Early alpha — looking for testers.** It's playable end to end (skirmish), but expect rough
> edges. Found a bug? [Open an issue](../../issues/new/choose).

## What you need

- **Your own copy of Zero Hour** from Steam
  ([C&C Ultimate Collection](https://store.steampowered.com/bundle/39394/)). This project
  contains **none of the game's assets** — it does not work without a real installation.
- **Meta Quest 3 with Quest Link** (cable or Air Link). Other PC VR headsets are untested.
- A Vulkan-capable gaming PC.

## Install

1. Install Zero Hour from Steam.
2. Install the [Meta Quest Link app](https://www.meta.com/quest/setup/), connect the headset,
   and set Meta as the active OpenXR runtime (Quest Link app → Settings → General).
3. Download **`GeneralsVR-Setup.cmd`** from the [latest release](../../releases/latest) and run
   it. Windows SmartScreen will warn about an unrecognized app — normal for an unsigned
   community mod; click **More info → Run anyway**.
4. Play from the **GeneralsVR** shortcut it puts on your desktop.

The launcher auto-updates to my newest build every time you play. If an update breaks something
for you, press **V** in the launcher and pick the previous version — you stay pinned to it until
you unpin.

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

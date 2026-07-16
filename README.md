# GeneralsVR

**Command & Conquer: Generals — Zero Hour, fully playable in VR.**

Stand over the battlefield like a real general. Look around the whole map, select units with a
laser, give orders with the motion controllers, build your base, and resize yourself from
"down among the tanks" to "the entire war on a table".

> **Status: early alpha, looking for testers.** Expect rough edges — that's the point.
> Found a bug? [Open an issue](../../issues/new/choose).

## What you need

- **Your own copy of Zero Hour** — it's on Steam in the
  [C&C Ultimate Collection](https://store.steampowered.com/bundle/39394/). This project contains
  **no game assets**; it will not work without a legitimate installation.
- **A Meta Quest 3 with Quest Link** (cable or Air Link). Other PC VR headsets are untested —
  reports welcome.
- **A Vulkan-capable gaming PC** (the mod renders through Vulkan; anything that runs modern
  games is fine).

## Install

1. Install *Command & Conquer Generals — Zero Hour* from Steam and launch it once.
2. Install the [Meta Quest Link app](https://www.meta.com/quest/setup/) and connect your headset
   (Link cable or Air Link). Make sure Meta is your active OpenXR runtime (Quest Link app →
   Settings → General → "Set Meta Quest Link as active").
3. Download **`GeneralsVR-Setup.cmd`** from the
   [latest release](../../releases/latest) and run it.
   Windows SmartScreen will warn about an unrecognized app — that's normal for an unsigned
   community mod; click **More info → Run anyway**.
4. The setup finds your Zero Hour install, downloads the newest build, sets everything up, and
   puts a **GeneralsVR** shortcut on your desktop. Play from that shortcut.

The launcher **auto-updates** to the newest release each time you play. If an update ever
misbehaves, press **V** in the launcher and pick any earlier version — you'll be pinned to it
until you unpin.

## Controls

See **[VR-CONTROLS.md](VR-CONTROLS.md)** — two lasers, a summonable HUD on either hand,
box-select by sweeping the ground, control groups, and grab-the-world resizing.

## Is this legal?

Yes. EA released the Generals/Zero Hour source code under the **GPL v3** in 2025. GeneralsVR is
a modification of that source (via the community's
[GeneralsGameCode](https://github.com/TheSuperHackers/GeneralsGameCode) project) and is itself
GPL v3 — see [LICENSE.md](LICENSE.md). No EA assets, art, audio, or data are distributed;
everything copyrighted stays inside your own Steam installation.

## How it works (the short version)

The engine's D3D8 renderer runs through [DXVK](https://github.com/doitsujin/dxvk) (D3D8 → Vulkan),
and an OpenXR session is created directly on DXVK's Vulkan device — the battlefield is rendered
once per eye into the headset's swapchain with no extra copies. The game's own interface is
composited into the headset as floating panels driven by the motion controllers. VR code lives
mainly under `GeneralsMD/Code/GameEngineDevice/` (`VRDevice/`).

Want to build from source? Follow upstream's
[build guide](https://github.com/TheSuperHackers/GeneralsGameCode/wiki) — this fork builds the
same way (`build/win32-log` preset); the VR work is on the `feature/openxr-vr` branch.

## Credits

- **EA** — for open-sourcing Generals/Zero Hour under the GPL.
- **[TheSuperHackers / GeneralsGameCode](https://github.com/TheSuperHackers/GeneralsGameCode)** —
  the community project this fork is based on: modern toolchain, hundreds of fixes.
- **[DXVK](https://github.com/doitsujin/dxvk)** (zlib license) — D3D8/9 → Vulkan translation.
- **[OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK)** (Apache 2.0) — the VR interface.

## License

GPL v3, same as the source release it derives from — see [LICENSE.md](LICENSE.md).

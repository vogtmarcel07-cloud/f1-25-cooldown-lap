# Cool-down Lap for F1 25 (Swofty Launcher addon)

After the chequered flag you keep your car and drive a cool-down lap into parc ferme instead of the game taking over.
The AI top 3 park in parc ferme too, with 1/2/3 boards in front of the cars.

Version **0.36.6** (2026-11-02), the latest version confirmed in game. Download: see
[Releases](../../releases) (`vmknolle.cooldown-lap.swoftyaddon`).

## Requirements
- F1 25 (Steam build)
- [Swofty Launcher](https://f1.swofty.net/) 1.6.10 or newer
- Works with the launcher's own bindings, no test bindings needed. Tested in Grand Prix mode.

## Install
1. Close the game.
2. Swofty Launcher: Addons -> Install -> `vmknolle.cooldown-lap.swoftyaddon`.
3. Enable it, set the options on the addon's row, start the game from the launcher.

## Options
| Option | Default | What it does |
|---|---|---|
| Cool-down lap | on | After the chequered flag you keep driving until you park in parc ferme (or the time runs out); the podium follows. |
| Cool-down lap: at most (s) | 300 | The cool-down ends on its own after this time (30-600 s). On long tracks the limit grows to a lap at about 100 km/h plus 90 s. |
| Cool-down lap: ends when | parked in parc ferme | Parked in parc ferme, pit lane entry, or time only. |
| AI top 3 park in parc ferme | on | The first three AI cars park in front of the podium too. |
| Parc ferme marking (prototype) | on | Puts the podium's parc ferme floor with the P1/P2/P3 boxes at the track's parc ferme when the cool-down starts, if the game has it loaded. Every try is written to loader.log. |
| Log details | on | Pit states, controller handovers and phase changes in the loader log. |

Parking: stop within 8 m of your board; the on-screen hint tells you how far ahead/behind and to the side it is.
After a DNF / retirement / disqualification there is no cool-down lap; the game's own end of race takes over.

## Building from source
The source is shared with the Race Start addon; this build sets `RS_PART 1` (first line of `src/cooldown_lap.cpp`)
and `SWOFTY_PUBLIC` / `RS_PUBLIC` (only the launcher's public bindings plus a few own signature scans in
`src/local_sigs.hpp`).

1. Install [zig](https://ziglang.org/) 0.15.x.
2. Get the Swofty addon SDK headers (`swofty_api.h` and `swofty/*.hpp`, MIT licence) from the Swofty addon
   template (`templates/addon/native/include`, see the
   [template docs](https://f1.swofty.net/reference/addons/template.html)) and put them in `sdk/include`,
   or set `SWOFTY_SDK=<folder>`. They are not included in this repository.
3. `./build.sh` (bash, e.g. Git Bash). It copies the SDK to `build/sdk`, applies `sdk-patch/mem_region_cache.patch`
   (a process-wide cache of VirtualQuery answers in `swofty/mem.hpp`, the 0.36.6 performance fix) and writes
   `addon/native/cooldown_lap.dll` and `dist/vmknolle.cooldown-lap.swoftyaddon`.

With zig 0.15.2 and the SDK the patch was made against, the DLL is byte-identical to the release.

## Layout
- `src/` addon source (`cooldown_lap.cpp` + its headers)
- `include/` shared vmknolle headers (live switches, timing, own hooks, track points)
- `sdk-patch/` the change to the Swofty SDK header `swofty/mem.hpp`
- `addon/` manifest (`swofty.json`) and the README that goes into the package

## Licence
MIT, see [LICENSE](LICENSE). By vmknolle.

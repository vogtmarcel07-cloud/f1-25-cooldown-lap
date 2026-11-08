# Changelog - Cool-down Lap

## 0.36.6 (2026-11-02)
- Performance: the memory checks of the game's big heaps are now shared by all game threads (before: one table per thread, and the game runs the in-game update on changing threads). Test of 0.37.4: about 6 ms per in-game update, 60 -> 27 fps.

## 0.36.5 (2026-11-02)
- No cool-down lap after a DNF: when your car is out of the race (terminal damage, retired, disqualified), the addon no longer asks you to drive to parc ferme; the game's own end of race takes over.

## 0.36.4 (2026-10-31)
- Parking at your parc ferme board is easier: stopping within 8 m counts (was 2.5 m); the hint says how far ahead / behind and to the side the board is, every second from 40 m, and 'here - stop' inside the 8 m.

## 0.36.3 (2026-10-31)
- Speed for 'parked' and for the hold check is measured from the car's movement (the game value read before was wrong, about 20 km/h always): parking in parc ferme is detected again.

## 0.36.2 (2026-10-31)
- A player who drives himself keeps his car over the line (before, one finish path always handed it to the AI).
- Parc ferme: the game's own hold (as under the red flag) stops the top 3 and the parked player instead of our per-frame pin; no slide onto the board. If a car still rolls after 5 s the old pin takes over (logged).
- Manifest category 'utility'.

## 0.36.1 (2026-10-31)
- Rebuilt on the current shared source (race start / pit lane start changes of 2026-10-31); cool-down unchanged.

## 0.36.0 (2026-10-31)
- Now its own addon again (split out of Race Start, replaces Cool-down Lap 0.4.x).
- Built for Swofty Launcher 1.6.10; own hooks checked offline on the F1 25 exe.

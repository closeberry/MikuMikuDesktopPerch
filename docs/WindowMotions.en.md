# Making window motions

[日本語](WindowMotions.md)

When a character is on a window's top edge (with "Ride on the active window" on), she can rest, or move along the window, with VMD motions, besides the "sit" and "hang" the app makes.

- **Built in**: motions people give are put into the exe for everyone (see "Giving motions" below)
- **Your own**: "Motion" → "Open window motions..." in the right-click menu picks VMDs on your computer (one or more; kept per character)

Each time she gets on a window, one of those she can use (built-in ones, your own, and the app's "sit" and "hang") is picked and played over and over. If there are moving ones, now and then (every 20–60 seconds) she moves along the window with one, then goes back to resting as before. "Gestures..." in the settings chooses which to use (the built-in ones, and the app's "sit" and "hang").

## How to make them

**Make the floor (height 0) the window's top edge.** Whatever goes below the floor hangs in front of the window (legs over the edge when sitting on it, for example).

- **Facing the front** (MMD's initial direction: the camera is in front)
- They are played as a **loop**: make the first and last frames the same pose, so the seam does not stop or jump
- **Do not move 全ての親.** Move the body up and down with センター or グルーブ
- Any length (resting ones of about 2–10 seconds work well)
- Facial morphs are not used; the app adds blinking and the mouth
- Hair and skirts move by physics: no need to key them (keys there are not used)

### In front of the window, or behind it

The app decides from the motion:

- **One that lowers the body by more than about 60% of its height** (the average of what センター and グルーブ lower it) is drawn **behind** the window: hanging from the edge by the hands, peeking over it, and so on
- Anything else is drawn **in front** of the window: sitting on the edge, lying on it, hugging the knees, chin in hand, and so on

### Resting, or moving

One whose センター ends up away from where it started (by about 10% of the height or more) is a **moving motion**. She really travels as far as one loop goes (the app moves her exactly as the motion advances, so a hand or knee on the floor does not slide).

- **Moving forward** (センター moves along Z: crawling on hands and knees, and so on): make it go toward the camera (-Z). The app turns her to the side to go left or right, as when walking
- **Moving sideways** (センター moves along X: scooting on the bottom facing the front, and so on): either way is fine. To go the other way, the app plays it with left and right swapped (as in a mirror)
- Move the foot IKs along with センター (so a foot on the floor stays where it is)
- Ones behind the window work too (going along hand over hand while hanging, and so on). She moves only with ones on the same side (in front or behind) as she was resting

## Giving motions

Attach the VMD files and how to credit you to a GitHub issue. The bones (hair, skirt, ...) and morphs of the model they were made on are left out, and the seam and the movement smoothed, before they are built in.

## Checking

How your own were taken (in front or behind, resting or moving) is written to the log (`%LOCALAPPDATA%\MikuMikuDesktopPerch\log.txt`) when you add them.

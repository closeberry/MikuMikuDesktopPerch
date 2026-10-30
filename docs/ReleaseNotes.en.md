# Release notes

[日本語](ReleaseNotes.md)

## 1.5.4

### New

- **A choice of gestures for each character**: "Gestures..." in the settings has "A choice for (name) only". Checked, the character the settings were opened on gets its own choice of which idle gestures, built-in idle motions and window motions to use, apart from the other characters (it starts as a copy of the shared choice; unchecking it goes back to the shared one). Characters without one follow the shared choice as before. In the ini, it is `OwnGestures` / `OwnDisabledGestures` in `[Mascot]`–`[Mascot6]`

## 1.5.3

### New

- **Othello**: "Play Othello" in the right-click menu opens a window where you play Othello against that character
  - She comes to the same Japanese-style room as for mahjong and hanafuda, sits at the table and reads ahead (the ending she reads through to the last disc)
  - The squares you can lay on are marked in gold; pointing at one puts a gold dot on the discs that would turn over. Click (or the arrow keys and Enter) to lay your disc; with nowhere to go, you pass by yourself
  - Discs are laid on the board, and those caught turn over one after another outward from the one laid (with the sounds of discs laid and turned). The last disc laid has a red dot
  - Who has black (and moves first) is drawn by lot for the first game, then goes back and forth
  - She talks when you take a corner, at a pass and at the game's end, those watching now and then comment, and she says what she thought back on the desktop. The camera turns as at mahjong
  - Playing a game to its end raises bond by 3 (up to 9 a day)
  - The design (in Japanese): `docs/Othello.md`

## 1.5.2

### New

- **Hanafuda (koi-koi)**: "Play hanafuda (koi-koi)" in the right-click menu opens a window where you play koi-koi against that character (twelve rounds, January to December)
  - She comes to the same Japanese-style room as for mahjong and sits at the table, playing her own hand from the cards she can see (taking the cards your yaku need, and weighing koi-koi against stopping)
  - The field, the pile and both piles taken lie on the 3D table, and the cards fly across it as they are dealt, played, turned up and taken (with the sound of cards slapped down)
  - Click a card in your hand (or ←→ and Enter) to play it. On your turn, the cards in your hand that would take from the field are rimmed in gold. When a card matches two on the field, click the one to take. With a yaku made, choose koi-koi or stop with the buttons (or 1 and 2)
  - The yaku: the five, four, rainy four and three brights, viewing the cherry and the moon with sake, boar-deer-butterflies, red poem and blue ribbons, animals, ribbons and chaff, and the deal wins (four of a month, four pairs). Seven points or more, and a win after the other's koi-koi, are doubled
  - She talks at a koi-koi, a win and the game's end, those watching now and then comment, and she says what she thought back on the desktop. The camera turns as at mahjong
  - Playing all twelve months raises bond by 3 (up to 9 a day)
  - The design (in Japanese): `docs/Hanafuda.md`

### Fixed

- In the games (mahjong, air hockey and the others), the expression of a line she said (smiling eyes and the like) stayed on her face for good
- Her lines in the games are voiced too, as the voice settings say
- Her lines through the LLM in the games fit the moment better (she no longer congratulates you on her own win, for one)

## 1.5.1

### Changed

- **More natural walking and running**: remade after the way people walk and run. The knee straightens as the foot lands, and the feet land on a narrow track under the hips. The run has a real flight and bounce, and the arms swing as people's do. It is fitted to each model's legs, shins, feet and stance, so it looks alike on every model (the knees point the same way on knock-kneed and bow-legged models too)
- **Natural standing, sitting and jumping**: the stance and the way the toes turn out, the hanging arms and the head's tilt when standing, the posture and arms when sitting, and the arm swing and knee bend of a jump follow how people stand, sit and jump, alike on every model
- Entering air hockey, mahjong and the other games stops her gestures, expression and talk, and she starts standing with a plain face

### Fixed

- At air hockey, her hand could reach only one side of the table when the game began after she had walked or during a gesture
- At mahjong, an eye morph from her expression before the game could stay
- At mahjong she looks across at you as she talks, not up at the ceiling
- At air hockey, mahjong and the dungeon, "Stop" and "Continue" after Esc can be clicked with the mouse

## 1.5.0

### New

- **Air hockey**: "Play air hockey" in the right-click menu opens a window where you play air hockey against that character (first to 7)
  - She comes from the desktop and stands across the table, bent over it with her mallet in her hand, guarding and striking forward. Her upper body turns toward her hand, and her eyes follow the puck
  - The mouse moves your mallet; the cursor stays over your half while you play
  - Keys 1-4 choose the puck's speed and 5-8 the size of the puck and mallets (kept for the next time). Space pauses, Esc stops, and Enter plays again once a game is over
  - Drag with the right button to turn the camera (the middle button or Shift and the right button to move it, the wheel to come nearer, a right double-click or F1 to put it back)
  - At the start, a goal, match point and the end she speaks in a bubble just right of her head. With an LLM, she says lines of her own asked for all at once as the window opens, with no wait. Her face changes at each goal, and at the end she claps or goes "oops"
  - The room is a playroom with a plank floor and a rug, a wainscot and wallpaper, windows and framed pictures
  - See `docs/AirHockey.md` (in Japanese)
- **She talks at mahjong**: at the game's start, a riichi, a win, a draw and the game's end she says something in a bubble just right of her head, looking at you (in her own words with an LLM, otherwise a line of her own). Her words for a pon and as she stops to think are asked for ahead, all at once
- **Those watching comment**: the characters watching mahjong or air hockey now and then cheer or comment in their own bubbles
- **A word afterwards**: back on the desktop from mahjong or air hockey, she says what she thought of the game (stopped midway too)
- **Nothing through the table**: hair and skirt fall on the table, and her arms and her lean stop short of it

### Changed

- The mahjong camera no longer moves with the left button; drag with the right button to turn it (the middle button or Shift and the right button to move it, a right double-click or F1 to put it back)
- At mahjong and air hockey, her words come out a character at a time, as on the desktop

## 1.4.0

### New

- **Mahjong**: "Play mahjong" in the right-click menu opens a window where you play two-player mahjong against that character (an East and a South round, four hands, no chi, 24 draws each). She plays her own hand from what she can see (riichi, pon on value tiles, and folding against your riichi), and yaku, fu and points are counted properly
  - She comes from the desktop and sits at a 3D table in a Japanese-style room, where both hands, both rivers and the melds are set out. Each hand begins with the tiles dropping into the hands one by one as they are dealt. Her discards vanish from her hand and appear softly in her river. She ponders, smiles at her wins and goes "oops" at yours
  - Keys: click a tile (or ←→ and Enter) to discard; tsumo, riichi, kan, ron and pon are the buttons above your hand on the right (or their number keys). Space, or a click anywhere off the buttons, lets a tile go, except one you could ron on. After riichi, your draws are discarded for you
  - While you choose the tile for riichi, the waits it leaves, what each would pay and how many are left are shown for the tile you point at; before riichi the tsumo button says what the tsumo pays
  - Tiles set down, drawn and called, the riichi stick and the deal have their sounds
  - Drag over the table to turn the camera as in MMD (middle button or Shift-drag to move it, the wheel to come nearer, double-click or F1 to put it back)
  - The room has a hanging scroll, washi wallpaper, shoji on either side and a lamp. The floor is set to her legs, so her feet never go through it
  - See `docs/Mahjong.md` (in Japanese)
- **Watching**: the other characters on the desktop step aside from the mahjong window and watch it, now and then thinking or nodding
- **Closeness**: +3 for a mahjong game played to its end (at most 9 a day)

### Fixed

- While a full-screen app hid the characters, a speech bubble stayed on top of it. The bubble is hidden too
- A character walking when drawing stalled for a moment could pass where she was going and walk on off the screen. She now stops once past it

## 1.3.0

### New

- **Dungeon**: "Go to the dungeon" in the right-click menu opens a window where you and that character explore a randomly made dungeon, turn by turn. Meanwhile she leaves the desktop and walks the dungeon in 3D, fights with a sword, casts magic, and talks about what happens in a speech bubble (in her own words with the LLM)
  - Keys: arrow keys or WASD to move, moving into a monster attacks with the sword, F for magic, Space to wait or look for traps, number keys to use or equip, Shift with a direction to jump ahead, step back or step aside (nothing can hurt her in the air), M to switch between the 2D map and 3D, V to switch cameras, P for a picture, Z for the records
  - There are chests, traps, shops (Enter buys, Shift with a number sells), locked vaults, monster houses, dark rooms, campfires to rest at (press C beside one to talk with her), crystal rooms and flooded floors, and a boss every five floors. Each kind of monster has its own shape and ways, and the dragon breathes fire. The walls and floors change as you go deeper, and stairs once found are marked on the small map
  - Skills: skill points add up across adventures, and at each new skill level she learns one of three random skills she does not know yet (vacuum blade, heal, sleep, haste, teleport and more, seventeen in all). Learned skills are ready on F1-F12, and the effects that make her stronger or monsters weaker wear off after some turns
  - The deepest floor, the records (monsters defeated, things and treasures found) and titles are kept for each character. Press T at the very start to play today's dungeon, the same for the whole day
  - The memory for drawing it in 3D is taken only while the dungeon is open; "Draw the 3D view light" under Advanced in the settings makes it lighter still
  - See `docs/Dungeon.md` (in Japanese)
- **Look at a treasure**: with "Look at a treasure" in the right-click menu, she turns aside, holds a treasure brought home from the dungeon on her palm and says when and where you found it (the others lean in to see). See `docs/Treasures.md` (in Japanese)

### Changed

- "Every minute" for how often the characters chat with each other. With it, a chat longer than a minute is followed by the next as soon as it ends

## 1.2.9

### New

- **Closeness**: each character counts how close you two have become. It grows while she is on the desktop as you use the computer, when you talk to her, click her, finish a focus mode round or check off a to-do (at most 30 a day; it never goes down), from "just met" to "partners" in about a month. The LLM is told how close you are, so she gradually talks more openly, keeping to her persona (never colder than it). The number keeps growing after that, and she brings up a stage reached, 7, 30 and 100 days together (then every 100), every year since you met, and closeness passing 1,000 to 10,000 (then every 10,000). Ask "how close are we?" and she tells you. It grows without the LLM too, and she then says the milestones in set words. "Closeness" under Talking and voice turns it off. See `docs/Bond.md` (in Japanese)
  - Characters you already had start as if together since their oldest memory (or, with none, since the settings ini was made), "friends" at least, so updating never sends them back to "just met"
- **Album**: pictures of the characters with their speech bubbles. One is taken when she brings up a day or a stage reached together, and you can take one any time with "Take a picture" in the menu or a shortcut that works anywhere (Ctrl+Alt+Shift+S by default), with a shutter sound you can turn off. **It does not capture the screen: it uses the characters and bubbles as the app drew them, so windows behind them and the desktop are never in it.** Each is saved with and without the bubbles, with the words in `album.txt`. The key, the shutter sound and the folder (changing it copies the pictures so far over, and leaves the old folder as it was) are under Album on the Assistant page. "Open album" in the menu opens the folder. See `docs/Album.md` (in Japanese)

### Changes

- The persona window shows how much of the 4,000 characters is written (a line break counts as one). OK with more than that says so, with why a shorter persona works better, and keeps the window open. Before, typing stopped at 4,000 and a line break counted as two

### Fixes

- Turning on anti-aliasing could make the model vanish. MSAA is now checked against the depth format too, and when it cannot be set up the app draws without it (and logs why)
- Old Intel graphics drivers (older than WDDM 2.7) get no anti-aliasing, and a character suggests updating the driver (once per driver)

## 1.2.8

### New

- **Speak with AivisSpeech**: choose "AivisSpeech" in the box below the voice under Talking and voice in the settings, and replies are spoken with [AivisSpeech](https://aivis-project.com/), started beforehand (at `http://127.0.0.1:10101`). "Style strength" (0.5 to 2.0, standard 1.0) sets how clearly styles such as sweet or sleepy come through. AivisSpeech's voices set their own pace, so the lip-sync follows the sound
- **Reset pose and physics**: "Reset pose and physics" in the right-click menu smoothly stops the motion or gesture playing from where it is, eases hair and skirt back to their shape, and then resumes physics (1.5 s). From the tray icon, with more than one character, it is "Reset everyone's pose and physics"

### Changes

- Chats between characters have 45 topics instead of 10 (including one about today's date) and a way to start (a question, a quiz, teasing, musing aloud, ...). Two characters who talked before don't repeat their last chats
- After the app updates itself, one of the characters says the update is done and which version this is
- A gesture cut short lowers its arms smoothly instead of dropping them

### Fixes

- The documentation said AivisSpeech could be used, but its address could not be chosen in the settings and its voice and lip-sync went out of step, so it did not work. It does now (above)
- With a voice, each phrase lasts as long as its sound, so phrases no longer overlap

## 1.2.7

### New

- **Choose the GPU to draw on**: "GPU for drawing" under Advanced in the settings. Choose "Auto" or one of this PC's GPUs by name (the one in use is marked). On a laptop with an integrated and a discrete GPU, if something looks wrong on one, try the other

### Changes

- "Auto" uses the GPU Windows picks for this app (changed in Settings > System > Display > Graphics). Until now, the power-saving GPU was always used
- The log lists every GPU found and the one chosen in the settings

## 1.2.6

### New

- **Motions on top of a window**: when a character gets on a window's top edge, she can rest or move along it with VMD motions, besides the sitting and hanging the app makes. Choose your own VMDs with "Motion" > "Open window motions..." in the right-click menu (one or more, kept per character), and choose which to use with "Gestures..." in the settings. How to make them, and how to offer them for everyone, is in the bundled `docs/WindowMotions.en.md`

### Changes

- The settings window is split into tabs (Motion and display, Talking and voice, Assistant, Advanced). It is less than half as tall, so it fits on smaller screens

## 1.2.5

### New

- **Control from other apps (HTTP API)**: turn on "Remote API" under Advanced in the settings (off by default; port 50080), and other apps and scripts on this computer can have the characters talk (`/say`), be talked to (`/talk`), make gestures (`/gesture`), play VMDs (`/motion`: a file's path or the VMD itself), walk (`/walk`), and tell their state (`/state`). It listens on 127.0.0.1 only and refuses requests from web pages
- **Lines with a sound**: hand over a sound made elsewhere (MP3, AAC, WAV, FLAC, ...) with the line, and she says it with that sound. The mouth follows how loud it is, so it fits in any language
- **News from the app, and handing over the conversation**: connected to `/events`, other apps hear when a character is talked to, finishes a line, is clicked, and so on. Connected with `/events?chat=external`, what you say goes to that app instead of the LLM, and it decides the answer (when it disconnects, the LLM answers again)

### Changes

- Ten built-in idle motions: a dogeza, a double peace, a ballet twirl, "Let's go!", exercises, looking around, floating, knee bends, a spin on one leg and "Listen, listen!" (by 結廼 (Yuno)). Each eases in from the standing pose and back out to it, and the movement was smoothed

## 1.2.4

### Fixes

- Part of the speech bubble could be hidden behind the character's head; it now always shows in front of the characters

### Changes

- Up to 6 characters at once instead of 4 (the load grows about in step with their number)
- Four built-in idle motions: a yawn, "Hmm... oh!", "Excuse me" and leaning (by 絢姫 (AKI)). "Gestures..." in the settings lists them with their author and chooses whether to use them
- The log notes the drawing settings (antialiasing, motion blur) and whether the model is drawn 3 seconds after starting

## 1.2.3

### Fixes

- Some models loaded but did not show. A hidden accessory or stray vertices placed far away made the window larger than the screen, with the character pushed off it. The size and place are now measured on the vertices of drawn materials near the skeleton only
- 32-bit TGA and BMP textures whose alpha is 0 everywhere are taken as opaque (they look as in MMD, instead of the material vanishing)
- When the GPU stops responding and nothing can be drawn, the app says so and asks whether to restart

### Changes

- With several monitors, the app starts on the one with the mouse cursor
- Let go past the edge of the screen, a character comes running back onto it
- A log for looking into problems: `%LOCALAPPDATA%\MikuMikuDesktopPerch\log.txt` (the last run's in `log.old.txt`; two files at most, 512 KB each). When a model does not show, please attach it when telling about it

- When updating, the app restarts after the character has finished their line
- "Gestures..." under Motion in the settings: check only the idle gestures (and built-in idle motions) to use. All are used by default, including ones added later
- VMDs embedded in the exe can be played as one of the idle gestures (the means to add built-in idle motions; none are included yet)
- Twenty casual idle gestures: dangling the arms, swinging one arm, shaking the hands loose, rolling a wrist, moving the fingers, rolling the shoulders, a shrug, a sigh, looking back, looking down at the feet, looking up, a quick glance, stretching the neck, nodding, nodding off for a moment, swaying, twisting the waist, peering forward, bouncing at the knees and tapping the toes. The arms only move forward, back and out from the standing pose, so the hands do not sink into the body on any model
- A "Check now" button under Display in the settings checks for a new version at once and says so when this is the latest. The automatic check is now labeled "Check for updates"

## 1.2.2

### Fixes

- Outlines were drawn only around the model's silhouette; the ones inside it (where the face meets the neck, inside the ears, where arms and clothes overlap) were missing. Fixed

## 1.2.1

### What's new

- **Timers, reminders and to-dos**: ask a character things like "remind me in 5 minutes", "remind me at 3pm to call mom" or "add milk to my todo list". Works without an LLM; with one, the characters answer in their own words and understand other ways of asking. When it is time, a character tells you with a chime, and you can choose "Got it" or "Again in 5 min". "Timer" in the menu sets a timer at once, and "Reminders and to-dos..." shows, adds and deletes them. They survive exiting the app
- **Deadlines for to-dos**: "add the report to my todo list by friday" and the like. Listed soonest first; ones due tomorrow, today or already are told once a day
- **Google Calendar**: set the calendar's "Secret address in iCal format", and its events are told in the morning summary and when asked, and announced a little before they start. Read only. The address is kept encrypted and only sent to calendar.google.com
- **Morning summary**: when you start using the computer in the morning, a character tells you today's reminders and to-dos. Asking "what do I have today" does too
- **Focus mode (pomodoro)**: 25 minutes of work and 5 of break in turn (lengths in the settings), with a character telling you at each turn. The characters do not start talking during work
- **Break reminder**: after 60 minutes at the computer without a break (changeable in the settings), a character suggests a rest

### Motion improvements

- **Hands together**: in the clap the palms face each other and meet, neither staying apart nor going through each other, whatever the model's shoulder width, standing pose (T-stance or A-stance) or hand thickness. When the lowered hands are held together in front, the elbows and hands no longer sink into the body
- **Shoulders**: raising an arm in a gesture moves the shoulder too, by the same formula as MotionTraceEditor's automatic shoulder; gently in gestures that bring a hand up to the face (the wave, the peace sign and so on). Over a motion the user registered, the shoulder is left as the motion has it
- **Arms in the standing pose**: how far the arms hang is set from each model's build (a skirt, the shoulder width and so on), as low as they go without the hands sinking into the body
- **Each character on its own timing**: with several characters, gestures, blinks and weight shifts come at each one's own random times, so they no longer strike the same pose together
- **Thinking pose**: the lowered hand no longer sinks into the body and sits a little outward
- **Yawn**: the palm of the hand brought to the mouth faces the mouth. The mouth is found on each model from its あ morph

### Fixes

- The user's name and "Speak first" in the settings were not saved and came back empty the next time; fixed

### Settings file

- New keys: `MorningSummary`, `RestMinutes`, `FocusMinutes`, `FocusBreakMinutes`, `CalendarAddress` (stored encrypted) and `CalendarLeadMinutes`. Reminders and to-dos are kept in `%LOCALAPPDATA%\MikuMikuDesktopPerch\planner.txt`

## 1.2.0

### What's new

- **Multiple characters**: with "Add character..." in the menu, up to 4 characters can be on the desktop. The model, the motions (idle, now and then, click, walk and run), the position, and the voice are remembered for each character. The size is shared by all. The load grows with the number of characters, so when it is heavy, use fewer characters or lower the drawing settings
- **Characters chatting together**: with two or more characters, now and then two of them stand side by side, turn to face each other, and take turns talking while looking each other in the eyes (raising or lowering the neck and head when their heights differ). The interval can be chosen in the settings (10 minutes to 1.5 hours, or "Never")
- **Interval for speaking first**: the interval of "Remarks" can be chosen from 10 minutes to 2 hours
- **No overlapping**: characters avoid places where other characters are when walking, and step aside when standing on top of each other. Bubbles are also shifted sideways so they do not overlap other characters or bubbles
- **Checking for updates**: shortly after starting and once a day, the app checks the latest release on GitHub. When there is a new version, a character says so, and choosing "Yes (update)" does everything automatically: the download (with a SHA-256 check), replacing the files, and restarting. It can be turned off with "Check for new versions" in the settings (on by default)
- **English**: Japanese or English can be chosen under Display → Language in the settings (the default, "Auto", follows the Windows display language). Besides the menus, the settings window and the messages, talks with the LLM are in English too, and the mouth moves for English replies. The voice (VOICEVOX) supports Japanese only
- Each character's voice plays on its own output, so when two talk at once, one does not cut the other off
- **Relaxed hands**: in generated motions (the standing pose, walking, running and so on), the fingers curl a little instead of staying straight

### Documents

- Added the English README ([README.en.md](../README.en.md)) and English versions of the docs ([LocalLLM.en.md](LocalLLM.en.md), [MotionRules.en.md](MotionRules.en.md))

### Settings file

- New keys: `Language`, `ChatterMinutes`, `BanterMinutes`, `UpdateCheck`. The second to fourth characters go in `[Mascot2]`–`[Mascot4]`. Existing settings keep working as they are

## Contents of the release zip

Under `MikuMikuDesktopPerch\`:

- `MikuMikuDesktopPerch.exe`, `LICENSE`, `licenses\*`
- `README.md`, `README.en.md`
- `docs\LocalLLM.md`, `docs\LocalLLM.en.md`, `docs\MotionRules.md`, `docs\MotionRules.en.md`

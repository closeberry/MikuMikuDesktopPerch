# Release notes

[日本語](ReleaseNotes.md)

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

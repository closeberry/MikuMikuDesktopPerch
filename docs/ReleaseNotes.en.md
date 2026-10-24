# Release notes

[日本語](ReleaseNotes.md)

## Next version (unreleased)

### What's new

- **Timers, reminders and to-dos**: ask a character things like "remind me in 5 minutes", "remind me at 3pm to call mom" or "add milk to my todo list". Works without an LLM; with one, the characters answer in their own words and understand other ways of asking. When it is time, a character tells you with a chime, and you can choose "Got it" or "Again in 5 min". "Timer" in the menu sets a timer at once, and "Reminders and to-dos..." shows, adds and deletes them. They survive exiting the app
- **Morning summary**: when you start using the computer in the morning, a character tells you today's reminders and to-dos. Asking "what do I have today" does too
- **Focus mode (pomodoro)**: 25 minutes of work and 5 of break in turn (lengths in the settings), with a character telling you at each turn. The characters do not start talking during work
- **Break reminder**: after 60 minutes at the computer without a break (changeable in the settings), a character suggests a rest

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

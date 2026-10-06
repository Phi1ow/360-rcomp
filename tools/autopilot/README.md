# Autopilot scripts for unattended console runs

An exploratory title (PS5) reads `/app0/autopilot.txt`, which is `/data/homebrew/<title id>/autopilot.txt` on the console, and ORs the buttons it lists into the
pad state while the title runs (`platform/ps5/input_ps5.cpp`, `autopilot_buttons`). No file, no effect. With a script present the title sees a connected pad
even when no controller is awake, so a run needs nobody at the console. Statuses of what a script reaches are observed on captured frames, never assumed.

Format: one step per line, `<start_ms> <buttons_hex> <duration_ms>`.
Times count from the title's first pad read, which is within a second of the guest's own start. Steps may overlap; their buttons are ORed.
The mask is the XINPUT one the guest sees: D-pad up `0001`, down `0002`, left `0004`, right `0008`, START `0010`, BACK `0020`, left stick `0040`, right stick `0080`,
LB `0100`, RB `0200`, A `1000`, B `2000`, X `4000`, Y `8000`. Buttons only: no stick or trigger axis.
The title logs `RCOMP-PAD autopilot buttons=0x... at N ms` the first time each distinct mask is pressed (later presses of the same mask are silent).

| Script | Game | What it does |
| --- | --- | --- |
| `gta4_new_game.txt` | Grand Theft Auto IV | B every 2 s from 15 s to 43 s skips the intro screens; the game then starts a new game by itself |
| `tbogt_to_ingame.txt` | Episodes from Liberty City, The Ballad of Gay Tony | B every 2 s from 15 s to 25 s (intro movies); D-pad right at 30 s highlights *The Ballad of Gay Tony* on the episode screen; A at 34 s starts it; A every 6 s from 40 s to 130 s skips the opening cutscenes. Luis is standing in the street next to Tony's car ("Get in Tony's car") from about 70 s |
| `tbogt_intro_cutscenes.txt` | the same | the same without the A presses after 34 s: the opening cutscenes play out (several minutes) |

The episode screen has *The Lost and Damned* selected by default; A alone (without the D-pad right) starts that episode, which is **NOT TESTED** (the first attempt stopped at a missing import that is implemented since).

Deploy and check:

```bash
python3 tools/autopilot/upload_autopilot.py tools/autopilot/tbogt_to_ingame.txt      # FTP upload + readback (RCOMP_PS5_HOST, RCOMP_PS5_FTP_PORT)
python3 tools/autopilot/upload_autopilot.py --remove                                  # no script = no autopilot
python3 tools/fetch_all_frames.py <out dir>                                           # the numbered captures of the last run, as PNG and a contact sheet
```

Timing is the weak point of a script that has no feedback: the intro lengths, the loading time and the cutscene skipping are fixed by the game, not by the title.
After a change of the build (a faster loader, another audio path) look at the captures of one run before trusting a script for a measurement.

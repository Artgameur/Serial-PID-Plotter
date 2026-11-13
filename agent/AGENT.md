# serial_dashboard_cli

Command line version of Serial Dashboard, for driving a real device over a serial port. It is used here for PID tests of a fridge: it sends commands, reads the replies and data, records tests with stop conditions, runs chains of tests, and saves CSV files and report pictures.

It runs the app's own engine with the user's saved app settings, so it behaves like the app: variable names and units, the line format, stop conditions, chains with Prepare / Start when, device settings in the legend, and the test bench. The tool never saves those settings, so the user's app setup cannot change.

Windows x64 only. One file, nothing to install.

## Rules

- **The port is exclusive.** While the Serial Dashboard app is connected to the device, the session cannot open the port, and the reverse is also true. Ask the user to disconnect or close the app first.
- **Commands act on real hardware.** `send`, `test-start`, `chain-run` and Prepare commands change the device, such as its setpoint or gains. Only send what the user asked for or approved.
- **Always `stop` the session when done.** It keeps the port open until then. It also stops by itself after 60 minutes without a command (`--idle`), unless a test is running.
- **Every command prints one JSON object.** Check `"ok"`. Exit codes: `0` ok, `1` refused by the session (see `error`), `2` bad arguments, `5` no session running.

## Quick start

```
serial_dashboard_cli ports                         # find the port, ex: COM5
serial_dashboard_cli start --connect COM5          # session in the background, with the user's settings
serial_dashboard_cli status                        # connection, variables and latest values, chain, tests
serial_dashboard_cli send "CONFIG" --until "Config;" --wait 2
serial_dashboard_cli send "SP:17" --test           # sends it and records a test from now
serial_dashboard_cli wait --timeout 300            # until the test ends
serial_dashboard_cli export --out C:\results\sp17.csv
serial_dashboard_cli picture --out C:\results\sp17.png --graph "Temp,Set Point" --constant "Set Point"
serial_dashboard_cli stop
```

## Session

| Command | What it does |
|---|---|
| `start [--connect PORT] [--baud N]` | Starts the session in the background and returns its `status` plus `pid` and `log`. Baud rate, data format and line ending default to the app's saved values. |
| `start ... --profile NAME` | Reads one of the app's saved profiles on top of the settings. |
| `start ... --settings FILE` / `--clean` | Reads another settings file, or none (app defaults; variables are then named `Var 1`, `Var 2`, ...). |
| `start ... --set KEY=VALUE` | Overrides a setting before anything starts (repeatable). Needed for settings without a widget, such as `connCount=2`. |
| `start ... --idle MINUTES` | Idle time before the session ends by itself; `0` = never. Default `60`. |
| `status` | Connections, variables with their latest value (`null` before any data), whether a test, chain or Prepare is under way (`busy`), the chain's steps, and the number of tests. |
| `stop` | Closes the ports and ends the session. |
| `connect PORT [--baud N]` / `disconnect` | Opens or closes the port during a session. |

`--conn N` picks the serial connection (1 to 4, default 1) on `connect`, `disconnect`, `send`, `read`, `test-start` and `chain-set`. Several connections need `--set connCount=N` at start.

## Sending and reading

| Command | What it does |
|---|---|
| `send TEXT` | Sends one line, with the app's line ending. Quote TEXT when it has spaces. |
| `send TEXT --wait S [--until TEXT2]` | Also returns the lines received for S seconds after sending, or until one contains TEXT2 (`"matched": true`). Use it to read a reply. |
| `send TEXT --test` | Like a command with the film button checked in the app: sends it and starts recording a test. |
| `read [--wait S] [--until TEXT] [--max N] [--conn N]` | Returns the lines received since the last `read`, the newest `N` of them (default 200). `lines_left_out` counts the older ones skipped. The data often comes at 30 to 60 lines per second, so prefer `status` for current values. |
| `messages [--all]` | The app's messages since the last call: test started or ended, chain finished, errors. |

Each line is `{"conn":1,"ms":...,"text":"..."}`, where `ms` is the time since the session started. The last 4096 lines are kept.

## Tests

A test records every variable from its start until its stop condition, then stops by itself. The stop condition comes from the settings (see below).

| Command | What it does |
|---|---|
| `test-start [TEXT]` | Sends TEXT if given, then records a test, as `send TEXT --test`. With no TEXT, records without sending anything. |
| `test-stop` | Stops the test being recorded. During a chain, this also stops the chain. |
| `wait [--timeout S]` | Returns when no test, chain, Prepare command or settings request is under way, with the messages and the test list. `"done": false` means the timeout passed first, so call it again. Default timeout 120 s. Keep it below your shell's own time limit. |
| `tests` | All tests: `end` (`recording`, `condition`, `plateau`, `not_stable`, `stopped`, `full`), `samples`, `duration_s`, `reached_s` (when the condition was met), `settings` (the device's reply), `commands`, `variables`. |
| `tests-clear` | Deletes all tests. 16 at most can be kept, so clear them between series. |
| `export --out FILE.csv [--test N]` | Writes test N (1 = first, default the last) as the app's CSV export. The folder must exist. |
| `picture --out FILE.png [--graph VARS]... [--constant VAR] [--format W:H] [--width IN] [--ppi N]` | Picture of the tests overlaid, as the Compare tab's report picture. With `--graph` (up to 4 graphs, 4 variables each, comma separated), it chooses what each graph shows. Otherwise it keeps the current choice. |

Variable names: the full name (`"Temp [°C]"`) or the name alone (`Temp`, any case).

## Chains

A chain runs several tests one after another, each started by its own commands.

```
serial_dashboard_cli chain-set "KP:5|SP:17" "KP:10|SP:17" "KP:20|SP:17"
serial_dashboard_cli chain-run
serial_dashboard_cli wait --timeout 600         # repeat while "done" is false
serial_dashboard_cli tests
```

- `chain-set STEP...`: one argument per test. A step's commands are separated by `|` and sent `chainDelay` ms apart. Up to 16 steps. This replaces the steps saved in the app's settings, for this session only.
- `chain-run` / `chain-stop`: start or stop the chain. When `chain-run` returns `"ok": false`, its `messages` say why, for example that the stop condition is not valid.
- Between tests: the `chainPrepare` commands are sent (before the first test too). Then, if `chainWait` is set, the session waits until that condition holds for `chainWaitHold` seconds.

## Settings

`get` lists every setting the app shows, with its current value and, for choices, its options. `get KEY` returns one. `set KEY VALUE` changes one now, as if typed in the app. For a choice, give the option's text (`set compareStop "Custom condition"`) or its number from 0. For a switch, give `on`/`off`. A key without a widget is only stored (`"live": false`). These are read when used, such as a connection's `baud` at `connect`. The most useful ones:

| Key | Meaning |
|---|---|
| `compareStop` | `Settled` (watched variable within the tolerance of the constant for N samples) or `Custom condition` |
| `compareWatch`, `compareConstant` | Watched variable, and the shared constant such as the setpoint (by name) |
| `compareSamples`, `compareTolerance`, `compareConversion` | N, tolerance in %, and % per unit (band = tolerance / conversion, in units) |
| `compareCondition` | Custom condition, for example `held(abs(Temp-17)<=0.1,250)` or `t>60`. Functions: `held, avg, lowest, highest, prev, slope, abs, min, max`. Handles: `ref, watch, band, N, t, n, start`. |
| `compareStableMax` | Max test time in seconds (0 = none); the test then ends as `not_stable` |
| `comparePlateau`, `comparePlateauTime`, `comparePlateauSmooth` | Auto stable: ends a test that stopped changing away from the goal |
| `compareSettingsCmd`, `compareSettingsReply` | Command asking the device for its settings at each test, and how its reply starts. The reply is stored with the test and shown in the legend. |
| `chainDelay`, `chainPrepare`, `chainWait`, `chainWaitHold` | Delay between commands (ms), Prepare commands, Start when condition, and how long it must hold (s) |
| `benchAuto`, `benchFolder` | Saves each test that ends as CSV + PNG in that folder |
| `benchFormat`, `benchInches`, `benchPpi` | Report picture size (16:8, 6.5 in, 300 ppi by default) |

## Pictures of saved CSV files (no session)

```
serial_dashboard_cli --list FILE.csv...
serial_dashboard_cli --out PICTURE.png --graph VARS [--constant VAR] [--format W:H] [--width IN] [--ppi N] [--label NAME] FILE.csv...
```

`--list` prints each file's variables, samples, duration and device settings. `--out` overlays up to 16 files exported by the app or this tool, with up to 4 `--graph` options. `--label NAME` names the next file in the legend. Exit codes here: `0` done, `2` bad arguments, `3` a file could not be read, `4` the picture could not be written.

## Troubleshooting

- **"Could not open COM5"**: the app (or another program) has the port. Ask the user to disconnect.
- **No variables in `status`**: no data line received yet. Check the connection, the baud rate (`--baud`), and that the device is sending.
- **A test never ends**: check the stop condition with `get compareStop`, `get compareCondition`, `get compareWatch`. A `Settled` band that is smaller than the noise never triggers. Use `test-stop`, or set `compareStableMax`.
- **The session's own log**: the `log` path returned by `start`, one JSON event per line.

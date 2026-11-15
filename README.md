# FH3 Data Out: telemetry for Forza Horizon 3

Version 1.0.0

Not affiliated with Playground / Turn 10 / Xbox / Microsoft.  Use at your own risk.

Forza Horizon 3 never got the **Data Out** telemetry option that Forza Horizon 4, 5 and 6 have.  
FH3 Data Out adds it from the outside: it reads your car's physics from the running game and sends the
same 324-byte UDP packet the later games send.  Motion rigs, bass shakers, wind sims and dashboards that
support Forza Horizon Data Out (SimHub, FlyPT Mover, DR Sim Manager, Sim Racing Studio, ...) then work
with FH3.

It also fixes **OpenXR-MotionCompensation** for FH3 in VR, which normally can't work inside FH3's sandbox
(see [VR motion compensation](#vr-motion-compensation-openxr-motioncompensation)).

## With GrayHorizon VR

FH3 Data Out comes with GrayHorizon's FH3 download (1.4.0 and later). On GrayHorizon Settings' **Data Out** tab,
tick **Start FH3 Data Out with the game**, set where it sends to (up to three destinations) and start FH3 from GrayHorizon Settings (its **Start Forza Horizon 3** button or the
**GrayHorizon FH3** desktop shortcut): FH3 Data Out then starts minimized with the game and closes when FH3 closes.
Its settings file (`FH3DataOut.ini`, see below) goes beside `FH3DataOut.exe`.

## Quick start

1. Download the zip from Releases and extract it to a folder of your own.
2. Start Forza Horizon 3.
3. Run `FH3DataOut.exe`.  By default it sends to `127.0.0.1:8888`, 60 packets per second.
4. In your telemetry app, pick any **Forza Horizon** title.

You can start FH3 Data Out before or after the game; it waits for FH3 and reconnects if you restart it.

The window shows a live panel while you drive: speed, gear, RPM, power, boost, fuel, pedals,
G-forces, per-wheel tire temperature / suspension / slip / rumble, and race position and lap times.

## Settings

Send to other ports, several apps at once, or another PC:

```
FH3DataOut.exe 127.0.0.1:8888 127.0.0.1:20777 192.168.1.50:8888 --rate 100
```

Or put the same settings in `FH3DataOut.ini` next to the exe (see `FH3DataOut.ini.example`):

```
dest=127.0.0.1:8888
dest=127.0.0.1:20777
rate=60
```

| Option | ini line | Default | |
|---|---|---|---|
| `host:port` (up to 8) | `dest=host:port` | `127.0.0.1:8888` | where packets go |
| `--rate N` | `rate=N` | `60` | packets per second (1 - 1000) |
| `--no-oxrmc` | `oxrmc=0` | on | turns off the VR motion compensation fixes |
| | `screen=0` | on | no live panel: the window shows two lines and is never redrawn |
| | `precise=0` | on | plain timing instead of the 1 ms timer (packets a little less evenly spaced) |

## What is sent

**Real values:** engine RPM (max, idle, current), acceleration, velocity, angular velocity, yaw / pitch /
roll, suspension travel (normalized and metres), tire slip ratio / angle / combined, wheel rotation speed,
puddles, surface rumble, tire temperatures, position, speed, power, torque, boost, fuel, throttle, brake,
clutch, handbrake, steering, gear, drivetrain and cylinder count.

**Races:** lap number, race position, best / last / current lap and race time.  These are 0 in free roam.

**Car id:** `CarOrdinal` is FH3's own car number.  It does not match FH4's numbers for the same car, so
apps that look cars up by id won't know FH3's cars.

**Always 0:** car class, PI, car group, distance, driving line and the smashable fields.

## VR motion compensation (OpenXR-MotionCompensation)

FH3 is a Microsoft Store (UWP) app and runs in a
sandbox, so OXRMC can't read its config, can't see your motion software's rig data and can't see your
keyboard.  It quietly runs with compensation off.

While FH3 Data Out runs, it fixes all three:

- **Config:** the first time, it copies your OXRMC settings into FH3's sandbox.  OXRMC has to have run in
  FH3 once before, so start FH3 in VR, then **restart FH3** after FH3 Data Out says it copied the config.
- **Rig data:** it passes your motion software's rig feed into FH3's sandbox, a thousand times a second.
  All three of OXRMC's virtual trackers work: **FlyPT** (FlyPT Mover, SimHub, DR Sim Manager), **Yaw VR**
  and **Sim Racing Studio**.
- **Hotkeys:** it watches for your OXRMC shortcuts (Ctrl+Ins, Ctrl+Del, Ctrl+Shift+HOME, the offset keys,
  ...) and passes them to OXRMC while FH3 is the focused window.  The fast modifier (Alt) can't be passed
  on, so offset keys move 1 cm per press (hold to repeat).

The panel shows `VR comp: mirroring the rig feed into FH3 (...), OXRMC hotkeys work in FH3` when it
works.  Start FH3 Data Out before OXRMC activates.

### Calibrate once, in FH3

The copied config does **not** use the reference pose you may have locked in other games: FH3 needs the
SteamVR runtime, and its playspace differs from other runtimes'.  So, once:

1. Sit in your neutral driving position, rig parked level, looking straight ahead.
2. Press **Ctrl+Del** (calibrate).
3. Press **Ctrl+Shift+HOME** (lock).

From then on OXRMC restores that calibration every time FH3 starts, no matter where your headset is
when it does.  This only changes FH3's own copy of the OXRMC settings; your other games are untouched.

If the view still moves with the rig, the center of rotation is off: adjust it with OXRMC's offset keys
(Ctrl+arrows forward/back/left/right, Ctrl+PgUp/PgDn up/down) and lock again.  Doing this while the motion
software holds the rig tilted makes the error easy to see.

## Supported game version

FH3 **1.0.125.x** (the last release).  Other builds are refused instead of sending bad data.

## Troubleshooting

- **"waiting for Forza Horizon 3..."** stays: FH3 isn't running, or it's a build other than 1.0.125.x.
- **Nothing arrives in my app:** check the app's port matches (8888 by default) and that it's set to
  Forza Horizon 4 or 5.  A firewall prompt the first time is normal for UDP to another PC.
- **Antivirus complains:** a small program that reads another program's memory sometimes gets flagged.
  Only download it from the Releases page.
- **VR comp: no rig feed yet:** your motion software isn't running or its output for OXRMC is off.

## Building from source

Visual Studio Build Tools 2019 or newer (x64 C++).  Run `build.bat`; it makes `FH3DataOut.exe`
(single file, static runtime, no dependencies).

## Credits

FH3 Data Out by GrayHorizon.  
OpenXR-MotionCompensation is by BuzzteeBear; FH3 Data Out only talks to it through its own published
interfaces.

License: PolyForm Noncommercial 1.0.0

## VR frame rate tip

If your frame rate drops while FH3 Data Out runs, look at what receives the data, not the tool itself (it uses about
a tenth of one CPU core). Dashboards your telemetry app draws on extra monitors redraw constantly - on USB display
adapters every frame is also copied back from the graphics card - and that alone can cost 10% of the VR frame rate.
Close them, or lower their frame rate, while you drive in VR.

## Version history

- **1.0.0** - first release.

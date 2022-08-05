<h1>conturn <img src="https://user-images.githubusercontent.com/16616463/182421854-486f911c-257c-403a-b9f1-423046c19243.png" width="24" height="23"></h1>

conturn provides alternative `+left/+right/+speed` binds for CS:GO with in-game `cl_yawspeed`, `cl_anglespeedkey` control.

These convars, which control the turning speed for the `+left/+right` commands, exist in previous games in the series (1.6, Source), but are inaccessible in CS:GO, forcing useless unchangable defaults. This program aims to fill this gap, as these commands are essential for movement game modes like surf.

conturn works by simulating mouse movement based on the game's console output, captured using `con_logfile`.

New convars/commands `+_left`, `+_right`, `+_speed`, `_cl_yawspeed`, `_cl_anglespeedkey` can be used in-game to turn and control the turning speed, and keys can be bound as usual through the console and .cfg files.

- [Installation](#installation)
- [Usage](#usage)
- [How it works](#how-it-works)
  - [conturn vs. turnbinds](#conturn-vs-turnbinds)
- [Anti-cheat software](#anti-cheat-software)
- [Building](#building)

## Installation

**1. [Download](https://github.com/t5mat/conturn/releases/latest/download/conturn.exe) and run `conturn.exe`**

Settings are stored in `<exe-name>.ini`.

The program will run as Administrator in order to be able to create symlinks.

**2. Select your `csgo.exe`**

On first run, conturn will ask for the location of your `csgo.exe` file. **The program is external to the game and does not patch it in any way**, the path is needed to know where to create 2 files - a log file (`csgo\conturn.log`) and a .cfg file (`csgo\cfg\conturn.cfg`). These are automatically deleted when you exit conturn.

**3. Attach to the game using `exec conturn`**

Once conturn is running (icon in the tray), you'll need to attach it to the game by running `exec conturn` in console. This will not make any permanent changes to your configuration.

You'll have to run `exec conturn` every time you open the game. You can add it to your `autoexec.cfg`, but that won't work for when you launch the game first. Another option is to rebind the console key as follows: ```bind ` "exec conturn; toggleconsole"```

## Usage

Once conturn is running and attached to the game, the following new convars/commands will be available in-game:

- `+_left` - bind this instead of `+left`
- `+_right` - bind this instead of `+right`
- `+_speed` - bind this instead of `+speed`
- `_cl_yawspeed` (default 90.0) - turning speed for `+_left/+_right`
- `_cl_anglespeedkey` (default 0.33) - factor by which `_cl_yawspeed` is scaled while in `+_speed`

### Changing convars

The values of `_cl_yawspeed`, `_cl_anglespeedkey`, `sensitivity`, `m_yaw` are used to calculate the speed by which the program moves the mouse cursor.

**conturn has to be notified when they change** - changing a convar will have no effect until conturn is notified of the new value. You do that by simply writing the variable name to print its value after each time you change it. For example:

```
toggle _cl_yawspeed 80 160 240; _cl_yawspeed
```

### Example config

<details>
<summary>surf.cfg</summary>

```
exec conturn

bind MOUSE1 +_left
bind MOUSE2 +_right
bind SHIFT +_speed

_cl_yawspeed 120; _cl_yawspeed
```

</details>

<details>
<summary>comp.cfg</summary>

```
conturn_off

bind MOUSE1 "+attack"
bind MOUSE2 "+attack2"
bind MOUSE5 "use weapon_flashbang"
```

</details>

<details>
<summary>surf_tronic_njv.cfg</summary>

```
exec surf

# Initial yawspeed
_cl_yawspeed 140; _cl_yawspeed

# Use MOUSE5 to change yawspeed
bind MOUSE5 "toggle _cl_yawspeed 70 140 210; _cl_yawspeed"

# Use SHIFT for fast spins
_cl_anglespeedkey 3.0; _cl_anglespeedkey
```

</details>

### Additional convars/commands

- `conturn_off` - turn off conturn
- `conturn_freq` (default 0.001) *(notify after changing)* - maximum frequency of simulated mouse moves, lower values decrease CPU usage in favor of turn smoothness
- `conturn_sleep` (default 0.0000005) *(notify after changing)* - main loop sleep duration, higher values decrease input polling rate and overall CPU usage
- *Console filtering* - conturn uses console filtering (`con_filter_enable`, `con_filter_text_out`) to filter the in-game console spam created by the convar prints. This prevents commands like `key_listboundkeys` from printing any output - you can use `con_filter_enable 0` to turn filtering off.

## How it works

*Game console output is read through a named pipe.* The command `con_logfile` can be used to write console output to a file. There are no checks on the path before opening it, so a symlink can be used instead of a file. conturn creates a symlink to a named pipe through which it receives the console output. Named pipes writes are fast, also due to the fact that there's no disk I/O (it's worth noting that the game flushes the file after every console message, which means using `con_logfile` with an actual file is not great for FPS).

`exec conturn` will:

- Create the relevant aliases and convars
- Connect the console output to the named pipe using `con_logfile`
- On first run, set `_cl_yawspeed`, `_cl_anglespeedkey` values from settings
- Notify conturn of the current in-game values of `sensitivity` and `m_yaw`

conturn uses convar prints (for example `"_cl_yawspeed" = "90.0"`, generated by running `_cl_yawspeed`) as "notifications" of convar changes, which signal when to start/stop turning or update the turning speed. This makes external input monitoring unnecessary (keyboard hook/raw input) - the game tells us when buttons are pressed.

conturn is active only when the foreground window belongs to the process connected to the pipe (the game), and the mouse cursor is hidden (do not want to move the cursor when the menu/console are open).

Relevant console commands:
- `setinfo` - create a new convar (`FCVAR_USERINFO`)
- `alias` - print all aliases
- `cvarlist <name>` - print convar info
- `key_listboundkeys` - list bound keys (turn off console filtering! `con_filter_enable 0`)

### conturn vs. turnbinds

conturn provides some benefits over [turnbinds](https://github.com/t5mat/turnbinds/):

- **It uses in-game configuration** - you can use aliases, binds, and manage your configuration in (per-map) .cfg files
- **It doesn't need to monitor input alongside the game** - conturn operates on the input captured by the game through the console output, which results in less CPU usage and better input consistency
- **No need to `Alt+Tab`** - there's no UI, everything happens in-game

## Anti-cheat software

The program does not patch or inject anything into the game.

Apart from simulating mouse input, it doesn't really do anything suspicious.

It would be fair to say it's as bannable as an AutoHotKey script.

Anti-cheat software can easily detect the simulation of mouse movement though, and either prevent it or prevent the program from running completely. This is expected, just don't actively try to use this program in unintended scenarios.

## Building

Run `./build` on a Linux machine with Docker installed.

# OutputSwitch

[![Latest release](https://img.shields.io/github/v/release/cgize/OutputSwitch?display_name=tag&label=release)](https://github.com/cgize/OutputSwitch/releases)
[![License](https://img.shields.io/github/license/cgize/OutputSwitch)](LICENSE)
[![Last commit](https://img.shields.io/github/last-commit/cgize/OutputSwitch)](https://github.com/cgize/OutputSwitch/commits/main)
[![Downloads](https://img.shields.io/github/downloads/cgize/OutputSwitch/total?label=downloads)](https://github.com/cgize/OutputSwitch/releases)

OutputSwitch is an ultra-lightweight Windows utility that lets you switch between your active audio output devices in seconds. It runs quietly in the system tray, so your speakers, headphones, monitor audio, or USB headset are always one keyboard shortcut away.

Whether you move between a desk setup and headphones throughout the day, or simply want a faster alternative to opening Windows sound settings, OutputSwitch keeps the task simple and out of the way.

## Features

- Switches to the next available audio output device with a global keyboard shortcut.
- Uses `Ctrl + Alt + S` by default.
- Supports speakers, headphones, monitors, USB audio devices, and other active Windows output devices.
- Changes the default device for media, system audio, and communications.
- Shows the selected device in the system tray and can display a notification after each switch.
- Lets you switch devices by double-clicking the tray icon as well.
- Includes a tray menu showing the active shortcut, a *Start with Windows* option, and commands to open or reload settings and exit the app.
- The shortcut can be launched at Windows logon without administrator privileges via the *Start with Windows* menu option (stored in `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`).
- Starts with no main window and stays out of your way.

## Getting Started

1. Download the latest `OutputSwitch.exe` from the [Releases](https://github.com/cgize/OutputSwitch/releases) page. (`OutputSwitch.ini` is optional.)
2. Run `OutputSwitch.exe`.
3. Press `Ctrl + Alt + S` to move to the next active audio output device.

The OutputSwitch icon appears in the Windows system tray. Hover over it to see the currently selected device, double-click it to switch outputs, or right-click it to access its menu.

## Customize the Shortcut

`OutputSwitch.ini` is **optional**: if it is absent, OutputSwitch starts silently with the default shortcut (`Ctrl + Alt + S`) and notifications enabled. Selecting **Open configuration** from the tray menu creates the file with the default values before opening it, so you can edit it and then choose **Reload configuration** from the same menu.

```ini
[Hotkey]
Modifiers=Ctrl+Alt
Key=S

[UI]
Notifications=1
```

You can combine `Ctrl`, `Alt`, `Shift`, and `Win` in `Modifiers`. The `Key` can be a letter, number, function key (`F1` to `F24`), or common navigation key such as `Space`, `Tab`, `Home`, `End`, `Insert`, `Delete`, `PageUp`, `PageDown`, or an arrow key.

Set `Notifications=0` if you prefer to switch devices without a notification. Set it back to `1` to enable notifications again.

## Start with Windows

Right-click the tray icon and toggle **Start with Windows**. The preference is written to the current user's registry (`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`), so no administrator privileges are required.

## Requirements

- Windows
- At least two active audio output devices to cycle between

## Build Size

Run `build.bat` to produce the smaller executable (about 38 KB). It requires
the x64 [Microsoft Visual C++ Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe)
to be installed on the target computer.

Run `build.bat standalone` to produce a self-contained executable (about 129
KB) that embeds the Microsoft C runtime.

## License

Licensed under the [PolyForm Noncommercial License 1.0.0](LICENSE). You may use, modify, and share OutputSwitch for noncommercial purposes. Commercial use is not permitted.

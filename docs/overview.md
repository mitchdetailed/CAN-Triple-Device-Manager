# CAN Triple Device Manager

CAN Triple Device Manager configures the CAN Triple gateway — an STM32G473-based device with three CAN buses — over the ST-Link virtual COM port. You build a configuration as a document on the PC, then program it into the device in one explicit step.

## How the program is organised

The application is **offline-first**: everything you edit lives in a configuration document (a [.ct3 file](files.md)), not in the device. Nothing reaches the hardware until you run **Online → Send Configuration** (F5), and the device's current configuration only enters the editor when you run **Online → Get Configuration**. This means you can prepare and validate a complete configuration at a desk with no hardware attached.

The menu bar follows the same split:

<table>
<tr><th>Menu</th><th>Contents</th></tr>
<tr><td><b>File</b></td><td>New, Open…, Save, Save As…, Secure Configuration Builder…,
Check Channels, Config Summary…, Reveal / Conceal Protected Comms, Recent
Files, Exit. See <a href="files.md">Configuration Files</a> and
<a href="validation-report.md">Validation &amp; the Config Summary</a>.</td></tr>
<tr><td><b>Connections</b></td><td>Communications… — the three CAN buses,
their messages (sections) and channels. See
<a href="communications.md">Communications</a>.</td></tr>
<tr><td><b>Calculations</b></td><td>Math Channels…, User Conditions…, Timers…,
Up / Down Counters…, Integrators…, Constants…, Tables…, Device Script… —
on-device calculations that read and write channels.</td></tr>
<tr><td><b>Online</b></td><td>Connect / Disconnect, Send / Get / Verify
Configuration, Send Secure Configuration…, Monitor Channels… (F3), CAN
Viewer… (F4), Reset Device, Device Status…, Set Access
Passwords…, Update Firmware…, Firmware License Manager…. See
<a href="online.md">Online: Send, Get &amp; Flash</a>.</td></tr>
<tr><td><b>Tools</b></td><td>Channel Editor…, Lua Console…, Connection
Settings… and Theme (the serial port and the look belong to the
application, not the document). See <a href="#themes">Themes</a>.</td></tr>
<tr><td><b>Help</b></td><td>Contents… (F1) — this help — and About….</td></tr>
</table>

The status bar shows the document's file path on the left (or "Unsaved configuration"), and on the right the protected-comms state (only when the document carries a Protected Comms password) and the link state — "Connected: COM5 @ 7372800" or "Not connected".

<a id="themes"></a>

## Themes

**Tools → Theme** sets the colours the whole program is drawn in:

<table>
<tr><th>Theme</th><th>Look</th></tr>
<tr><td>Windows Default</td><td>The program as it has always looked: Windows'
own controls, light or dark as Windows is set.</td></tr>
<tr><td>Light</td><td>Light grey and white with blue highlights, whatever
Windows is set to.</td></tr>
<tr><td>Dark</td><td>Graphite with blue highlights.</td></tr>
<tr><td>Midnight Blue</td><td>Deep navy with teal highlights.</td></tr>
<tr><td>Carbon Red</td><td>Near black with red highlights.</td></tr>
</table>

A theme applies at once, to every window that is open, and the program starts in it next time. It belongs to your Windows account, not to the configuration, so a configuration looks the same to everyone it is sent to. Errors, warnings and notes keep their meaning in every theme, in colours picked to be read on it. This help follows the theme too: on Dark, Midnight Blue and Carbon Red, and on Windows Default when Windows is dark, its pages are drawn in the theme's colours.

## Where to start

If this is your first session, read [Getting Started](getting-started.md): it walks from connecting the hardware to seeing live values.

## Contents

<table>
<tr><th>Topic</th><th>Pages</th></tr>
<tr><td>Basics</td><td>
<a href="getting-started.md">Getting Started</a> ·
<a href="files.md">Configuration Files (.ct3)</a> ·
<a href="troubleshooting.md">Troubleshooting</a></td></tr>
<tr><td>Communications</td><td>
<a href="communications.md">Messages &amp; Sections</a> ·
<a href="channels.md">Channels</a> ·
<a href="datatypes.md">Data Types &amp; Ranges</a> ·
<a href="dbc-import.md">DBC Import</a> ·
<a href="relays.md">Message Relays</a></td></tr>
<tr><td>Calculations</td><td>
<a href="math-channels.md">Math Channels</a> ·
<a href="conditions.md">User Conditions</a> ·
<a href="constants.md">Constants</a> ·
<a href="tables.md">Lookup Tables</a> ·
<a href="counters.md">Up/Down Counters</a> ·
<a href="timers.md">Timers</a> ·
<a href="integrators.md">Integrators</a> ·
<a href="device-scripts.md">Device Scripts</a></td></tr>
<tr><td>Timing</td><td>
<a href="engine.md">Order &amp; Timing of Operations</a> — what runs when,
in what order, and how often</td></tr>
<tr><td>Scripting</td><td>
<a href="device-scripts.md">Device Scripts</a> — code that runs on the unit ·
<a href="scripting.md">Lua Console</a> — scripting the document on your PC</td></tr>
<tr><td>Online</td><td>
<a href="online.md">Send, Get &amp; Flash</a> ·
<a href="monitor.md">Monitoring Live Values</a> ·
<a href="firmware-update.md">Updating Firmware</a> ·
<a href="licensing.md">Firmware Licensing &amp; Access Keys</a></td></tr>
<tr><td>Reports</td><td>
<a href="validation-report.md">Validation &amp; the Config Summary</a></td></tr>
<tr><td>Legal</td><td><a href="license.md">License (MIT)</a></td></tr>
</table>

> **Note:** This help is available at any time from **Help → Contents…** (F1). Use the Index tab to jump to a dialog or operation by name, and the Search tab for full-text search.

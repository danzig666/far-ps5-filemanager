PS5 File Manager for Far Manager 3
==================================

A Far Manager panel plugin for browsing and managing a PlayStation 5's
files through the ps5-web-file-manager payload
(https://github.com/owendswang/ps5-web-file-manager).

Requirements
------------
- Far Manager 3.0.6638 or newer, 64-bit (x64)
- ps5-web-file-manager v1.12 running on the console (port 8888 by default)
- The PC and the console on the same network

Installation
------------
1. Close Far Manager.
2. Copy the PS5FM folder from this zip into %FARHOME%\Plugins\
   so that you have %FARHOME%\Plugins\PS5FM\PS5FM.dll
3. Start Far Manager.

Getting started
---------------
- Alt+F1 / Alt+F2 (disk menu) > PS5. The first time, you are asked for the
  console's IP address.
- Or type in the command line:  ps5:192.168.1.50
                                ps5:192.168.1.50:8889/data/homebrew
- Enter on a console shows its volumes (internal, USB, M.2, extended, root,
  and SMB shares added in the web UI). Enter on a volume opens it.

Keys
----
Console list:
  Enter            connect
  Shift+F4 / F7    add a console
  F4 / Shift+F6    edit the console under the cursor
  F8               remove consoles from the list

Inside a console:
  F5 / F6          copy / move between the PC and the PS5
  F5 / F6          with PS5 panels on both sides (same console): copy or
                   move ON THE CONSOLE - the data never crosses the network
  Shift+F6         rename (type a path with "/" to move)
  F7               make folder ("a/b/c" creates nested folders)
  F8               delete
  F3 / F4          view / edit (F4 uploads when you save)
  Shift+F1         PS5 actions: install PKG, PKG information, launch ELF,
                   extract archive, chmod, free space, running task,
                   open web UI, stop the payload
  Enter on .pkg    PKG information with an Install button
  Ctrl+R           refresh

Notes
-----
- The payload runs one task at a time and refuses to list folders while a
  copy, upload or delete is running. The plugin waits on a progress dialog;
  Esc cancels. If something else (the web UI) is busy, you are offered to
  wait for it or cancel it.
- Downloads go to "<name>.ps5part" and are renamed when complete. If a file
  already exists and is smaller, "Resume" continues it.
- File names that are not UTF-8 are shown using the console's fallback code
  page (default 936/GBK, configurable per console) and still work for all
  operations.
- The payload has no authentication: anyone on your network can use it
  while it runs.

Settings
--------
F9 > Options > Plugins configuration > PS5 File Manager:
disk menu entry, command line prefix, confirmation for on-console
copy/move, and whether Enter on a .pkg file shows PKG information.

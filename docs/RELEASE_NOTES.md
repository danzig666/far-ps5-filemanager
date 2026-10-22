First release of the PS5 File Manager plugin for Far Manager 3.

**Requirements:** Far Manager 3.0.6638+ (x64) and the
[ps5-web-file-manager](https://github.com/owendswang/ps5-web-file-manager) payload v1.12 on the console.

**Install:** unzip, copy the `PS5FM` folder into `%FARHOME%\Plugins\`, restart Far. Open it from the disk menu (Alt+F1/Alt+F2 → PS5) or type `ps5:<console-ip>`.

### Features
- Console list with saved hosts; volumes view (internal, USB, M.2, extended, root, SMB shares)
- F5/F6 between PC and PS5 for files and folder trees, with progress, overwrite/skip/resume prompts and Esc to cancel
- On-console copy and move when both panels are on the same PS5
- F7 mkdir, F8 delete, Shift+F6 rename/move, F3 view, F4 edit with upload on save (warns if the file changed on the console)
- Shift+F1 actions: install PKG, PKG information, launch ELF, extract archive, chmod, free space, running task, open web UI, stop the payload
- Correct handling of non-UTF-8 file names via the payload's byte encoding

### Known limitations
- One operation at a time (a payload limitation).
- Uploads cannot resume; downloads can.
- Copying between two different consoles goes through a temporary folder on the PC.
- SMB passwords are asked once per Far session and not stored.

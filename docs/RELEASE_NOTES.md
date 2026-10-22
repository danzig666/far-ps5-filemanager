### 1.0.2

- Clearer, safer keys in progress windows:
  - Tasks running on the console (extract, copy/move on the console, delete, chmod, PKG install): **Esc only closes the window and the task keeps running**. **F8** stops the task, after a confirmation whose default button is "Keep running". After closing, the window does not pop up again on every panel refresh; Shift+F1 → Running task reopens it.
  - Transfers between the PC and the PS5: Esc or F8 asks "Stop the transfer?" with **Continue** as the default button.
  - The bottom line of each progress window says exactly what the keys do.

### 1.0.1

- When the console is busy with a task started elsewhere (for example a long extract from the web UI), the panel now shows it live: archive/source, the file being processed, bytes done of total, item count, speed, elapsed and remaining time. It refreshes every half second and the panel opens on its own when the task ends. 
- The progress window for the plugin's own console operations shows the same details.

**Install:** unzip, copy the `PS5FM` folder into `%FARHOME%\Plugins\` (replace the old one), restart Far.

Requirements: Far Manager 3.0.6638+ (x64) and the [ps5-web-file-manager](https://github.com/owendswang/ps5-web-file-manager) payload v1.12.

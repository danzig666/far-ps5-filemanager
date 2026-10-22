### 1.0.1

- When the console is busy with a task started elsewhere (for example a long extract from the web UI), the panel now shows it live: archive/source, the file being processed, bytes done of total, item count, speed, elapsed and remaining time. It refreshes every half second and the panel opens on its own when the task ends. Esc offers Keep waiting / Cancel the task / Stop watching.
- The progress window for the plugin's own console operations shows the same details.

**Install:** unzip, copy the `PS5FM` folder into `%FARHOME%\Plugins\` (replace the old one), restart Far.

Requirements: Far Manager 3.0.6638+ (x64) and the [ps5-web-file-manager](https://github.com/owendswang/ps5-web-file-manager) payload v1.12.

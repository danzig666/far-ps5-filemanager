#pragma once

// Self-update from the plugin's GitHub releases. Shared between the
// far-ps5-filemanager and far-github-panel plugins; the per-plugin details
// live in UpdaterConfig.hpp.
//
// How an update is applied while Far is running:
//  - Windows cannot overwrite or delete a loaded DLL, but it can rename it.
//    The running DLL is renamed to "<name>.dll.<old version>-<time>.old" and
//    the new DLL is moved into its place. New Far instances load the new
//    version; running ones keep the old file until they restart.
//  - "*.old" files are deleted later (at Far start and after an update), but
//    only when no running far.exe still has that file loaded. If a Far process
//    cannot be inspected (for example one running as administrator), nothing
//    is deleted that round.

namespace updater {

// Shift+F1 > Check for updates.
void CheckForUpdates();

// Deletes "<name>.dll.*.old" next to the plugin DLL that no Far uses any more.
// Quiet and cheap when there is nothing to delete; safe to call at startup.
void CleanupOldFiles();

}  // namespace updater

# far-ps5-filemanager: design

A Far Manager 3 panel plugin that browses and manages a PlayStation 5's file
system through the HTTP API of the
[ps5-web-file-manager](https://github.com/owendswang/ps5-web-file-manager)
payload. It is meant to feel like Far's built-in Network plugin or NetBox: open
it from the drive menu (Alt+F1/F2), pick a console, and then use the normal
panel keys.

| | |
|---|---|
| Host | Far Manager 3, build **3.0.6638 or newer**, **Windows x64** only |
| Language | C++20, Far 3 SDK (`plugin.hpp`), WinHTTP, no other runtime dependencies |
| Toolchain | CMake + MSVC (built on GitHub Actions `windows-latest`), static CRT (`/MT`), one `PS5FM.dll` |
| Payload | ps5-web-file-manager **v1.12** (commit `246eb6f`), port 8888 by default |

Everything about the payload in this document comes from reading its source at
that commit (`src/filemgr.c`, `list.c`, `upload.c`, `download.c`,
`file_response.c`, `text.c`, `space.c`, `path_util.c`, `json_util.c` and
`assets/main.js`). The payload's API is not documented or versioned, so these
notes are the contract this plugin implements.

---

## 1. Goals and non-goals

**MVP goals**

1. Browse the console: a root view of storage volumes, then directories, with
   size, mtime and mode columns.
2. F5 to and from the PC, for files and whole directory trees, with a Far-style
   progress dialog, overwrite prompts, and Esc to cancel.
3. F8 delete, F7 mkdir, Shift+F6 rename.
4. **On-console copy and move (F5/F6 between two PS5 panels)**: the main reason
   to write this plugin instead of using FTP, since the data never crosses the
   network.
5. F3 view and F4 edit through a temp file, with upload on save.
6. An actions menu (F11 → PS5, or Shift+F1) for install PKG, launch ELF, extract
   archive, chmod and free space.

**Non-goals for v1**

- SMB shares mounted by the payload (`smb://` paths): they need a per-session
  `/api/smb/connect` with a password. Planned for phase 3 (section 10).
- Running several operations at once. The payload allows one task at a time
  (section 4), so neither does the plugin.
- Supporting payload versions other than the pinned one beyond a version check
  and a warning.
- x86 or ARM64 builds, and Far 2.

---

## 2. Architecture

```
            Far Manager 3 (x64)
                   │  exported *W functions (plugin.hpp)
┌──────────────────▼───────────────────────────────────────────┐
│ Plugin.cpp       GetGlobalInfoW / SetStartupInfoW / OpenW    │
│                  routes every call to a Panel instance       │
├──────────────────────────────────────────────────────────────┤
│ Panel            one per open panel: host, current dir,      │
│                  cached listing, F-key handling              │
│   ├─ Ops         Download / Upload / Delete / Mkdir /        │
│   │              Rename / RemoteCopy / RemoteMove / Extras   │
│   └─ Progress    Far dialog fed by local byte counts or      │
│                  by polling /api/tasks                       │
├──────────────────────────────────────────────────────────────┤
│ Ps5Client        typed API: List(), OpenRead(), Upload(),    │
│                  StartCopy(), Tasks(), Cancel() ...          │
│   ├─ PathCodec   UTF-16 <-> payload "byte-token" strings     │
│   ├─ Json        small reader (vendored, header-only)        │
│   └─ Http        WinHTTP session per host, sync requests,    │
│                  streamed bodies, timeouts                   │
├──────────────────────────────────────────────────────────────┤
│ Config           host list (name, address, port) in Far's    │
│                  settings storage (PSL_ROAMING)              │
│ (UI strings are English literals; no .lng files in v1)      │
└──────────────────────────────────────────────────────────────┘
```

Source layout:

```
src/core/           platform-neutral, built and tested on Linux too
  Json.*            small JSON DOM parser
  Text.*            UTF-8/UTF-16, percent-encoding, formatting
  PathCodec.*       payload byte-token <-> raw bytes <-> display names
  Http.hpp          transport interface
  Ps5Client.*       typed payload API
  Operations.*      task polling, remote tree walk, error texts
src/far/            Windows-only Far plugin
  Plugin.cpp        exported *W functions
  Panel.*           panel state, navigation and every operation
  Progress.*        progress window
  FarApi.*          message/menu/dialog helpers
  Config.*          hosts and options in Far's settings
  WinHttpTransport.*
  PS5FM.def/.rc     exports and version resource
far-sdk/            plugin.hpp + farcolor.hpp from FarGroup/FarManager (ci/v3.0.6638.4754)
tests/              unit tests, integration tests, POSIX HTTP transport
CMakeLists.txt
```

### Threading

Far plugin entry points run on Far's main thread and block it. The plugin
does network I/O synchronously in those calls, as NetBox and the Network plugin
do, but every long operation goes through the progress dialog loop:

- Transfers read and write in 1 MiB chunks. Between chunks the plugin redraws
  progress at most every 100 ms and checks for Esc with
  `CheckForEsc`-style polling (`ReadConsoleInput` peek, as Far's own copy does).
- WinHTTP timeouts are set explicitly (resolve 5 s, connect 5 s, send and
  receive 30 s per chunk) so a console that drops off Wi-Fi fails the operation
  instead of hanging Far.
- Server-side tasks (copy, move, delete, extract) are polled every 500 ms from
  the same loop.

A background thread is not worth it for v1: Far's panel API is not
thread-safe, and the payload's one-task limit would serialize the work anyway.

---

## 3. Payload API contract (v1.12)

### 3.1 General shape

- Plain HTTP, no authentication, no TLS. Default port 8888. If 8888 is busy the
  payload picks the next free port (8889 and up), so the port is configurable
  per host.
- The web UI sends nearly everything as `POST`. Parameters go either in the
  query string or in an `application/x-www-form-urlencoded` body, depending on
  the endpoint (see the table). Some endpoints require a specific method and
  return `405` otherwise.
- JSON responses always carry `"ok"`. Errors look like:
  ```json
  {"ok":false,"error":"another task is running","error_code":"active_task","error_arg":"..."}
  ```
  Branch on `error_code`, not `error`, and use the HTTP status as a fallback.
- Multi-path parameters (`paths`) are newline-separated in one form field.

### 3.2 Endpoints the plugin uses

| Purpose | Request | Parameters | Result |
|---|---|---|---|
| Volumes | `GET /api/space` | `path` (optional) | `spaces[]`: `label_key`, `path`, `free`, `total`, `current` |
| List dir | `POST /api/list` | query `path` | `path`, `parent`, `entries[]`: `name`, `path`, `type` (`d l - c b p s`), `mode` (octal perms as int), `size`, `mtime` (Unix s) |
| Read file | `GET /fs` | query `path`; header `Range` supported | raw bytes; `206` + `Content-Range` for ranges |
| Upload: begin | `POST /api/upload/prepare` | form `path` (dest dir), `src`, `total`, `count`, `rels`, `sizes`, `overwrite` | `task_id` |
| Upload: file | `POST /api/upload-file` | raw body; headers `X-WFM-Task-ID`, `X-WFM-Path`, `X-WFM-Rel`, `X-WFM-Size`, `X-WFM-Overwrite` | `{"ok":true}` |
| Upload: end | `POST /api/upload/finish` | query `task_id` | `{"ok":true}` |
| Copy | `POST /api/copy` | form `paths`, `dst`, `overwrite=1` | `task_id` |
| Move | `POST /api/move` | form `paths`, `dst`, `overwrite=1` | `task_id` |
| Delete | `POST /api/delete` | form `paths` | `task_id` |
| Mkdir | `POST /api/mkdir` | query `path` (parent), `name` | ok |
| Rename | `POST /api/rename` | query `path` (full source), `name` (new leaf) | ok |
| Task status | `POST /api/tasks` | none | `tasks[]` (section 4), `completion` |
| Cancel | `POST /api/cancel` | query `id` | ok, or `404 active_task_not_found` |
| Text read | `GET /api/text` | query `path` | UTF-8 body, header `X-Text-Version` |
| Text save | `POST /api/text/save` | query `path`, `version`; raw UTF-8 body | ok, or `409 text_file_changed` |
| chmod | `POST /api/chmod` | form `paths`, `mode`, `recursive` | `task_id` |
| Extract | `POST /api/extract` | form `paths`, `destination`, `separate`, `overwrite`, `password` | task ids |
| PKG info | `GET /api/pkg-info` | query `path` | metadata JSON |
| Install PKG | `POST /api/install-pkg` | form `paths` | `task_ids[]` (cannot be cancelled) |
| Launch ELF | `POST /api/launch-elf` | form `path` | ok |
| Stop payload | `POST /api/exit` | none | ok |

Endpoints deliberately not used:

- `/api/download/prepare` + `/api/download?id=`: these stream a single file or a
  `.tar` of several paths as one download task. `/fs` is better for Far: it is
  per file (so Far's per-file overwrite and skip logic applies), it supports
  `Range` for resuming, and it does not occupy the task slot. Its one drawback,
  no server-side progress, does not matter because the plugin counts bytes
  itself.
- `/api/text/create`: F4 on a new name is handled as an upload of an empty file.

### 3.3 Path encoding: the one thing that must be right

The payload treats paths as **raw bytes** (PS5 file names are not guaranteed
to be valid UTF-8) and smuggles them through JSON and URLs with its own scheme:

- **Server → client.** `json_escape` writes every byte `>= 0x80` as `\u00XX`.
  A UTF-8 name such as `日本` (`E6 97 A5 E6 9C AC`) therefore arrives as six
  code points `U+00E6 U+0097 ...`, not as two CJK characters.
- **Client → server.** `fs_path_value` UTF-8-decodes every incoming path. If
  every code point is `<= U+00FF` and at least one is `>= U+0080`, it collapses
  each code point back into one byte. Otherwise it uses the string unchanged.

The consequence: sending a name as ordinary UTF-8 is **wrong for Latin-1-range
characters**. `café` sent as UTF-8 decodes to `U+00E9`, which collapses to the
single byte `0xE9`, so the server looks for a file that does not exist. The
web UI gets away with it only because it always echoes back the byte-token
string it was given.

`PathCodec` implements both directions:

```
Decode(tokenString) -> {std::string rawBytes, std::wstring display}
  1. JSON-unescaped string -> code points.
  2. Each code point 0x80..0xFF -> one byte; ASCII -> itself.
     (A code point > 0xFF should not appear; keep it as UTF-8 if it does.)
  3. display = UTF-8 decode of rawBytes if valid, otherwise decode with the
     configured fallback code page (default 936/GBK, matching the web UI),
     otherwise map each byte to U+00XX.

Encode(rawBytes) -> tokenString      // used for every path sent as a param
  each byte >= 0x80 -> U+0080..U+00FF, then UTF-8-encode, then percent-encode.
```

Rules that follow from this:

1. **Keep the raw path from the listing.** Each `PluginPanelItem` stores the
   entry's raw `path` bytes in `UserData`. Operations on existing items send
   `Encode(raw)` and never round-trip through the display name, so names that
   are not UTF-8 still work.
2. **New names** (mkdir, rename, upload targets) are UTF-16 → UTF-8 → `Encode`.
3. **Exception: `X-WFM-Rel`** (upload relative path) does *not* go through
   `fs_path_value` on the server. Send it as plain percent-encoded UTF-8.
   `X-WFM-Path` (destination dir) *does*, so it uses `Encode`.
4. `/api/mkdir` and `/api/rename` trim leading and trailing whitespace from
   `name`. Reject such names on the client with a clear message instead of
   creating something the user did not type.
5. A display name that will not decode cleanly gets a marker (for example a
   trailing `~` in the Description column) so the user knows it is lossy.

### 3.4 Version check

The payload does not expose its version over HTTP: `VERSION_TAG` only goes to
stdout and the on-console notification. So the plugin checks capabilities
instead. Connecting calls `/api/space`, so an unreachable or incompatible
server fails at connect time. An unknown endpoint returns `404` with
`error_code: "unknown_api"`, which the plugin reports as "this payload version
does not support the operation". CI pins the integration tests to v1.12.

Worth upstreaming: a tiny `GET /api/version` returning
`{"ok":true,"version":"v1.12"}`.

---

## 4. The single-task model

The payload keeps a global task list and refuses to start a task while another
is active (`409`, `error_code: "active_task"`). It also refuses **reads**
during a task: `/api/list`, `/fs`, `/api/text`, `/api/rename` and `/api/mkdir`
all return `409 active_task`. Upload, copy, move, delete, chmod, extract and
PKG install are tasks. `/fs` reads are not tasks, so two plain reads can run in
parallel.

The plugin's rules:

1. **One operation at a time, per host.** A `HostState` shared by all panels
   for the same `address:port` holds a busy flag. A second panel on the same
   console waits on the first operation's progress dialog.
2. **Every server-side operation is modal:** start → progress dialog that polls
   `/api/tasks` → terminal state → panel refresh. The user cannot browse
   during an on-console copy anyway, because `/api/list` would return 409.
3. **Recovering from `active_task`** (left by the web UI, a crashed Far, or a
   PKG install): the plugin calls `/api/tasks` and offers
   "A task is running on the console: <op> <src> (NN%). [Wait] [Cancel task] [Close]".
   Wait attaches the normal progress dialog to that task id. PKG installs
   cannot be cancelled, so only Wait and Close are offered for them.

### Task polling

`/api/tasks` returns every task the server still holds:

```json
{"ok":true,"tasks":[{"id":7,"op":"copy","state":"running","src":"/data/a","dst":"/mnt/usb0/a",
  "current":"/data/a/big.bin","error":"","error_code":"","error_arg":"",
  "src_count":1,"completed_count":0,"total":4294967296,"done":1073741824,
  "speed":52428800,"eta":61,"cancel_requested":false,"created_at":...,"elapsed":20,
  "total_elapsed":21,"updated_at":...}],
 "now":...,"completion":{...}}
```

- `state` is one of `queued`, `running`, `done`, `failed`, `canceled`.
- The server removes a finished copy, move, delete, chmod or extract task once
  one `/api/tasks` response has reported it (upload results are kept for a
  short retention window), so the plugin must catch the terminal state from
  that single poll. The web UI polls the same endpoint, so if a browser is open
  on the console it can take the result first. If its task id disappears without one, the plugin treats the task as
  done and re-lists the directory to check.
- Progress dialog: title from `op`, current file from `current`, a bar from
  `done/total`, speed and ETA from the server's `speed`/`eta`.
- Esc → confirm → `/api/cancel?id=` → keep polling until `canceled`.

---

## 5. Far integration

### 5.1 Plugin identity

- `GetGlobalInfoW`: `MinFarVersion = MAKEFARVERSION(3,0,0,6638,VS_RELEASE)`,
  plugin GUID, title "PS5 File Manager", version from `version.hpp`.
- `GetPluginInfoW`: a disk menu item ("PS5"), a plugin menu item (F11),
  a config item, and command prefix `ps5:`.
- `OpenW` handles `OPEN_LEFTDISKMENU`/`OPEN_RIGHTDISKMENU`, `OPEN_PLUGINSMENU`,
  `OPEN_COMMANDLINE` (`ps5:192.168.1.50`, `ps5:192.168.1.50:8889/data/homebrew`)
  and `OPEN_SHORTCUT` (Ctrl+0..9 folder shortcuts restore host and path via
  `OpenPanelInfo.ShortcutData`).

### 5.2 Panel levels

```
Hosts            (saved consoles; like NetBox sessions)
 └─ host:port    Volumes view from /api/space: Internal (/data), USB0..7,
                 M.2 (/mnt/ext1), Extended (/mnt/ext0), plus "/" for the full tree
     └─ /path    normal directory listing
```

- **Hosts level**: Shift+F4 new host, F4 edit, F8 remove, Enter connect. Host
  fields: name, address, port (8888), fallback code page (936), "show `/` root"
  toggle. Stored with `FarSettings` (`PSL_ROAMING`).
- **Volumes level**: items are pseudo-directories. The Description column shows
  "free / total". Enter switches to the volume's path.
- **Directory level**: `..` from a volume root goes back to Volumes, unless the
  user browsed from `/`.

`GetOpenPanelInfoW`:

- `Flags = OPIF_ADDDOTS | OPIF_SHOWPRESERVECASE | OPIF_USEATTRHIGHLIGHTING`
- `CurDir` = display path; `HostFile` empty; `Format = L"ps5"`
- `PanelTitle = L" PS5 192.168.1.50:/mnt/usb0/games "`
- Mode 0 (Ctrl+0) shows a Mode column with the `mode` field as `rwxr-xr-x`.

### 5.3 Listing → `PluginPanelItem`

| JSON | PluginPanelItem |
|---|---|
| `name` (decoded) | `FileName` |
| `size` | `FileSize` (0 for dirs) |
| `mtime` | `LastWriteTime` (Unix → FILETIME) |
| `type == 'd'` | `FILE_ATTRIBUTE_DIRECTORY` |
| `type == 'l'` | `FILE_ATTRIBUTE_REPARSE_POINT` (the payload `lstat`s, so link targets are not followed; v1 treats links as files) |
| `mode` without write bits | `FILE_ATTRIBUTE_READONLY` |
| name starts with `.` | `FILE_ATTRIBUTE_HIDDEN` |
| raw `path` bytes | `UserData` (owned, freed in `FreeFindDataW`) |

The listing for the current directory is cached on the `Panel` and reused when
Far asks again in the same state (for example on a redraw after a dialog). It
is dropped on Ctrl+R and after every operation.

### 5.4 Key and operation mapping

| User action | Far callback | Implementation |
|---|---|---|
| Enter on dir / `..` | `SetDirectoryW` | set `curDir`; `/api/list` (validates) |
| F5 PS5 → local | `GetFilesW` (Move=0) | recursive download over `/fs` (5.5) |
| F6 PS5 → local | `GetFilesW` (Move=1) | download, then `/api/delete` of the sources |
| F5 local → PS5 | `PutFilesW` | upload protocol (5.6) |
| F6 local → PS5 | `PutFilesW` (Move=1) | upload; Far deletes the local sources on success |
| F5/F6 PS5 → PS5 | `ProcessPanelInputW` intercept | `/api/copy` / `/api/move` (5.7) |
| Shift+F6 | `ProcessPanelInputW` | rename dialog → `/api/rename` |
| F7 | `MakeDirectoryW` | `/api/mkdir` |
| F8 / Shift+Del | `DeleteFilesW` | confirm → `/api/delete` task |
| F3 | `GetFilesW` with `OPM_VIEW` | download to temp; Far opens the viewer |
| F4 | `GetFilesW` with `OPM_EDIT`, then `PutFilesW` on save | 5.8 |
| Ctrl+R | `UpdateW`-style refresh | drop cache, re-list |
| Shift+F1 / F11 → PS5 | `ProcessPanelInputW` / `OpenW` | actions menu (5.9) |
| Alt+F6, hard links | n/a | not supported (message) |

### 5.5 Download (GetFilesW)

1. Build the job list. Selected files go in directly. Selected directories are
   walked depth-first with `/api/list`, collecting files and empty
   directories. A pre-scan dialog shows "Scanning... N files, X GB" and can be
   cancelled with Esc.
2. Overwrite policy uses Far's standard dialog (Overwrite / Skip / Rename /
   Append is off / Resume / "Remember choice"), driven by the local file's
   existence and size. With `OPM_SILENT`, overwrite without asking.
3. For each file: `GET /fs?path=` → write to `<dest>.ps5part`, then rename to
   the final name when the byte count matches `size`. On **Resume**, send
   `Range: bytes=<localSize>-` and append, expecting a `206`.
4. Set local mtime from `mtime`.
5. A transient failure (connection reset, timeout) retries the file up to
   3 times with a `Range` resume from the bytes already written.
6. Progress: per file and total bars, speed, ETA, all counted locally.
7. For `OPM_VIEW`/`OPM_EDIT`/`OPM_QUICKVIEW` the destination is Far's temp dir
   and there is no confirmation.
8. On Move=1, after every file in a top-level item has transferred, one
   `/api/delete` task removes those items, with the normal task polling.

### 5.6 Upload (PutFilesW)

The payload's upload is a three-step task:

1. **Scan locally**: expand selected directories with `FindFirstFileW`, giving
   a list of (local path, relative path with `/` separators, size). Empty local
   directories are collected separately.
2. **Conflicts**: one `/api/list` of the destination gives the existing
   top-level names. Show Far's overwrite dialog per conflicting top-level item.
   The payload's `overwrite` flag applies to the whole upload task, so a mixed
   answer (overwrite A, skip B) drops B from the manifest and sends
   `overwrite=1` for the rest.
3. `POST /api/upload/prepare` with:
   - `path` = `Encode(destDirRaw)`
   - `src` = first relative path
   - `count`, `total`
   - `rels` = relative paths joined with `\n` (plain UTF-8, see 3.3 rule 3)
   - `sizes` = sizes joined with `\n`
   - `overwrite` = `0|1`

   The server checks free space against the manifest here and fails early
   with `error_code: "no_space"`, `error_arg: "<required>,<available>"`, which
   the plugin shows as a readable message.
4. For each file, `POST /api/upload-file` with the raw body streamed from disk
   in 1 MiB chunks through `WinHttpWriteData` and these headers:
   ```
   Content-Type: application/octet-stream
   Content-Length: <size>
   X-WFM-Task-ID: <task_id>
   X-WFM-Path: <percent-encoded Encode(destDirRaw)>
   X-WFM-Rel: <percent-encoded UTF-8 rel>
   X-WFM-Size: <size>
   X-WFM-Overwrite: 0|1
   ```
   The server writes to `<target>.wfm-upload-*.tmp`, checks the size, and
   renames to the target. It creates missing parent directories from `Rel`.
5. Empty directories: `/api/mkdir` for each one after `finish` (mkdir is
   refused while the upload task is active).
6. `POST /api/upload/finish?task_id=`.
7. **Cancel or error**: `/api/cancel?id=` then `/api/upload/finish?task_id=`,
   the same sequence the web UI uses, so the task does not stay active and
   block the console.
8. Uploads cannot be resumed: the server truncates its temp file on every
   request. A failed file is retried from the start, up to 3 times.

### 5.7 On-console copy and move

Far's default for F5 between two plugin panels is to download to a temp dir
and upload again, which defeats the point. Instead, `ProcessPanelInputW`
intercepts F5 and F6 (no modifiers) when **both** of these are true:

- the passive panel is a plugin panel owned by this plugin
  (`FCTL_GETPANELINFO` on `PANEL_PASSIVE` → `PluginHandle`, then
  `FCTL_GETPANELDIRECTORY` and our own instance table), and
- both panels point at the **same host:port**.

Flow:

1. Show a Far-style copy dialog: "Copy N items to: [/mnt/usb0/games/]" with an
   editable destination, prefilled with the passive panel's raw path.
2. Pre-flight conflict check with one `/api/list` of the destination. Ask
   Overwrite All / Skip conflicts / Cancel. The server's `overwrite=1` refuses
   a dir/file type mismatch ("remove conflicting file or directory first"),
   which is reported as an error.
3. `POST /api/copy` or `/api/move` with `paths` = `Encode` of each raw path
   joined with `\n`, `dst` = `Encode(destRaw)`, and `overwrite`. For one source
   the server treats `dst` as the target name if it is not an existing
   directory, which gives "copy as new name" for free.
4. Task polling dialog → refresh both panels.

Errors the server checks before starting, mapped to messages:
`source_destination_same`, `destination_inside_source`,
`destination_must_be_directory`, "destination exists" (only when
`overwrite=0`), `no_space`.

If the hosts differ (two consoles), the intercept does nothing and Far's
default temp-dir round trip applies. That works, just slowly, and is
acceptable for v1.

### 5.8 Editing (F4)

- Text files up to the payload's text limit could use `/api/text` and
  `/api/text/save?version=`, which gives optimistic concurrency through
  `X-Text-Version` but only accepts UTF-8 and certain extensions. It also
  **reformats** the text on save (`format_text_for_save`) to match the original
  line endings.
- **Decision:** use the generic path for every file: `GetFilesW(OPM_EDIT)` to
  temp, then Far calls `PutFilesW` when the editor saves, and that does a
  one-file upload with `overwrite=1`. This keeps bytes exact and works for any
  file type. Before the upload, the plugin compares the remote `size`/`mtime`
  from a fresh listing with the values when the file was opened. If they
  changed, it asks "File changed on the console since it was opened.
  Overwrite?"

### 5.9 Actions menu (Shift+F1, or F11 → PS5 on a PS5 panel)

| Item | Applies to | Endpoint | Notes |
|---|---|---|---|
| Install PKG | selected `*.pkg` | `/api/install-pkg` | Show `/api/pkg-info` (title, ID, size) first. Not cancellable. |
| PKG info | `*.pkg` under cursor | `/api/pkg-info` | Read-only dialog |
| Launch ELF | `*.elf`, `*.bin` under cursor | `/api/launch-elf` | Confirm first |
| Extract here / to… | archives | `/api/extract` | Destination prompt, "separate folders", overwrite, password field |
| chmod… | selection | `/api/chmod` | Octal or `rwx` checkboxes; recursive option |
| Free space | any | `/api/space` | Table of volumes |
| Open in browser | any | none | `ShellExecute http://host:port/` |
| Stop payload | any | `/api/exit` | Confirm; then close the panel |

Enter on a `.pkg` file opens the PKG info dialog with an Install button, the
way Enter on an archive opens it in other plugins. This can be turned off in
the config.

---

## 6. HTTP layer

- One `WinHttpOpen` session for the plugin, with
  `WINHTTP_ACCESS_TYPE_NO_PROXY` (the console is on the LAN; a system proxy
  would break it).
- One `WinHttpConnect` handle per host, reused. Keep-alive is on, but every
  request is self-contained, so a dropped connection costs only a retry.
- Requests:
  - `Request(method, path, query, formBody?) -> {status, headers, body}` for
    JSON calls (body limit 16 MiB; large directories make large listings).
  - `OpenRead(path, rangeStart?) -> stream` for `/fs`.
  - `UploadStream(headers, size, reader callback)` using
    `WinHttpSendRequest` with a total length, then `WinHttpWriteData` per chunk.
- Query strings and form bodies are built with a percent-encoder that encodes
  everything except unreserved characters (`A-Z a-z 0-9 - _ . ~`) and writes
  space as `%20`. The server decodes `+` as space, so a literal `+` must be
  sent as `%2B`.
- Non-2xx responses with a JSON body become an `ApiError{status, code, message, arg}`.
  A connection failure becomes `NetError{winhttpError}`, with a message like
  "Cannot reach 192.168.1.50:8888. Is the payload running?"

---

## 7. Error handling and messages

All errors surface through one `ShowError(op, item, error)` using
`Info.Message` with `FMSG_WARNING`, plus Retry / Skip / Skip all / Cancel
during multi-item operations.

`error_code` → message (`DescribeApiError` in `src/core/Operations.cpp`):

| code | message |
|---|---|
| `active_task` | A task is already running on the console (offer Wait / Cancel task) |
| `file_not_found` | File not found on the console |
| `no_space` | Not enough space: need X, Y free |
| `source_destination_same` | Source and destination are the same |
| `destination_inside_source` | Cannot copy a folder into itself |
| `destination_must_be_directory` | Destination must be a folder |
| `file_already_exists` | A file with that name already exists |
| `invalid_path` | Invalid path |
| `pkg_install_unsupported` | PKG install is only available on a real PS5 |
| `system_error` | `error` text from the server (usually `strerror`) |
| other | `error` text, plus the code in brackets |

---

## 8. Configuration

Far's F9 → Options → Plugins configuration → PS5 File Manager:

- Add to disk menu: on
- Command-line prefix: `ps5`
- Default port: 8888
- Fallback code page for non-UTF-8 names: 936
- Transfer chunk size: 1 MiB
- Request timeout: 30 s
- Confirm on-console copy/move: on
- Enter on `.pkg` opens PKG info: on

Per-host settings are under the Hosts level (5.2).

---

## 9. Build and test

### Build

- CMake (`cmake -S . -B build -A x64 && cmake --build build --config Release`),
  `/std:c++20`, `/MT`, `/W4`, linked against `winhttp.lib` only.
- Output `PS5FM.dll`. To install, copy it to `%FARHOME%\Plugins\PS5FM\`.
- The Far SDK headers are vendored from FarGroup/FarManager at a tag
  >= 3.0.6638 so the build is reproducible.
- GitHub Actions (`.github/workflows/build.yml`): unit tests on Linux,
  integration tests against the payload built for Linux, the MSVC x64 build
  with an exports and runtime-dependency check, and a zip artifact. Pushing a
  `v*` tag creates a GitHub release with the zip.

### Tests

1. **Unit tests** (a tiny built-in harness, no Far, no network): `PathCodec` round trips (ASCII,
   Latin-1 `café`, CJK, invalid UTF-8 bytes, GBK names), percent-encoding,
   JSON parsing of recorded `/api/list` and `/api/tasks` responses, Unix time →
   FILETIME, and mode → attribute mapping.
2. **Integration tests against the real payload on Linux.** The payload's
   Makefile has a Linux build target (`LINUX_CFLAGS`), so CI can build it in a
   Linux job, run it against a temp directory, and run a small Windows-side
   or cross-platform harness driving `Ps5Client` through list, upload, `/fs`
   with Range, copy, move, delete, rename, mkdir, cancel, and the `active_task`
   conflict. This runs `Ps5Client` without Far. To make that possible,
   `Ps5Client` and `PathCodec` have no Far dependencies, and `Http` sits behind
   an interface (a WinHTTP implementation on Windows and a small POSIX-socket
   one for the Linux test run).
3. **Manual checklist on a console** (in `docs/TESTING.md`): USB ↔ internal
   copy of a 20 GB folder, cancel mid-copy, PKG install, non-UTF-8 file names,
   payload restarted mid-transfer, Wi-Fi drop.

---

## 10. Milestones

| Phase | Scope | Rough size |
|---|---|---|
| 0 | Skeleton: Far exports, hosts level, config, `Http` + `Ps5Client.List`, `PathCodec` + unit tests | ~600 LOC |
| 1 (MVP) | Browse, F5/F6 both directions with progress, F7, F8, Shift+F6, F3/F4, task polling and `active_task` recovery | ~1,500 LOC total |
| 2 | On-console copy/move intercept, actions menu (PKG, ELF, extract, chmod, space), resume on download, CI with payload integration tests | +800 LOC |
| 3 | SMB volumes (`/api/smb/connect`, password prompt, kept for the Far session only). Not done: storing SMB passwords, following symlinked folders, PS5↔PS5 across two consoles without Far's temp-dir round trip | +300 LOC |

---

## 11. Risks

| Risk | Mitigation |
|---|---|
| Undocumented, unversioned API changes | Pin v1.12, version check on connect, integration tests against the pinned commit, isolate all wire details in `Ps5Client` |
| Wrong path encoding corrupts or misses files | `PathCodec` as the only way to build path params, raw paths kept in `UserData`, unit tests for Latin-1/CJK/invalid bytes |
| Far UI freezes on network stalls | Explicit WinHTTP timeouts, chunked I/O with Esc checks |
| Console left with a stuck task | Always pair cancel with `upload/finish`; `active_task` recovery dialog |
| Very large directories (10k+ entries) | 16 MiB JSON limit, a streaming-friendly parser, listing cache |
| No auth: anyone on the LAN can use the payload | Out of scope for the plugin; noted in the README |

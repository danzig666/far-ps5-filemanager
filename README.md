# far-ps5-filemanager

A Far Manager 3 panel plugin (in the style of the Network plugin and NetBox)
for browsing and managing a PS5's file system through the
[ps5-web-file-manager](https://github.com/owendswang/ps5-web-file-manager)
payload's HTTP API.

- Target: Far Manager **3.0.6638+**, **Windows x64**
- Payload: ps5-web-file-manager **v1.12**

Download the latest build from the [Releases](../../releases) page. Usage: [docs/PLUGIN-README.txt](docs/PLUGIN-README.txt). Design: [docs/DESIGN.md](docs/DESIGN.md).

Highlights:

- Browse console storage (internal, USB, M.2, extended) as a Far panel
- F5/F6 between PC and PS5 with progress, resume on download, and cancel
- **On-console copy and move** between two PS5 panels, with no round trip
  through the PC
- F7, F8, Shift+F6, F3/F4
- Actions menu: install PKG, launch ELF, extract archive, chmod, free space

Note: the payload has no authentication. Anyone on your LAN who can reach the
port can read and write the console's files while it runs.

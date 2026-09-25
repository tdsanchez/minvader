# minvader

A zero-dependency media server in C. Point it at any directory and browse your files — images, video, audio, documents — through a web UI in your browser. Runs on Mac, Windows, and Linux.

minvader is the core of [PostMac](https://github.com/tdsanchez) — distilled from a full Go + Erlang/OTP media server stack into a single C binary under 2MB.

## Platforms

| Binary | Platforms | Size |
|--------|-----------|------|
| `minvader` | macOS 10.6+ (Intel + Apple Silicon fat binary) | ~600KB |
| `minvader.exe` | Windows 10 x86_64 | ~1.7MB |
| `minvader` (Linux build) | Linux x86_64 | ~300KB |

The Windows binary is larger because it includes the SQLite amalgamation (no DLL dependency).

## Quick Start

**macOS / Linux:**
```
find /path/to/files -type f | ./minvader --stdin --port=9090
```

**Windows:**
```
dir /s /b C:\Photos | minvader.exe --stdin --port=9090
```

Then open `http://localhost:9090` in your browser.

### Recommended Browsers (macOS)

The author recommends these browsers based on testing across vintage and modern macOS:

| macOS version | Browser |
|---------------|---------|
| 10.6 Snow Leopard | [Power Fox](https://powerfox.org/) |
| 10.11 El Capitan – 10.15 Catalina | [Momiji-Stable](https://github.com/aobaharuki2005/momiji-web-browser/releases) |
| 11 Big Sur+ | [Waterfox](https://www.waterfox.net/) |

Any browser should work — the UI is server-rendered HTML with minimal JS and no framework dependencies. It may run fine in browsers going back a decade or more.

### Tested Scale

| Hardware | OS | Browser | Files | Notes |
|----------|----|---------|-------|-------|
| 2008 MacBook, spinning disk, 2GB RAM | Snow Leopard | Power Fox | ~500,000 | No issues |
| 2014 Mac Mini, 16GB RAM | El Capitan | Momiji-Stable | ~4,000,000 | No issues |
| M4 Mac Mini, 48GB RAM | macOS Sequoia | Waterfox | 10,500,000 | No issues |
| 6-core Xeon Mac Pro, 32GB RAM (VMware) | Windows 10 | Chromium | — | No issues |
| Ryzen 9, 32GB RAM | Debian Trixie | Waterfox | — | No issues |

### Warm Restart

When you load files with `--stdin`, minvader creates a cache database at `~/.minvader/cache-PORT.db`. Tags you add during the session are saved there. Next time, skip the scan:

```
./minvader --warm --port=9090
```

## GUI Launchers

Native GUI launchers are included for macOS (Cocoa) and Windows (Win32). They provide a simple window with port, directory picker, and warm/fresh toggle — no command line needed.

- **macOS:** `minvader-launcher.app` — double-click to launch
- **Windows:** `minvader-launcher.exe` — place next to `minvader.exe`

## Features

- **Gallery view** with thumbnail grid, category/tag sidebar, sorting, pagination
- **Single file viewer** with keyboard navigation, zoom, fullscreen
- **Tagging** — add/remove tags per file, persisted to SQLite cache DB
- **Tag hints** — autocomplete from existing tags as you type
- **Random mode** — press N to jump to a random file in the current category
- **Slideshow** — press S for auto-advancing slideshow with progress bar
- **Markdown rendering** — .md files rendered inline with full formatting
- **Search** — filename search across all loaded files
- **Delete** — move files to Trash (macOS) with X key. Windows/Linux deletion is a planned feature; use Reveal (R key) and delete natively as a workaround. minvader is not capable of batch deletions by policy and design.


## Keyboard Shortcuts (Single File View)

| Key | Action |
|-----|--------|
| Left/Right | Previous / Next file |
| Space | Next file |
| N | Random file |
| T | Focus tag input |
| L | Add heart tag |
| S | Toggle slideshow |
| Z | Toggle zoom (images) |
| F | Fullscreen |
| X | Delete (move to Trash) |
| R | Reveal in Finder |
| D | Download |
| Esc | Back to gallery |
| ? | Keyboard help |

## Building from Source

### macOS (fat binary, 10.6+)
```
cc -O2 -arch arm64 -arch x86_64 -mmacosx-version-min=10.6 \
   -framework CoreServices -framework CoreFoundation -lsqlite3 \
   -o minvader main.c tag.c entity.c md4c.c md4c-html.c
codesign -s - minvader
```

### Linux
```
cc -O2 -o minvader main.c tag.c entity.c md4c.c md4c-html.c -lsqlite3 -lm
```

### Windows (cross-compile from Linux with MinGW-w64)

Download the [SQLite amalgamation](https://www.sqlite.org/download.html) and place `sqlite3.c` and `sqlite3.h` in the source directory, then:
```
x86_64-w64-mingw32-gcc -O2 -o minvader.exe \
   main.c tag.c entity.c md4c.c md4c-html.c sqlite3.c \
   -I. -lws2_32 -lshlwapi
```

### macOS GUI Launcher
```
cc -O2 -fobjc-arc -arch arm64 -arch x86_64 -mmacosx-version-min=10.11 \
   -framework Cocoa -o minvader-launcher launcher.m
```
Then create a `.app` bundle:
```
mkdir -p minvader-launcher.app/Contents/MacOS
cp minvader-launcher minvader minvader-launcher.app/Contents/MacOS/
cp Info.plist minvader-launcher.app/Contents/
codesign -s - minvader-launcher.app
```

### Windows GUI Launcher (cross-compile)
```
x86_64-w64-mingw32-gcc -O2 -o minvader-launcher.exe \
   launcher-win32.c -lgdi32 -lcomdlg32 -lshell32 -lole32 -mwindows
```

## Architecture

minvader is a single-process HTTP server. On startup it loads file paths either from stdin (`--stdin`) or from a SQLite cache database (`--warm`). All file metadata is held in a flat array in memory — navigation, search, and random access are O(1) array lookups.

Tags are stored as macOS extended attributes (xattr) where available, and always written through to the SQLite cache DB for portability. On platforms without xattr (Linux, Windows, older macOS), the cache DB is the sole tag store.

The web UI is generated server-side as HTML — no JavaScript framework, no build step, no CDN dependencies. Tested in Power Fox on Snow Leopard.

### Third-party code

- [md4c](https://github.com/mity/md4c) — Markdown parser (MIT license, included in source)

## License

MIT

<div align="center">

# 🌌 Celestial Bypass Research & Analysis
**Reverse Engineering the Celestial.exe v2.4.1 Skin-Hack Filter & API**

[![Educational Purposes Only](https://img.shields.io/badge/Purpose-Educational-blue.svg)](#)
[![Target](https://img.shields.io/badge/Target-Celestial.exe_v2.4.1-purple.svg)](#)
[![Architecture](https://img.shields.io/badge/Arch-x64_Rust-orange.svg)](#)

*Expressing my immense love to Celestial and Divine skins!*  
*Deepest appreciation and gratitude for all your hard work and dedication.*  
**LoveCELESTIAL! ✨**

</div>

---

## 📖 Overview
This repository contains documentation of the reverse engineering, architecture, and bypass methodology for the internal mod-filtering system in Celestial v2.4.1. This research explores Rust ABI memory layouts, MSVC compiler quirks, and Tauri backend manipulation. 

*This documentation and the associated tools are strictly for educational research and software architecture study.*

---

## 🛡️ The Skin-Hack Filter Architecture

Through static analysis, the complete lifecycle of a mod passing through Celestial's verification engine was mapped out. Here is how the internal filter processes every custom asset:

```text
[Mod Scanned by Skin-Hack Filter]
  │
  ├── 1. Whitelist Check
  │     ├── Is it built-in? ("__noskin__", "__honor_recall__") ──► ALLOW
  │     └── Is it from Celestial Store? (Verified hash) ─────────► ALLOW
  │
  ├── 2. Blocklist Check (sub_1404F11E0)
  │     ├── Matches known pirated mod fingerprint? ──────────────► DETECTED
  │     ├── Author on blacklist? ────────────────────────────────► DETECTED
  │     └── Metadata has tool keywords? ("rose", "darkseal") ────► DETECTED
  │
  ├── 3. Riot Asset Hash Check (sub_1404E70F0)
  │     └── Does skin0.bin reference paid Riot skin 3D/tex hashes? ─► DETECTED
  │
  ├── 4. Heuristic .bin Pointer-Swap Check (sub_14161FD90)
  │     ├── WAD has <= 10 files?
  │     ├── Contains ZERO custom 3D models / textures?
  │     └── skin0 loadscreen points to "Skin02+", NOT "Base"? ───► DETECTED
  │
  └── [ACTION ON DETECTION]
        ├── Inside "creator_hub"? ──► Just disable for run (keep files)
        ├── Can be auto-stripped? ──► Strip skinhack, keep custom parts
        └── Pure skin-hack? ────────► Move to <TEMP>/celestial-quarantine
```

---

## 🔍 Reverse Engineering Findings

### 1. The Rust ABI Secret: `INT64_MIN`
Instead of returning a boolean, the security functions return a 24-byte `Rust String` struct (Length, Pointer, Capacity). To signal **"No Detection / Safe"**, the program writes a highly specific sentinel value: `0x8000000000000000` (`INT64_MIN`). 

```c
// How Celestial verifies the check internally:
if ( __OFSUB__(-v98[0], 1LL) ) {
    // If the length field is INT64_MIN, it overflows!
    // This tells the engine: "No detection found, ALLOW skin."
}
```
*Note: A naive bypass writing `0` (empty string) actually **fails** because `0` does not trigger the overflow subtraction check, causing the engine to read uninitialized memory and crash!*

### 2. The MSVC Decoy Trap
Because of how the MSVC compiler rounds up stack frame sizes, the exact 48-byte prologue for these security checks (`sub rsp, 0x148` etc.) is **shared by decoy functions**. A standard pattern scanner will hook the wrong function entirely. A deeper unique byte-walk analysis was required, utilizing specific SSE2 memory offsets 42+ bytes deep into the function bodies.

### 3. Patcher Preflight Trap
A function at `0x11D24D0` scans running processes for managers. Initially, it looked like a security check, but hooking it and forcing a success code completely breaks the game. It turns out this function is the **actual asset patcher**, and bypassing it simply stops the assets from installing.

---

## 🧰 Toolkit Architecture & API

To safely interact with and bypass the filter, a 3-part custom toolkit architecture was engineered.

### 1. The Launcher (`celestial_launcher.exe`)
*   Bootstraps `Celestial.exe` using `CREATE_SUSPENDED`.
*   Injects the DLL payload into the frozen process using `VirtualAllocEx` and `CreateRemoteThread`.
*   Injects required environment variables (e.g., `CELESTIAL_NO_HIDE`) before Tauri/WebView2 initializes.

### 2. The Payload (`LoveCelestial.dll`)
*   **MinHook Intercepts:** Places detours on the 3 critical filter RVAs, injecting the `INT64_MIN` ABI structs.
*   **CDP (Chrome DevTools Protocol) Bridge:** Overcomes Tauri's 3-second HTTP `keep-alive` stall by manually parsing `Content-Length` headers, and maintains a persistent WebSocket cache for instantaneous zero-latency Javascript evaluation.
*   **TCP Server:** Hosts a headless API server on a dynamic port (`46321-46340`) and broadcasts the active port to the Windows Registry and `%LOCALAPPDATA%`.

### 3. The Controller API (TCP / JSON)
The Python client (`celestial_controller.py`) auto-discovers the active TCP port and acts as a bridge for commands.

#### **Supported API Payloads:**
Communication is done via newline-delimited JSON over TCP.

| Command | Payload Structure | Description |
| :--- | :--- | :--- |
| **Hello** | `{"cmd": "hello"}` | Returns API version, target PID, and credits. |
| **Status** | `{"cmd": "status"}` | Returns live stats on hook resolution, call counts, and CDP state. |
| **List Mods** | `{"cmd": "list"}` | Invokes Tauri internals to fetch the user's mod library. |
| **Toggle Mod** | `{"cmd": "toggle", "target": "<id>", "enabled": true}` | Enables/Disables a specific mod in real-time. |
| **Import** | `{"cmd": "import", "path": "C:/path/to.wad.client"}` | Auto-detects mod type (WAD, Fantome, Modpkg) and imports it. |
| **Hooks** | `{"cmd": "hook_enable"}` / `hook_disable` | Dynamically toggles the MinHook detours on/off. |
| **Patcher** | `{"cmd": "start_patcher"}` / `stop_patcher` | Triggers the internal Tauri patcher preflight. |

#### **Python REPL Example:**
The controller features an interactive shell for rapid testing:
```bash
celestial> status
{
  "ok": true,
  "hooks": {
    "blocklist": {"resolved": true, "calls": 12},
    "heuristic": {"resolved": true, "calls": 4}
  },
  "cdp": {"available": true, "port": 9222}
}

celestial> toggle MyCoolSkin
{
  "ok": true,
  "modId": "abd00f7c-d173-4270-a89b-e8f3ccb8a0e7",
  "response": {"ok": true}
}
```

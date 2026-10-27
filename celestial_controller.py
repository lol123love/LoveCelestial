# Made by lol123love
# Expressing my immense love to Celestial and Divine skins!
# Deepest appreciation and gratitude for all your hard work and dedication.
# LoveCELESTIAL!

import sys, os, json, socket, argparse, time

PORT_RANGE_START = 46321
PORT_RANGE_END   = 46340
REG_PATH         = r"Software\LoveCelestial"
REG_VALUE        = "ApiPort"

_port_cache = None

def _probe(port, timeout=0.4):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    except (ConnectionRefusedError, socket.timeout, OSError):
        return None
    try:
        s.sendall(b'{"cmd":"hello"}\n')
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(4096)
            if not chunk: return None
            buf += chunk
        r = json.loads(buf.split(b"\n", 1)[0].decode("utf-8", "replace"))
        if r.get("ok") is True and r.get("service") == "LoveCelestial":
            return r
        return None
    except Exception:
        return None
    finally:
        s.close()

def _port_from_registry():
    try:
        import winreg
        k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, REG_PATH)
        try:
            v, _ = winreg.QueryValueEx(k, REG_VALUE)
            return int(v)
        finally:
            winreg.CloseKey(k)
    except Exception:
        return None

def _port_from_file(path):
    try:
        with open(path, "r") as f:
            return int(f.read().strip())
    except Exception:
        return None

def _discover_port():
    seen = set()

    p = _port_from_registry()
    if p and p not in seen:
        seen.add(p)
        if _probe(p): return p

    la = os.environ.get("LOCALAPPDATA")
    if la:
        p = _port_from_file(os.path.join(la, "LoveCelestial", "port.txt"))
        if p and p not in seen:
            seen.add(p)
            if _probe(p): return p

    p = _port_from_file(r"C:\Users\Public\LoveCelestial.port")
    if p and p not in seen:
        seen.add(p)
        if _probe(p): return p

    for port in range(PORT_RANGE_START, PORT_RANGE_END + 1):
        if port in seen: continue
        if _probe(port): return port

    return None

def _get_port():
    global _port_cache
    if _port_cache is not None and _probe(_port_cache):
        return _port_cache
    _port_cache = _discover_port()
    if _port_cache is None:
        raise RuntimeError(
            "LoveCelestial API not reachable.\n"
            f"  Tried registry HKCU\\{REG_PATH}, LOCALAPPDATA file, "
            f"C:\\Users\\Public\\LoveCelestial.port, and scan {PORT_RANGE_START}..{PORT_RANGE_END}.\n"
            "  LoveCelestial.dll is not running inside Celestial.exe.\n"
            "  Run celestial_launcher.exe first."
        )
    return _port_cache

def call(req: dict, timeout: float = 30.0) -> dict:
    global _port_cache
    for attempt in range(2):
        port = _get_port()
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
            s.sendall((json.dumps(req) + "\n").encode())
            buf = b""
            while b"\n" not in buf:
                chunk = s.recv(65536)
                if not chunk: raise ConnectionError("DLL closed connection")
                buf += chunk
            return json.loads(buf.split(b"\n", 1)[0].decode("utf-8", "replace"))
        except Exception:
            if attempt == 0:
                _port_cache = None
                continue
            raise
        finally:
            try: s.close()
            except Exception: pass

def pretty(o): return json.dumps(o, indent=2, ensure_ascii=False)

def hello()                 -> dict: return call({"cmd": "hello"})
def ping()                  -> dict: return call({"cmd": "ping"})
def status()                -> dict: return call({"cmd": "status"})
def list_mods()             -> dict: return call({"cmd": "list"})
def import_skin(p)          -> dict: return call({"cmd": "import", "path": os.path.abspath(p)})
def toggle(t, enabled=True) -> dict: return call({"cmd": "toggle", "target": t, "enabled": enabled})
def start_patcher()         -> dict: return call({"cmd": "start_patcher"})
def stop_patcher()          -> dict: return call({"cmd": "stop_patcher"})
def raw(cmd, args=None)     -> dict: return call({"cmd": "raw_invoke", "command": cmd, "args": args or {}})
def hook_enable()           -> dict: return call({"cmd": "hook_enable"})
def hook_disable()          -> dict: return call({"cmd": "hook_disable"})
def kill()                  -> dict:
    try: return call({"cmd": "kill"})
    except Exception: return {"ok": True, "note": "connection dropped — game terminating"}

def self_test() -> int:
    print("=" * 60); print("celestial_bypass self-test (TCP)"); print("=" * 60)

    print("\n[discover]")
    try:
        p = _get_port()
        print(f"  port = {p}")
    except RuntimeError as e:
        print(f"  FAIL: {e}")
        return 1

    failures = 0
    def check(name, fn, pred=lambda r: r.get("ok") is True):
        nonlocal failures
        print(f"\n[TEST] {name}")
        try:
            r = fn(); print(pretty(r))
            if pred(r): print("  -> PASS")
            else:       print("  -> FAIL"); failures += 1
        except Exception as e:
            print(f"  -> FAIL: {e}"); failures += 1

    check("hello",            hello,         lambda r: r.get("service") == "LoveCelestial")
    check("ping",             ping,          lambda r: r.get("ok") is True)
    check("status/hooks",     status,        lambda r: all(
        r.get("hooks", {}).get(k, {}).get("resolved")
        for k in ("blocklist", "hash", "heuristic")))
    check("hook_disable",     hook_disable,  lambda r: r.get("ok") is True)
    check("hook_enable",      hook_enable,   lambda r: r.get("ok") is True)
    check("CDP available",    status,        lambda r: r.get("cdp", {}).get("available") is True)
    check("list mods",        list_mods,     lambda r: isinstance(r, dict) and "library" in r)

    print("\n" + "=" * 60); print("PASSED" if failures == 0 else f"FAILED ({failures})")
    return 0 if failures == 0 else 1

def repl():
    print("celestial REPL — 'help', 'port', 'kill', Ctrl-C to quit")
    while True:
        try: line = input("celestial> ").strip()
        except (EOFError, KeyboardInterrupt): print(); break
        if not line: continue
        if line in ("q","quit","exit"): break
        if line == "help":
            print("  port | hello | status | list | start | stop")
            print("  hooks-on | hooks-off | kill")
            print("  import <path> | toggle <name|id> [off] | raw <cmd> [args]")
            continue
        try:
            parts = line.split(maxsplit=2); cmd = parts[0]
            if   cmd == "port":      print(f"port = {_get_port()}")
            elif cmd == "hello":     print(pretty(hello()))
            elif cmd == "status":    print(pretty(status()))
            elif cmd == "list":      print(pretty(list_mods()))
            elif cmd == "start":     print(pretty(start_patcher()))
            elif cmd == "stop":      print(pretty(stop_patcher()))
            elif cmd == "hooks-on":  print(pretty(hook_enable()))
            elif cmd == "hooks-off": print(pretty(hook_disable()))
            elif cmd == "kill":      print(pretty(kill())); break
            elif cmd == "import" and len(parts) >= 2: print(pretty(import_skin(parts[1])))
            elif cmd == "toggle" and len(parts) >= 2:
                enable = not (len(parts) >= 3 and parts[2] in ("off","--disable","false"))
                print(pretty(toggle(parts[1], enable)))
            elif cmd == "raw" and len(parts) >= 2:
                args = json.loads(parts[2]) if len(parts) >= 3 else {}
                print(pretty(raw(parts[1], args)))
            else: print(f"unknown: {cmd}")
        except Exception as e: print(f"error: {e}")

def main():
    ap = argparse.ArgumentParser(); sub = ap.add_subparsers(dest="action")
    for n in ("hello","status","ping","list","start","stop","hooks-on","hooks-off",
              "kill","port","selftest","shell"):
        sub.add_parser(n)
    p = sub.add_parser("import"); p.add_argument("path")
    p = sub.add_parser("toggle"); p.add_argument("target"); p.add_argument("--disable", action="store_true")
    p = sub.add_parser("raw");    p.add_argument("command"); p.add_argument("args", nargs="?", default="{}")
    a = ap.parse_args()

    try:
        if a.action in (None, "selftest"): sys.exit(self_test())
        if   a.action == "port":     print(_get_port())
        elif a.action == "hello":    print(pretty(hello()))
        elif a.action == "ping":     print(pretty(ping()))
        elif a.action == "status":   print(pretty(status()))
        elif a.action == "list":     print(pretty(list_mods()))
        elif a.action == "start":    print(pretty(start_patcher()))
        elif a.action == "stop":     print(pretty(stop_patcher()))
        elif a.action == "hooks-on": print(pretty(hook_enable()))
        elif a.action == "hooks-off":print(pretty(hook_disable()))
        elif a.action == "kill":     print(pretty(kill()))
        elif a.action == "import":   print(pretty(import_skin(a.path)))
        elif a.action == "toggle":   print(pretty(toggle(a.target, not a.disable)))
        elif a.action == "raw":      print(pretty(raw(a.command, json.loads(a.args))))
        elif a.action == "shell":    repl()
    except RuntimeError as e:
        print(f"ERROR: {e}", file=sys.stderr); sys.exit(1)

if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""urushi_dev_mcp.py - Development MCP server & CLI for URUSHI.

Bridges an MCP client or direct --cli calls to:
  1. The in-game named pipe \\\\.\\pipe\\urushi_dev (DX11 backbuffer PNG shots,
     ImGui layout inspection, menu/window control, Lua eval & hot-reload,
     CarFX / Optimizer state, synthetic UI input).
  2. The version.dll reload event Local\\UrushiReload_<pid> (hot-swaps
     urushi.dll inside the live game without restarting).

Pure Python stdlib (no third-party packages required).
"""

import argparse
import base64
import ctypes
from ctypes import wintypes
import json
import os
import shutil
import subprocess
import sys
import time

PIPE_PATH = r"\\.\pipe\urushi_dev"
DEFAULT_GAME_DIR = r"C:\Program Files (x86)\Steam\steamapps\common\CarX Drift Racing Online 2"
REPO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME_EXE_NAME = "CarX Drift Racing Online 2.exe"

PIPE_RETRIES = 10
PIPE_RETRY_WAIT_S = 0.08
PROTOCOL_VERSION = "2024-11-05"


class PipeError(RuntimeError):
    pass


def _find_game_pids():
    TH32CS_SNAPPROCESS = 0x00000002
    INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

    class PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [
            ("dwSize", wintypes.DWORD),
            ("cntUsage", wintypes.DWORD),
            ("th32ProcessID", wintypes.DWORD),
            ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
            ("th32ModuleID", wintypes.DWORD),
            ("cntThreads", wintypes.DWORD),
            ("th32ParentProcessID", wintypes.DWORD),
            ("pcPriClassBase", ctypes.c_long),
            ("dwFlags", wintypes.DWORD),
            ("szExeFile", wintypes.WCHAR * 260),
        ]

    k32 = ctypes.windll.kernel32
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == INVALID_HANDLE_VALUE or not snap:
        return []
    pids = []
    try:
        pe = PROCESSENTRY32W()
        pe.dwSize = ctypes.sizeof(PROCESSENTRY32W)
        if k32.Process32FirstW(snap, ctypes.byref(pe)):
            while True:
                if pe.szExeFile.lower() == GAME_EXE_NAME.lower():
                    pids.append(int(pe.th32ProcessID))
                if not k32.Process32NextW(snap, ctypes.byref(pe)):
                    break
    finally:
        k32.CloseHandle(snap)
    return pids


def _signal_dll_reload(pid=None):
    pids = [int(pid)] if pid else _find_game_pids()
    if not pids:
        raise RuntimeError(f"{GAME_EXE_NAME} is not running")

    EVENT_MODIFY_STATE = 0x0002
    k32 = ctypes.windll.kernel32
    k32.OpenEventW.restype = wintypes.HANDLE
    k32.OpenEventW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
    k32.SetEvent.argtypes = [wintypes.HANDLE]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]

    signaled = []
    for p in pids:
        ev_name = f"Local\\UrushiReload_{p}"
        h = k32.OpenEventW(EVENT_MODIFY_STATE, False, ev_name)
        if h:
            k32.SetEvent(h)
            k32.CloseHandle(h)
            signaled.append(p)
    if not signaled:
        raise RuntimeError(f"Could not open Local\\UrushiReload_<pid> for {pids}. Is proxy version.dll loaded?")
    return signaled


def _deploy_artifacts(game_dir=None):
    game_dir = game_dir or os.environ.get("URUSHI_GAME_DIR") or DEFAULT_GAME_DIR
    dll_src = os.path.join(REPO_DIR, "build", "core", "Release", "urushi.dll")
    if not os.path.isfile(dll_src):
        dll_src = os.path.join(REPO_DIR, "build", "core", "Release", "CarX2ScriptHook.dll")
    scripts_src = os.path.join(REPO_DIR, "scripts")
    copied = []
    if os.path.isfile(dll_src):
        shutil.copy2(dll_src, os.path.join(game_dir, "urushi.dll"))
        shutil.copy2(dll_src, os.path.join(game_dir, "CarX2ScriptHook.dll"))
        copied.append("urushi.dll")
    if os.path.isdir(scripts_src):
        dst_scripts = os.path.join(game_dir, "scripts")
        os.makedirs(dst_scripts, exist_ok=True)
        for fn in os.listdir(scripts_src):
            if fn.lower().endswith(".lua"):
                shutil.copy2(os.path.join(scripts_src, fn), os.path.join(dst_scripts, fn))
                copied.append(f"scripts/{fn}")
    shaders_src = os.path.join(REPO_DIR, "shaders")
    if os.path.isdir(shaders_src):
        dst_shaders = os.path.join(game_dir, "shaders")
        os.makedirs(dst_shaders, exist_ok=True)
        for fn in os.listdir(shaders_src):
            if fn.lower().endswith((".hlsl", ".hlsli")):
                shutil.copy2(os.path.join(shaders_src, fn), os.path.join(dst_shaders, fn))
                copied.append(f"shaders/{fn}")
    return copied


def pipe_call(cmd, args=None, req_id=1):
    payload = json.dumps({"id": req_id, "cmd": cmd, "args": args or {}}, ensure_ascii=False).encode("utf-8") + b"\n"
    last_err = None
    for _ in range(PIPE_RETRIES):
        try:
            with open(PIPE_PATH, "r+b", buffering=0) as f:
                f.write(payload)
                buf = b""
                while b"\n" not in buf:
                    chunk = f.read(1 << 20)
                    if not chunk:
                        raise PipeError("pipe closed before reply")
                    buf += chunk
                line = buf.split(b"\n", 1)[0].decode("utf-8", errors="replace")
                return json.loads(line)
        except OSError as e:
            if getattr(e, "winerror", None) in (2, 231):
                last_err = e
                time.sleep(PIPE_RETRY_WAIT_S)
                continue
            raise PipeError(f"{PIPE_PATH} error: {e}") from e
    raise PipeError(f"{PIPE_PATH} unavailable ({last_err}). Is updated urushi.dll loaded in the game?")


def wait_for_pipe(timeout_s=10.0):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            res = pipe_call("ping")
            if res.get("ok"):
                return res
        except PipeError:
            pass
        time.sleep(0.15)
    raise PipeError(f"{PIPE_PATH} did not come back online within {timeout_s:.1f}s")


# -------------------- MCP Tool Definitions & Handlers --------------------

TOOLS = [
    {
        "name": "urushi_state",
        "description": "Get live CarX2ScriptHook/URUSHI state: PID, FPS, menu_open, in_session, accent, net_cars, viewport dimensions, dock rectangle, and all registered windows with exact pixel coordinates.",
        "inputSchema": {"type": "object", "properties": {}, "required": []},
    },
    {
        "name": "urushi_windows",
        "description": "Inspect all registered floating UI windows (id, title, open, pinned, shaded, scrollable, x, y, width, actual_w, actual_h, body_h).",
        "inputSchema": {"type": "object", "properties": {}, "required": []},
    },
    {
        "name": "urushi_menu",
        "description": "Open, close, or toggle the URUSHI left dock menu without needing keyboard focus.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {"type": "string", "enum": ["open", "close", "toggle"], "description": "Menu action (default toggle)."}
            },
            "required": [],
        },
    },
    {
        "name": "urushi_window",
        "description": "Control any URUSHI floating window: open, close, toggle, solo (open only this window), pin, unpin, shade, unshade, move, or close_all.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {
                    "type": "string",
                    "enum": ["open", "close", "toggle", "solo", "pin", "unpin", "shade", "unshade", "move", "close_all"],
                    "description": "Action to perform.",
                },
                "id": {"type": "string", "description": "Window id (perf, carfx, replay, anticollision, ghost, hud, fps_net, system)."},
                "x": {"type": "number", "description": "X coordinate relative to work area (for action='move')."},
                "y": {"type": "number", "description": "Y coordinate relative to work area (for action='move')."},
            },
            "required": ["action"],
        },
    },
    {
        "name": "urushi_accent",
        "description": "Switch URUSHI lacquer theme accent color.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string", "enum": ["amber", "vermilion", "platinum", "indigo"], "description": "Accent preset name."}
            },
            "required": ["name"],
        },
    },
    {
        "name": "urushi_shot",
        "description": "Capture an exact PNG screenshot directly from the DX11 SwapChain BackBuffer inside the running game (works even when the window is unfocused or overlapped by other apps). Returns PNG image.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "window_id": {"type": "string", "description": "Optional window id to crop around ('dock', 'perf', 'carfx', 'replay', 'anticollision', 'ghost', 'hud', 'fps_net', 'system'). Omit for full game frame."},
                "x": {"type": "integer", "description": "Optional custom crop left pixel."},
                "y": {"type": "integer", "description": "Optional custom crop top pixel."},
                "w": {"type": "integer", "description": "Optional custom crop width."},
                "h": {"type": "integer", "description": "Optional custom crop height."},
                "sim_w": {"type": "integer", "description": "Optional simulated viewport width (e.g. 1280, 1920, 2560, 3840) for offscreen DX11 multi-resolution capture."},
                "sim_h": {"type": "integer", "description": "Optional simulated viewport height (e.g. 720, 1080, 1440, 2160) for offscreen DX11 multi-resolution capture."},
                "sim_scale": {"type": "number", "description": "Optional forced UI scale override (e.g. 1.0, 1.25, 1.5) during simulated capture. Omit to test Auto scale."},
                "delay_frames": {"type": "integer", "description": "Frames to wait before capture so layout/animations settle (default 2, or 3 for sim)."},
                "out_path": {"type": "string", "description": "Optional disk path to also save the captured PNG."},
            },
            "required": [],
        },
    },
    {
        "name": "urushi_click",
        "description": "Inject a synthetic ImGui mouse click at client pixel coordinates (x, y) without moving the physical cursor or focusing the game window.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "x": {"type": "number", "description": "Client-area X coordinate."},
                "y": {"type": "number", "description": "Client-area Y coordinate."},
                "button": {"type": "integer", "description": "Mouse button (0=left, 1=right, 2=middle; default 0)."},
            },
            "required": ["x", "y"],
        },
    },
    {
        "name": "urushi_lua_eval",
        "description": "Execute Lua code in the running game's LuaEngine and capture print() output / return value.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "code": {"type": "string", "description": "Lua code or expression to evaluate."}
            },
            "required": ["code"],
        },
    },
    {
        "name": "urushi_reload_lua",
        "description": "Reload all Lua scripts (scripts/main.lua and modules) inside the running game immediately.",
        "inputSchema": {"type": "object", "properties": {}, "required": []},
    },
    {
        "name": "urushi_reload_dll",
        "description": "Hot-reload urushi.dll inside the live running CarX Drift Racing Online 2 process via version.dll event (no game restart needed).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "deploy": {"type": "boolean", "description": "Copy freshly built Release DLL and scripts/*.lua to game directory before reloading (default false)."},
                "wait": {"type": "boolean", "description": "Wait for dev pipe to come back online after reload (default true)."},
            },
            "required": [],
        },
    },
    {
        "name": "urushi_logs",
        "description": "Read the last N lines of urushi.log from the game directory.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "lines": {"type": "integer", "description": "Number of tail lines (default 40, max 300)."}
            },
            "required": [],
        },
    },
    {
        "name": "urushi_optimizer",
        "description": "Inspect live C++ CarSimOptimizer config, hook status, switching calls, and anti-flicker blocks.",
        "inputSchema": {"type": "object", "properties": {}, "required": []},
    },
    {
        "name": "urushi_carfx",
        "description": "Inspect live CarFX DX11 shader hook status, body detection, and matched draws.",
        "inputSchema": {"type": "object", "properties": {}, "required": []},
    },
]


def handle_tool(name, params):
    params = dict(params or {})
    if name == "urushi_reload_dll":
        copied = []
        if params.get("deploy", False):
            copied = _deploy_artifacts()
        signaled = _signal_game = _signal_dll_reload()
        online = None
        if params.get("wait", True):
            time.sleep(0.45)
            online = wait_for_pipe(10.0)
        info = {"ok": True, "signaled_pids": signaled, "deployed": copied, "pipe_online": online}
        return {"content": [{"type": "text", "text": json.dumps(info, ensure_ascii=False, indent=2)}], "isError": False}

    cmd_map = {
        "urushi_state": "state",
        "urushi_windows": "windows",
        "urushi_menu": "menu",
        "urushi_window": "window",
        "urushi_accent": "accent",
        "urushi_shot": "shot",
        "urushi_click": "click",
        "urushi_lua_eval": "lua_eval",
        "urushi_reload_lua": "reload_lua",
        "urushi_logs": "logs",
        "urushi_optimizer": "optimizer",
        "urushi_carfx": "carfx",
    }
    if name not in cmd_map:
        raise ValueError(f"Unknown tool: {name}")

    out_path = params.pop("out_path", None)
    reply = pipe_call(cmd_map[name], params)
    is_err = not reply.get("ok", False)

    png_b64 = reply.pop("png_b64", None)
    content = []
    if png_b64:
        if out_path:
            os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
            with open(out_path, "wb") as f:
                f.write(base64.b64decode(png_b64))
            reply["saved_path"] = os.path.abspath(out_path)
        content.append({"type": "text", "text": json.dumps(reply, ensure_ascii=False, indent=2)})
        content.append({"type": "image", "data": png_b64, "mimeType": "image/png"})
    else:
        content.append({"type": "text", "text": json.dumps(reply, ensure_ascii=False, indent=2)})
    return {"content": content, "isError": is_err}


# -------------------- JSON-RPC Stdio Loop --------------------

def _envelope(msg_id, result=None, error=None):
    msg = {"jsonrpc": "2.0", "id": msg_id}
    if error is not None:
        msg["error"] = error
    else:
        msg["result"] = result
    return msg


def handle_message(msg):
    if not isinstance(msg, dict):
        return _envelope(None, error={"code": -32600, "message": "invalid request"})
    method = msg.get("method")
    msg_id = msg.get("id")
    if msg_id is None:
        return None
    try:
        if method == "initialize":
            return _envelope(msg_id, {
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": "urushi-dev", "title": "URUSHI Dev MCP", "version": "1.0.0"},
            })
        if method == "tools/list":
            return _envelope(msg_id, {"tools": TOOLS})
        if method == "tools/call":
            p = msg.get("params") or {}
            return _envelope(msg_id, handle_tool(p.get("name"), p.get("arguments")))
        if method == "ping":
            return _envelope(msg_id, {})
        return _envelope(msg_id, error={"code": -32601, "message": f"method not found: {method}"})
    except (PipeError, RuntimeError, ValueError) as e:
        if method == "tools/call":
            return _envelope(msg_id, {"content": [{"type": "text", "text": str(e)}], "isError": True})
        return _envelope(msg_id, error={"code": -32603, "message": str(e)})
    except Exception as e:
        return _envelope(msg_id, error={"code": -32603, "message": str(e)})


def serve():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    if hasattr(sys.stdin, "reconfigure"):
        sys.stdin.reconfigure(encoding="utf-8")
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue
        reply = handle_message(msg)
        if reply is not None:
            sys.stdout.write(json.dumps(reply, ensure_ascii=False) + "\n")
            sys.stdout.flush()


def cli(argv):
    extracted_out = ""
    sim_args = {}
    filtered = []
    i = 0
    while i < len(argv):
        if argv[i] == "--out" and i + 1 < len(argv):
            extracted_out = argv[i + 1]
            i += 2
        elif argv[i] == "--sim" and i + 2 < len(argv):
            sim_args["sim_w"] = int(argv[i + 1])
            sim_args["sim_h"] = int(argv[i + 2])
            i += 3
            if i < len(argv) and not argv[i].startswith("--"):
                try:
                    sim_args["sim_scale"] = float(argv[i])
                    i += 1
                except ValueError:
                    pass
        else:
            filtered.append(argv[i])
            i += 1

    ap = argparse.ArgumentParser(description="URUSHI Dev Pipe CLI / MCP")
    ap.add_argument("cmd", help="Command: state, windows, menu, window, accent, shot, click, lua_eval, reload_lua, reload_dll, deploy_reload, logs, optimizer, carfx")
    ap.add_argument("rest", nargs=argparse.REMAINDER)
    a = ap.parse_args(filtered)
    a.out = extracted_out

    cmd = a.cmd
    args = {}
    rest = a.rest

    if cmd == "deploy_reload":
        res = handle_tool("urushi_reload_dll", {"deploy": True, "wait": True})
        print(res["content"][0]["text"])
        return 1 if res.get("isError") else 0
    if cmd == "reload_dll":
        res = handle_tool("urushi_reload_dll", {"deploy": False, "wait": True})
        print(res["content"][0]["text"])
        return 1 if res.get("isError") else 0

    if cmd == "menu" and rest:
        args["action"] = rest[0]
    elif cmd == "window":
        if len(rest) >= 1:
            args["action"] = rest[0]
        if len(rest) >= 2:
            args["id"] = rest[1]
        if len(rest) >= 4:
            args["x"] = float(rest[2])
            args["y"] = float(rest[3])
    elif cmd == "accent" and rest:
        args["name"] = rest[0]
    elif cmd == "shot":
        if len(rest) == 1:
            args["window_id"] = rest[0]
        elif len(rest) >= 4:
            args["x"], args["y"], args["w"], args["h"] = map(int, rest[:4])
        args.update(sim_args)
    elif cmd == "click" and len(rest) >= 2:
        args["x"], args["y"] = float(rest[0]), float(rest[1])
        if len(rest) >= 3:
            args["button"] = int(rest[2])
    elif cmd == "lua_eval":
        args["code"] = " ".join(rest)
    elif cmd == "logs" and rest:
        args["lines"] = int(rest[0])

    try:
        reply = pipe_call(cmd, args)
    except PipeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    png_b64 = reply.pop("png_b64", None)
    if png_b64:
        out_file = a.out or os.path.join(REPO_DIR, "build", "urushi_shot.png")
        os.makedirs(os.path.dirname(os.path.abspath(out_file)), exist_ok=True)
        with open(out_file, "wb") as f:
            f.write(base64.b64decode(png_b64))
        reply["saved_path"] = os.path.abspath(out_file)
    print(json.dumps(reply, ensure_ascii=False, indent=2))
    return 0 if reply.get("ok") else 1


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--cli":
        sys.exit(cli(sys.argv[2:]))
    serve()

#!/usr/bin/env python3
"""MCP proxy for the Claude-friendly Godot fork.

Bridges the Model Context Protocol (MCP, over stdio) to the editor's
`--editor-rpc-port` JSON-RPC 2.0 server. Lets MCP clients (e.g. Claude
Code) query and (eventually) drive a running Godot editor.

Usage
-----
1. Start Godot with the RPC server enabled:

       godot --editor --editor-rpc-port 6664 --path /path/to/project

2. Install this proxy's dependencies (one-time):

       pip3 install -r tools/mcp_proxy/requirements.txt

3. Register the proxy with your MCP client. For Claude Code, in
   ~/.claude/mcp_servers.json (or via `claude mcp add`):

       {
         "godot-fork": {
           "command": "python3",
           "args": ["/abs/path/to/godot/tools/mcp_proxy/mcp_server.py"],
           "env": { "GODOT_RPC_PORT": "6664" }
         }
       }

Environment variables
---------------------
- GODOT_RPC_HOST   default 127.0.0.1
- GODOT_RPC_PORT   default 6664
- GODOT_RPC_TIMEOUT_S  default 5.0
"""

from __future__ import annotations

import json
import os
import socket
from typing import Any

from mcp.server.fastmcp import FastMCP

HOST = os.environ.get("GODOT_RPC_HOST", "127.0.0.1")
PORT = int(os.environ.get("GODOT_RPC_PORT", "6664"))
TIMEOUT_S = float(os.environ.get("GODOT_RPC_TIMEOUT_S", "5.0"))

mcp = FastMCP("godot-fork")

_next_id_counter = 0


def _next_id() -> int:
    global _next_id_counter
    _next_id_counter += 1
    return _next_id_counter


def _rpc_call(method: str, params: Any | None = None) -> Any:
    """Synchronous JSON-RPC call over a fresh TCP connection.

    Opens, sends one line, reads until newline, closes. Cheap and stateless;
    fine for the read-only methods exposed today. If we add subscriptions or
    high-frequency calls later, switch to a persistent connection.
    """
    req: dict[str, Any] = {"jsonrpc": "2.0", "method": method, "id": _next_id()}
    if params is not None:
        req["params"] = params
    payload = (json.dumps(req) + "\n").encode("utf-8")

    sock = socket.create_connection((HOST, PORT), timeout=TIMEOUT_S)
    try:
        sock.sendall(payload)
        sock.settimeout(TIMEOUT_S)
        buf = bytearray()
        while b"\n" not in buf:
            chunk = sock.recv(8192)
            if not chunk:
                break
            buf.extend(chunk)
    finally:
        sock.close()

    line, _, _ = bytes(buf).decode("utf-8").partition("\n")
    if not line.strip():
        raise RuntimeError(f"Godot RPC returned empty response for method '{method}'")
    resp = json.loads(line)
    if "error" in resp:
        err = resp["error"]
        raise RuntimeError(f"Godot RPC error {err.get('code')}: {err.get('message')}")
    return resp.get("result")


@mcp.tool()
def godot_ping() -> str:
    """Check that the Godot editor's RPC server is reachable. Returns "pong" on success."""
    return _rpc_call("ping")


@mcp.tool()
def godot_get_current_scene_path() -> str:
    """Return the file path (e.g. 'res://main.tscn') of the scene currently being edited.

    Returns an empty string if no scene is open in the active tab.
    """
    return _rpc_call("editor.get_current_scene_path")


@mcp.tool()
def godot_get_scene_tree() -> dict:
    """Return the currently edited scene's node tree as a nested object.

    Each node has: name, class, optional properties (editor-visible only),
    optional children (array of nodes). Returns null/None if no scene is open.
    """
    return _rpc_call("editor.get_scene_tree")


@mcp.tool()
def godot_list_open_scenes() -> list:
    """List all scenes currently open as tabs in the Godot editor.

    Returns an array of objects with fields: path, root_name, root_class.
    """
    return _rpc_call("editor.list_open_scenes")


@mcp.tool()
def godot_open_scene(path: str) -> str:
    """Open a scene in the editor, making it the currently edited scene.

    Args:
        path: Resource path of the scene to open (e.g. "res://main.tscn").

    Returns the path of the now-current scene on success.
    """
    return _rpc_call("editor.open_scene", {"path": path})


@mcp.tool()
def godot_save_scene() -> str:
    """Save the currently edited scene to its existing file path.

    Returns the path that was saved. Errors if no scene is open or the
    scene has no file path yet (use save-as instead, not yet implemented).
    """
    return _rpc_call("editor.save_scene")


@mcp.tool()
def godot_set_property(node_path: str, property: str, value: Any) -> Any:
    """Set a property on a node in the currently edited scene.

    Args:
        node_path: Path of the node relative to the scene root (e.g. "Sprite",
            "UI/Label", "."). The root is referred to as ".".
        property: Property name (e.g. "position", "text", "modulate").
        value: New value. Plain JSON types (string, number, bool) are passed
            through. JSON arrays of length 2/3/4 are coerced to Vector2/3/4 or
            Color when the target property is one of those types.

    Returns the value after the set (which may be coerced by Godot).
    """
    return _rpc_call("editor.set_property", {
        "node_path": node_path,
        "property": property,
        "value": value,
    })


@mcp.tool()
def godot_add_node(parent_path: str, class_: str, name: str | None = None) -> dict:
    """Add a new node as a child of an existing node in the currently edited scene.

    Args:
        parent_path: Path of the parent node, relative to the scene root.
            Use "." to add a child to the root.
        class_: Class name of the new node (e.g. "Sprite2D", "Label", "Node3D").
            Must be a registered subclass of Node.
        name: Optional name for the new node. Defaults to the class name.
            If a sibling with the same name exists, Godot will append a suffix.

    Returns an object with the actual name, class, and path of the created node.
    The node is owned by the scene root, so it will be saved when the scene is saved.
    """
    params: dict[str, Any] = {"parent_path": parent_path, "class": class_}
    if name is not None:
        params["name"] = name
    return _rpc_call("editor.add_node", params)


@mcp.tool()
def godot_delete_node(node_path: str) -> bool:
    """Delete a node from the currently edited scene.

    Args:
        node_path: Path of the node to delete, relative to the scene root.
            Cannot be the scene root itself ("."). All descendants are deleted
            along with the node.

    Returns True on success.
    """
    return _rpc_call("editor.delete_node", {"node_path": node_path})


@mcp.tool()
def godot_save_scene_as(path: str) -> str:
    """Save the currently edited scene to a new file path.

    Args:
        path: Destination resource path (e.g. "res://copy.tscn"). The scene's
            file path becomes this new path.

    Returns the path that was saved.
    """
    return _rpc_call("editor.save_scene_as", {"path": path})


@mcp.tool()
def godot_get_selected_nodes() -> list:
    """Return the paths of all nodes currently selected in the editor.

    Paths are relative to the active scene root. Returns an empty array if
    nothing is selected.
    """
    return _rpc_call("editor.get_selected_nodes")


@mcp.tool()
def godot_select_nodes(paths: list, replace: bool = True) -> list:
    """Select one or more nodes in the editor.

    Args:
        paths: Array of node paths (relative to the scene root) to select.
            Paths that do not resolve to a node are silently skipped.
        replace: If True (default), the existing selection is cleared first.
            If False, the given nodes are added to the current selection.

    Returns the paths that were actually selected (paths missing from the
    scene are filtered out).
    """
    return _rpc_call("editor.select_nodes", {"paths": paths, "replace": replace})


@mcp.tool()
def godot_play(scene_path: str | None = None) -> bool:
    """Run the project in Play mode (spawns the game process).

    Args:
        scene_path: Which scene to launch. Options:
            - None or "main" (default): play the project's configured main
              scene (application/run/main_scene in project.godot).
            - "current": play the scene currently active in the editor.
            - Any "res://..." path: play that specific scene.

    Returns True if Godot reports a play session is active after launching.
    """
    params: dict[str, Any] = {}
    if scene_path is not None:
        params["scene_path"] = scene_path
    return _rpc_call("editor.play", params if params else None)


@mcp.tool()
def godot_stop_playing() -> bool:
    """Stop any currently running play session. Safe to call when nothing is playing."""
    return _rpc_call("editor.stop_playing")


@mcp.tool()
def godot_is_playing() -> bool:
    """Return True if the editor currently has a play session running."""
    return _rpc_call("editor.is_playing")


if __name__ == "__main__":
    mcp.run()

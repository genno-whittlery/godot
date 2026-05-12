# Claude-friendly Godot fork

A small fork of [Godot](https://github.com/godotengine/godot) that adds four
features for tooling and AI agents (Claude Code in particular) to introspect
and drive the engine programmatically.

Each feature lives on its own branch and is designed to rebase cleanly onto
upstream `master` for potential PRs. `feature/mcp-server` is the integration
tip — it contains all four features stacked.

## The four features

| | Branch | Adds | Touches |
|---|---|---|---|
| F1 | `feature/json-output` | `--log-format json` — NDJSON logs/errors on stdout | `core/io/logger.{h,cpp}`, `core/os/os.{h,cpp}`, `main/main.cpp` |
| F2 | `feature/scene-inspect` | `--inspect-scene <path>` — one-shot scene tree dump | `core/io/scene_inspector.{h,cpp}`, `main/main.cpp` |
| F3 | `feature/editor-rpc` | `--editor-rpc-port <port>` — JSON-RPC 2.0 server in the editor | `editor/editor_rpc_server.{h,cpp}`, `editor/editor_node.{h,cpp}`, `main/main.{h,cpp}` |
| F4 | `feature/mcp-server` | MCP proxy bridging Claude Code to F3 | `tools/mcp_proxy/` |

### F1 — structured CLI output

Adds `--log-format <text\|json>`. When `json` is selected, every log line is a
single NDJSON record on stdout:

```bash
godot --headless --log-format json --script scene.gd
```

Output schema:
- info/print: `{"ts","level":"info","msg"}`
- errors/warnings: `{"ts","level","function","file","line","code","rationale","editor_notify","backtrace"?}`

`level` ∈ `info | error | warning | script_error | shader_error`.

### F2 — headless scene introspection

Adds `--inspect-scene <path>`. Loads a `.tscn` without launching the editor,
walks the node tree, prints a single nested JSON object to stdout, exits.

```bash
godot --headless --path /my/project --inspect-scene res://main.tscn
```

Each node has `name`, `class`, optional `properties` (editor-visible only),
optional `children`. Useful for scripted scene analysis and CI checks.

This branch also fixes an unrelated upstream bug: the "no main scene defined"
check used to fire a modal `NSAlert` on macOS even for cmdline tools that
legitimately have no main scene; the guard now respects `cmdline_tool`.

### F3 — editor automation RPC

Adds `--editor-rpc-port <port>`. While the editor runs, exposes a
line-delimited JSON-RPC 2.0 server on `127.0.0.1:<port>`. Polled from
`EditorNode::NOTIFICATION_PROCESS` so all dispatches run on the main thread —
no locking required.

```bash
godot --editor --path /my/project --editor-rpc-port 6664
# then, from anywhere:
echo '{"jsonrpc":"2.0","method":"ping","id":1}' | nc 127.0.0.1 6664
# -> {"jsonrpc":"2.0","result":"pong","id":1}
```

Method set:

*Read:*
- `ping` → `"pong"`
- `editor.get_current_scene_path` → string
- `editor.get_scene_tree` → nested node dump (same shape as F2)
- `editor.list_open_scenes` → array of `{path, root_name, root_class}`

*Write:*
- `editor.open_scene` `{path}` → opens scene as new active tab
- `editor.save_scene` → saves current scene to its existing path
- `editor.set_property` `{node_path, property, value}` → sets a property on
  a node in the active scene. JSON arrays of length 2/3/4 are coerced to
  Vector2/3/4 or Color when the target property is one of those types.
- `editor.add_node` `{parent_path, class, name?}` → instantiate a Node
  subclass, parent it, owner-link to the scene root so it persists on save.
  Returns `{name, class, path}`.
- `editor.delete_node` `{node_path}` → remove a node (and its descendants)
  from the scene. The scene root itself cannot be deleted.
- `editor.save_scene_as` `{path}` → save the current scene to a new path.
- `editor.get_selected_nodes` → array of node paths currently selected.
- `editor.select_nodes` `{paths, replace?}` → select the given nodes
  (skipping any path that doesn't resolve). `replace` defaults to true.

**Timing caveat**: `editor.open_scene` mounts the loaded scene into the
editor's SceneTree on the next frame, not synchronously. A client that
calls `open_scene` and `select_nodes` back-to-back in the same RPC poll
will see nodes silently skipped because they aren't yet `is_inside_tree()`.
Either wait one frame (any cheap intervening call works, e.g. another
`ping`), or verify selection with `get_selected_nodes` and retry.

*Execution control:*
- `editor.play` `{scene_path?}` → start a play session. `scene_path` accepts
  `"main"` (default — uses the project's `application/run/main_scene`),
  `"current"` (the active editor tab), or any `res://...` path. Returns
  whether a play session is now active.
- `editor.stop_playing` → stop any running play session.
- `editor.is_playing` → bool, whether a play session is currently active.
- `editor.get_recent_log` `{limit?: 200}` → array of recent Output-dock
  messages, each `{index, text, type, count}` where `type` is `std | error
  | warning | editor | std_rich`. While play mode is active, this includes
  the running game's prints and errors (forwarded via Godot's debugger
  protocol). Closes the debug loop: open → modify → save → play → read
  log → iterate.
- `editor.tail_log` `{since: int}` → cheap incremental polling primitive.
  Returns `{messages, next_since}` — only entries with `index >= since`.
  Pass the returned `next_since` back as the next `since`. If the buffer
  was cleared (server's count regressed below `since`), the call returns
  the full current buffer so the client can re-sync.

Further write methods (`move_node`, `reparent_node`, per-property
`get_property` for efficient single-value reads) are future work.

### F4 — MCP proxy

`tools/mcp_proxy/mcp_server.py` is a [Model Context Protocol](https://modelcontextprotocol.io/)
server that talks MCP over stdio and translates each tool call into an F3
RPC request. Lets MCP clients (Claude Code, etc.) drive the editor without
knowing the TCP protocol.

Setup:
```bash
cd tools/mcp_proxy
uv venv --python 3.12 .venv
uv pip install --python .venv/bin/python 'mcp>=1.0'
```

Register with Claude Code:
```bash
claude mcp add godot-fork \
  /abs/path/to/godot/tools/mcp_proxy/.venv/bin/python \
  /abs/path/to/godot/tools/mcp_proxy/mcp_server.py
```

Then start Godot with `--editor-rpc-port 6664` (or set `GODOT_RPC_PORT`).

Tools exposed:
- read: `godot_ping`, `godot_get_current_scene_path`, `godot_get_scene_tree`,
  `godot_list_open_scenes`, `godot_get_selected_nodes`, `godot_is_playing`,
  `godot_get_recent_log`, `godot_tail_log`
- write: `godot_open_scene`, `godot_save_scene`, `godot_save_scene_as`,
  `godot_set_property`, `godot_add_node`, `godot_delete_node`,
  `godot_select_nodes`
- execution: `godot_play`, `godot_stop_playing`

## Building

Standard Godot build, plus a few flags useful on macOS without the Vulkan SDK:

```bash
scons platform=macos target=editor dev_build=yes vulkan=no accesskit=no angle=no -j8
```

Binary lands at `bin/godot.macos.editor.dev.arm64`. First build ~4 min on an
M1 Max; incremental builds touching only `main.cpp` or `editor_*.cpp` are
~15-60s.

## Branch layout & upstream story

```
upstream/master              godotengine/godot tip
└─ master                    local mirror, leave clean
   └─ fork/main              integration trunk (unused so far)
      └─ feature/json-output            F1 — small, additive, plausible upstream PR
         └─ feature/scene-inspect       F2 — also plausible upstream PR
            └─ feature/editor-rpc       F3 — bigger surface, harder upstream sell
               └─ feature/mcp-server    F4 — out-of-tree, unlikely upstream
                  └─ feature/editor-rpc-writes  F5 — write methods on F3 (open/save/set_property)
```

Each `feature/*` branch is meant to rebase cleanly onto `upstream/master`. To
prepare an upstream PR for a single feature, rebase its branch on `master`
and push to a fresh branch on your upstream fork.

`fork.<user>.github.com:genno-whittlery/godot` is the working fork. Push with
`git push fork feature/<name>`.

## Known v2 work

- More write methods on F3 (`move_node`, `reparent_node`, per-property
  `get_property`, incremental log tailing with a since-cursor)
- F3 JSON-RPC ids round-trip as `1.0` instead of `1` — Godot's `Variant`→JSON
  serializer doesn't distinguish int from float
- F2 property dumps are verbose — a `--inspect-scene-compact` flag that
  only emits non-default values would be more useful for diffing
- F4 currently opens one TCP connection per call; persistent connection would
  matter once write methods land
- F5 `set_property` coercion currently handles Vector2/3/4 and Color from
  flat arrays; extending to Transform2D/3D, Rect2, AABB, NodePath etc. would
  make the API more complete

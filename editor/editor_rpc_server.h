/**************************************************************************/
/*  editor_rpc_server.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/io/stream_peer_tcp.h"
#include "core/io/tcp_server.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/variant.h"

/**
 * Line-delimited JSON-RPC 2.0 server, bound to 127.0.0.1:<port>, intended for
 * external tooling (editors, agents) to query and (later) drive the running
 * Godot editor. Polled from the editor's main-thread process notification,
 * so all method handlers run on the main thread without locking.
 *
 * MVP method set (read-only):
 *   - ping
 *   - editor.get_current_scene_path
 *   - editor.get_scene_tree
 *   - editor.list_open_scenes
 */
class EditorRpcServer {
public:
	EditorRpcServer(int p_port);
	~EditorRpcServer();

	bool is_listening() const { return listening; }
	int get_port() const { return port; }

	// Drives accept/read/dispatch. Safe to call every frame; cheap when idle.
	void poll();

private:
	int port = 0;
	bool listening = false;
	Ref<TCPServer> server;
	Vector<Ref<StreamPeerTCP>> clients;
	Vector<String> client_buffers;

	void process_client(int p_client_idx);
	void handle_request_line(int p_client_idx, const String &p_line);
	Variant dispatch(const String &p_method, const Variant &p_params, bool &r_has_error, int &r_error_code, String &r_error_message);
	void send_response(int p_client_idx, const Variant &p_result, const Variant &p_id);
	void send_error(int p_client_idx, int p_code, const String &p_message, const Variant &p_id);
	void send_raw_line(int p_client_idx, const String &p_line);
};

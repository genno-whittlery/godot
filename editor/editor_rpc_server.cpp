/**************************************************************************/
/*  editor_rpc_server.cpp                                                 */
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

#include "editor_rpc_server.h"

#include "core/io/ip.h"
#include "core/io/json.h"
#include "core/io/scene_inspector.h"
#include "core/string/print_string.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "editor/editor_data.h"
#include "editor/editor_node.h"
#include "scene/main/node.h"

EditorRpcServer::EditorRpcServer(int p_port) {
	port = p_port;
	server.instantiate();
	Error err = server->listen((uint16_t)p_port, IPAddress("127.0.0.1"));
	if (err == OK) {
		listening = true;
		print_line(vformat("Editor RPC: listening on 127.0.0.1:%d", port));
	} else {
		print_error(vformat("Editor RPC: failed to bind 127.0.0.1:%d (error %d).", port, (int)err));
	}
}

EditorRpcServer::~EditorRpcServer() {
	if (server.is_valid() && server->is_listening()) {
		server->stop();
	}
}

void EditorRpcServer::poll() {
	if (!listening || server.is_null()) {
		return;
	}

	while (server->is_connection_available()) {
		Ref<StreamPeerTCP> client = server->take_connection();
		if (client.is_valid()) {
			client->set_no_delay(true);
			clients.push_back(client);
			client_buffers.push_back(String());
		}
	}

	for (int i = clients.size() - 1; i >= 0; i--) {
		Ref<StreamPeerTCP> client = clients[i];
		if (client.is_null()) {
			clients.remove_at(i);
			client_buffers.remove_at(i);
			continue;
		}
		client->poll();
		StreamPeerSocket::Status status = client->get_status();
		if (status == StreamPeerSocket::STATUS_CONNECTED) {
			process_client(i);
		} else if (status != StreamPeerSocket::STATUS_CONNECTING) {
			// STATUS_NONE or STATUS_ERROR — drop the client.
			clients.remove_at(i);
			client_buffers.remove_at(i);
		}
	}
}

void EditorRpcServer::process_client(int p_client_idx) {
	Ref<StreamPeerTCP> client = clients[p_client_idx];
	int avail = client->get_available_bytes();
	if (avail <= 0) {
		return;
	}

	Vector<uint8_t> buf;
	buf.resize(avail);
	Error err = client->get_data(buf.ptrw(), avail);
	if (err != OK) {
		return;
	}
	String chunk = String::utf8((const char *)buf.ptr(), avail);

	String buffer = client_buffers[p_client_idx];
	buffer += chunk;

	while (true) {
		int newline_idx = buffer.find("\n");
		if (newline_idx < 0) {
			break;
		}
		String line = buffer.substr(0, newline_idx).strip_edges();
		buffer = buffer.substr(newline_idx + 1);
		if (!line.is_empty()) {
			handle_request_line(p_client_idx, line);
		}
	}
	client_buffers.write[p_client_idx] = buffer;
}

void EditorRpcServer::handle_request_line(int p_client_idx, const String &p_line) {
	JSON json;
	Error err = json.parse(p_line);
	if (err != OK) {
		send_error(p_client_idx, -32700, "Parse error: " + json.get_error_message(), Variant());
		return;
	}
	Variant data = json.get_data();
	if (data.get_type() != Variant::DICTIONARY) {
		send_error(p_client_idx, -32600, "Invalid Request: not a JSON object", Variant());
		return;
	}
	Dictionary req = data;
	Variant id = req.get("id", Variant());
	if (!req.has("method") || req["method"].get_type() != Variant::STRING) {
		send_error(p_client_idx, -32600, "Invalid Request: missing or non-string 'method'", id);
		return;
	}
	String method = req["method"];
	Variant params = req.get("params", Variant());

	bool has_error = false;
	int error_code = 0;
	String error_message;
	Variant result = dispatch(method, params, has_error, error_code, error_message);
	if (has_error) {
		send_error(p_client_idx, error_code, error_message, id);
	} else {
		send_response(p_client_idx, result, id);
	}
}

Variant EditorRpcServer::dispatch(const String &p_method, const Variant &p_params, bool &r_has_error, int &r_error_code, String &r_error_message) {
	r_has_error = false;

	if (p_method == "ping") {
		return "pong";
	}

	EditorNode *en = EditorNode::get_singleton();
	if (!en) {
		r_has_error = true;
		r_error_code = -32000;
		r_error_message = "Editor not available";
		return Variant();
	}

	if (p_method == "editor.get_current_scene_path") {
		Node *root = en->get_edited_scene();
		return root ? root->get_scene_file_path() : String();
	}

	if (p_method == "editor.get_scene_tree") {
		Node *root = en->get_edited_scene();
		if (!root) {
			return Variant();
		}
		return SceneInspector::dump_node(root);
	}

	if (p_method == "editor.list_open_scenes") {
		Vector<EditorData::EditedScene> scenes = en->get_editor_data().get_edited_scenes();
		Array arr;
		for (const EditorData::EditedScene &es : scenes) {
			Dictionary d;
			d["path"] = es.path;
			if (es.root) {
				d["root_name"] = String(es.root->get_name());
				d["root_class"] = es.root->get_class();
			} else {
				d["root_name"] = Variant();
				d["root_class"] = Variant();
			}
			arr.push_back(d);
		}
		return arr;
	}

	r_has_error = true;
	r_error_code = -32601;
	r_error_message = "Method not found: " + p_method;
	return Variant();
}

void EditorRpcServer::send_response(int p_client_idx, const Variant &p_result, const Variant &p_id) {
	Dictionary resp;
	resp["jsonrpc"] = "2.0";
	resp["result"] = p_result;
	resp["id"] = p_id;
	send_raw_line(p_client_idx, JSON::stringify(resp, "", false, true));
}

void EditorRpcServer::send_error(int p_client_idx, int p_code, const String &p_message, const Variant &p_id) {
	Dictionary resp;
	resp["jsonrpc"] = "2.0";
	Dictionary err;
	err["code"] = p_code;
	err["message"] = p_message;
	resp["error"] = err;
	resp["id"] = p_id;
	send_raw_line(p_client_idx, JSON::stringify(resp, "", false, true));
}

void EditorRpcServer::send_raw_line(int p_client_idx, const String &p_line) {
	if (p_client_idx < 0 || p_client_idx >= clients.size()) {
		return;
	}
	Ref<StreamPeerTCP> client = clients[p_client_idx];
	if (client.is_null()) {
		return;
	}
	CharString cs = (p_line + "\n").utf8();
	client->put_data((const uint8_t *)cs.get_data(), cs.length());
}

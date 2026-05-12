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
#include "core/object/class_db.h"
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

	if (p_method == "editor.open_scene") {
		if (p_params.get_type() != Variant::DICTIONARY) {
			r_has_error = true;
			r_error_code = -32602;
			r_error_message = "Invalid params: expected object with 'path'";
			return Variant();
		}
		Dictionary params = p_params;
		if (!params.has("path") || params["path"].get_type() != Variant::STRING) {
			r_has_error = true;
			r_error_code = -32602;
			r_error_message = "Invalid params: missing or non-string 'path'";
			return Variant();
		}
		String path = params["path"];
		Error err = en->load_scene(path);
		if (err != OK) {
			r_has_error = true;
			r_error_code = -32000;
			r_error_message = vformat("load_scene('%s') failed (error %d)", path, (int)err);
			return Variant();
		}
		Node *root = en->get_edited_scene();
		return root ? root->get_scene_file_path() : path;
	}

	if (p_method == "editor.save_scene") {
		Node *root = en->get_edited_scene();
		if (!root) {
			r_has_error = true;
			r_error_code = -32000;
			r_error_message = "No edited scene to save";
			return Variant();
		}
		String path = root->get_scene_file_path();
		if (path.is_empty()) {
			r_has_error = true;
			r_error_code = -32000;
			r_error_message = "Edited scene has no file path; use editor.save_scene_as (not yet implemented)";
			return Variant();
		}
		en->save_scene_to_path(path, false);
		return path;
	}

	if (p_method == "editor.set_property") {
		if (p_params.get_type() != Variant::DICTIONARY) {
			r_has_error = true;
			r_error_code = -32602;
			r_error_message = "Invalid params: expected object with 'node_path', 'property', 'value'";
			return Variant();
		}
		Dictionary params = p_params;
		if (!params.has("node_path") || params["node_path"].get_type() != Variant::STRING) {
			r_has_error = true;
			r_error_code = -32602;
			r_error_message = "Invalid params: missing or non-string 'node_path'";
			return Variant();
		}
		if (!params.has("property") || params["property"].get_type() != Variant::STRING) {
			r_has_error = true;
			r_error_code = -32602;
			r_error_message = "Invalid params: missing or non-string 'property'";
			return Variant();
		}
		Node *scene_root = en->get_edited_scene();
		if (!scene_root) {
			r_has_error = true;
			r_error_code = -32000;
			r_error_message = "No edited scene";
			return Variant();
		}
		String node_path = params["node_path"];
		Node *node = scene_root->get_node_or_null(NodePath(node_path));
		if (!node) {
			r_has_error = true;
			r_error_code = -32000;
			r_error_message = "Node not found: " + node_path;
			return Variant();
		}
		String property = params["property"];
		Variant value = params["value"];

		// Best-effort Array → Vector2/3/4/Color coercion. JSON has no native
		// vector types, so the conventional encoding is a flat array of numbers.
		if (value.get_type() == Variant::ARRAY) {
			Variant::Type prop_type = Variant::NIL;
			List<PropertyInfo> plist;
			node->get_property_list(&plist);
			for (const PropertyInfo &pi : plist) {
				if (pi.name == property) {
					prop_type = pi.type;
					break;
				}
			}
			Array arr = value;
			auto as_f = [&](int i) { return arr.size() > i ? (real_t)(double)arr[i] : (real_t)0.0; };
			switch (prop_type) {
				case Variant::VECTOR2:
					if (arr.size() == 2) {
						value = Vector2(as_f(0), as_f(1));
					}
					break;
				case Variant::VECTOR3:
					if (arr.size() == 3) {
						value = Vector3(as_f(0), as_f(1), as_f(2));
					}
					break;
				case Variant::VECTOR4:
					if (arr.size() == 4) {
						value = Vector4(as_f(0), as_f(1), as_f(2), as_f(3));
					}
					break;
				case Variant::COLOR:
					if (arr.size() == 3 || arr.size() == 4) {
						value = Color(as_f(0), as_f(1), as_f(2), arr.size() == 4 ? as_f(3) : (real_t)1.0);
					}
					break;
				default:
					break;
			}
		}

		bool valid = false;
		node->set(property, value, &valid);
		if (!valid) {
			r_has_error = true;
			r_error_code = -32000;
			r_error_message = vformat("Property '%s' on node '%s' (%s) could not be set (value type: %s)", property, node_path, node->get_class(), Variant::get_type_name(value.get_type()));
			return Variant();
		}
		return node->get(property);
	}

	if (p_method == "editor.add_node") {
		if (p_params.get_type() != Variant::DICTIONARY) {
			r_has_error = true;
			r_error_code = -32602;
			r_error_message = "Invalid params: expected object with 'parent_path', 'class' (and optional 'name')";
			return Variant();
		}
		Dictionary params = p_params;
		if (!params.has("parent_path") || params["parent_path"].get_type() != Variant::STRING) {
			r_has_error = true;
			r_error_code = -32602;
			r_error_message = "Invalid params: missing or non-string 'parent_path' (use '.' for the scene root)";
			return Variant();
		}
		if (!params.has("class") || params["class"].get_type() != Variant::STRING) {
			r_has_error = true;
			r_error_code = -32602;
			r_error_message = "Invalid params: missing or non-string 'class'";
			return Variant();
		}
		Node *scene_root = en->get_edited_scene();
		if (!scene_root) {
			r_has_error = true;
			r_error_code = -32000;
			r_error_message = "No edited scene";
			return Variant();
		}
		String parent_path = params["parent_path"];
		Node *parent = scene_root->get_node_or_null(NodePath(parent_path));
		if (!parent) {
			r_has_error = true;
			r_error_code = -32000;
			r_error_message = "Parent node not found: " + parent_path;
			return Variant();
		}
		String class_name = params["class"];
		if (!ClassDB::class_exists(class_name) || !ClassDB::is_parent_class(class_name, "Node")) {
			r_has_error = true;
			r_error_code = -32000;
			r_error_message = vformat("Class '%s' is not a known Node subclass", class_name);
			return Variant();
		}
		Object *obj = ClassDB::instantiate(class_name);
		Node *node = Object::cast_to<Node>(obj);
		if (!node) {
			if (obj) {
				memdelete(obj);
			}
			r_has_error = true;
			r_error_code = -32000;
			r_error_message = vformat("Failed to instantiate '%s'", class_name);
			return Variant();
		}
		String name = params.has("name") && params["name"].get_type() == Variant::STRING ? String(params["name"]) : class_name;
		node->set_name(name);
		parent->add_child(node, true /* readable name → disambiguates collisions */);
		node->set_owner(scene_root);

		Dictionary result;
		result["name"] = String(node->get_name()); // may differ from requested name if disambiguated
		result["class"] = node->get_class();
		result["path"] = String(scene_root->get_path_to(node));
		return result;
	}

	if (p_method == "editor.delete_node") {
		if (p_params.get_type() != Variant::DICTIONARY) {
			r_has_error = true;
			r_error_code = -32602;
			r_error_message = "Invalid params: expected object with 'node_path'";
			return Variant();
		}
		Dictionary params = p_params;
		if (!params.has("node_path") || params["node_path"].get_type() != Variant::STRING) {
			r_has_error = true;
			r_error_code = -32602;
			r_error_message = "Invalid params: missing or non-string 'node_path'";
			return Variant();
		}
		Node *scene_root = en->get_edited_scene();
		if (!scene_root) {
			r_has_error = true;
			r_error_code = -32000;
			r_error_message = "No edited scene";
			return Variant();
		}
		String node_path = params["node_path"];
		Node *node = scene_root->get_node_or_null(NodePath(node_path));
		if (!node) {
			r_has_error = true;
			r_error_code = -32000;
			r_error_message = "Node not found: " + node_path;
			return Variant();
		}
		if (node == scene_root) {
			r_has_error = true;
			r_error_code = -32000;
			r_error_message = "Cannot delete the scene root";
			return Variant();
		}
		Node *parent = node->get_parent();
		if (parent) {
			parent->remove_child(node);
		}
		node->queue_free();
		return true;
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

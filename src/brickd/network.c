/*
 * brickd
 * Copyright (C) 2012-2013 Matthias Bolte <matthias@tinkerforge.com>
 * Copyright (C) 2014 Olaf Lüke <olaf@tinkerforge.com>
 *
 * network.c: Network specific functions
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>
#ifndef _WIN32
	#include <netdb.h>
	#include <unistd.h>
#endif

#include "network.h"

#include "array.h"
#include "config.h"
#include "event.h"
#include "log.h"
#include "packet.h"
#include "socket.h"
#include "utils.h"

#define LOG_CATEGORY LOG_CATEGORY_NETWORK

static Array _clients;
static EventHandle _server_socket_normal    = INVALID_EVENT_HANDLE;
static EventHandle _server_socket_websocket = INVALID_EVENT_HANDLE;

static void network_handle_accept(void *opaque) {
	EventHandle client_socket;
	struct sockaddr_storage address;
	socklen_t length = sizeof(address);
	Client *client;
	SocketType type = (SocketType)opaque;
	EventHandle server_socket = type == SOCKET_TYPE_NORMAL ? _server_socket_normal : _server_socket_websocket;


	// accept new client socket
	if (socket_accept(server_socket, &client_socket,
	                  (struct sockaddr *)&address, &length) < 0) {
		if (!errno_interrupted()) {
			log_error("Could not accept new socket: %s (%d)",
			          get_errno_name(errno), errno);
		}

		return;
	}

	// enable non-blocking
	if (socket_set_non_blocking(client_socket, 1) < 0) {
		log_error("Could not enable non-blocking mode for socket (handle: %d): %s (%d)",
		          client_socket, get_errno_name(errno), errno);

		socket_destroy(client_socket);

		return;
	}

	// append to client array
	client = array_append(&_clients);

	if (client == NULL) {
		log_error("Could not append to client array: %s (%d)",
		          get_errno_name(errno), errno);

		socket_destroy(client_socket);

		return;
	}

	if (client_create(client, client_socket, type, (struct sockaddr *)&address, length) < 0) {
		array_remove(&_clients, _clients.count - 1, NULL);
		socket_destroy(client_socket);

		return;
	}

	log_info("Added new client (socket: %d, peer: %s)",
	         client->socket, client->peer);
}

static const char *network_get_address_family_name(int family, int report_dual_stack) {
	switch (family) {
	case AF_INET:
		return "IPv4";

	case AF_INET6:
		if (report_dual_stack && config_get_listen_dual_stack()) {
			return "IPv6 dual-stack";
		} else {
			return "IPv6";
		}

	default:
		return "<unknown>";
	}
}

int network_init_port(uint16_t port, SocketType type) {
	int phase = 0;
	const char *listen_address = config_get_listen_address();
	struct addrinfo *resolved_address = NULL;
	EventHandle *server_socket = type == SOCKET_TYPE_NORMAL ? &_server_socket_normal : &_server_socket_websocket;

	log_debug("Initializing network subsystem (type: %d, port: %u)", type, port);

	// resolve listen address
	// FIXME: bind to all returned addresses, instead of just the first one.
	//        requires special handling if IPv4 and IPv6 addresses are returned
	//        and dual-stack mode is enabled
	resolved_address = socket_hostname_to_address(listen_address, port);

	if (resolved_address == NULL) {
		log_error("Could not resolve listen address '%s' (port: %u): %s (%d)",
		          listen_address, port, get_errno_name(errno), errno);

		goto cleanup;
	}

	phase = 1;

	// create socket
	if (socket_create(server_socket, resolved_address->ai_family,
	                  resolved_address->ai_socktype, resolved_address->ai_protocol) < 0) {
		log_error("Could not create %s server socket: %s (%d)",
		          network_get_address_family_name(resolved_address->ai_family, 0),
		          get_errno_name(errno), errno);

		goto cleanup;
	}

	phase = 2;

	if (resolved_address->ai_family == AF_INET6) {
		if (socket_set_dual_stack(*server_socket, config_get_listen_dual_stack()) < 0) {
			log_error("Could not %s dual-stack mode for IPv6 server socket: %s (%d)",
			          config_get_listen_dual_stack() ? "enable" : "disable",
			          get_errno_name(errno), errno);

			goto cleanup;
		}
	}

#ifndef _WIN32
	// on Unix the SO_REUSEADDR socket option allows to rebind sockets in
	// CLOSE-WAIT state. this is a desired effect. on Windows SO_REUSEADDR
	// allows to rebind sockets in any state. this is dangerous. therefore,
	// don't set SO_REUSEADDR on Windows. sockets can be rebound in CLOSE-WAIT
	// state on Windows by default.
	if (socket_set_address_reuse(*server_socket, 1) < 0) {
		log_error("Could not enable address-reuse mode for server socket: %s (%d)",
		          get_errno_name(errno), errno);

		goto cleanup;
	}
#endif

	// bind socket and start to listen
	if (socket_bind(*server_socket, resolved_address->ai_addr,
	                resolved_address->ai_addrlen) < 0) {
		log_error("Could not bind %s server socket to '%s' on port %u: %s (%d)",
		          network_get_address_family_name(resolved_address->ai_family, 1),
		          listen_address, port, get_errno_name(errno), errno);

		goto cleanup;
	}

	if (socket_listen(*server_socket, 10) < 0) {
		log_error("Could not listen to %s server socket bound to '%s' on port %u: %s (%d)",
		          network_get_address_family_name(resolved_address->ai_family, 1),
		          listen_address, port, get_errno_name(errno), errno);

		goto cleanup;
	}

	log_debug("Started listening to '%s' (%s) on port %u",
	          listen_address,
	          network_get_address_family_name(resolved_address->ai_family, 1),
	          port);

	if (socket_set_non_blocking(*server_socket, 1) < 0) {
		log_error("Could not enable non-blocking mode for server socket: %s (%d)",
		          get_errno_name(errno), errno);

		goto cleanup;
	}

	if (event_add_source(*server_socket, EVENT_SOURCE_TYPE_GENERIC, EVENT_READ,
	                     network_handle_accept, (void*)type) < 0) {
		goto cleanup;
	}

	phase = 3;

	freeaddrinfo(resolved_address);

cleanup:
	switch (phase) { // no breaks, all cases fall through intentionally
	case 2:
		socket_destroy(*server_socket);

	case 1:
		freeaddrinfo(resolved_address);

	default:
		break;
	}

	return phase == 3 ? 0 : -1;
}

int network_init(void) {
	uint16_t port_socket = config_get_listen_port();
	uint16_t port_websocket = config_get_listen_websocket_port();
	int ret_socket;
	int ret_websocket;

	// the Client struct is not relocatable, because it is passed by reference
	// as opaque parameter to the event subsystem
	if (array_create(&_clients, 32, sizeof(Client), 0) < 0) {
		log_error("Could not create client array: %s (%d)",
		          get_errno_name(errno), errno);

		array_destroy(&_clients, (FreeFunction)client_destroy);

		return -1;
	}

	ret_socket = network_init_port(port_socket, SOCKET_TYPE_NORMAL);
	ret_websocket = network_init_port(port_websocket, SOCKET_TYPE_WEBSOCKET);

	if (ret_socket < 0 && ret_websocket < 0) {
		return -1;
	}

	return 0;
}

void network_exit(void) {
	log_debug("Shutting down network subsystem");

	network_cleanup_clients();

	array_destroy(&_clients, (FreeFunction)client_destroy);

	event_remove_source(_server_socket_normal, EVENT_SOURCE_TYPE_GENERIC);
	event_remove_source(_server_socket_websocket, EVENT_SOURCE_TYPE_GENERIC);

	socket_destroy(_server_socket_normal);
	socket_destroy(_server_socket_websocket);
}

// remove clients that got marked as disconnected
void network_cleanup_clients(void) {
	int i;
	Client *client;

	// iterate backwards for simpler index handling
	for (i = _clients.count - 1; i >= 0; --i) {
		client = array_get(&_clients, i);

		if (client->disconnected) {
			log_debug("Removing disconnected client (socket: %d, peer: %s)",
			          client->socket, client->peer);

			array_remove(&_clients, i, (FreeFunction)client_destroy);
		}
	}
}

void network_dispatch_response(Packet *response) {
	char base58[MAX_BASE58_STR_SIZE];
	int i;
	Client *client;
	int rc;
	int dispatched = 0;

	if (_clients.count == 0) {
		if (packet_header_get_sequence_number(&response->header) == 0) {
			log_debug("No clients connected, dropping %scallback (U: %s, L: %u, F: %u)",
			          packet_get_callback_type(response),
			          base58_encode(base58, uint32_from_le(response->header.uid)),
			          response->header.length,
			          response->header.function_id);
		} else {
			log_debug("No clients connected, dropping response (U: %s, L: %u, F: %u, S: %u, E: %u)",
			          base58_encode(base58, uint32_from_le(response->header.uid)),
			          response->header.length,
			          response->header.function_id,
			          packet_header_get_sequence_number(&response->header),
			          packet_header_get_error_code(&response->header));
		}

		return;
	}

	if (packet_header_get_sequence_number(&response->header) == 0) {
		log_debug("Broadcasting %scallback (U: %s, L: %u, F: %u) to %d client(s)",
		          packet_get_callback_type(response),
		          base58_encode(base58, uint32_from_le(response->header.uid)),
		          response->header.length,
		          response->header.function_id,
		          _clients.count);

		for (i = 0; i < _clients.count; ++i) {
			client = array_get(&_clients, i);

			client_dispatch_response(client, response, 1);
		}
	} else {
		log_debug("Dispatching response (U: %s, L: %u, F: %u, S: %u, E: %u) to %d client(s)",
		          base58_encode(base58, uint32_from_le(response->header.uid)),
		          response->header.length,
		          response->header.function_id,
		          packet_header_get_sequence_number(&response->header),
		          packet_header_get_error_code(&response->header),
		          _clients.count);

		for (i = 0; i < _clients.count; ++i) {
			client = array_get(&_clients, i);

			rc = client_dispatch_response(client, response, 0);

			if (rc < 0) {
				continue;
			} else if (rc > 0) {
				dispatched = 1;
			}
		}

		if (dispatched) {
			return;
		}

		log_warn("Broadcasting response because no client has a matching pending request");

		for (i = 0; i < _clients.count; ++i) {
			client = array_get(&_clients, i);

			client_dispatch_response(client, response, 1);
		}
	}
}

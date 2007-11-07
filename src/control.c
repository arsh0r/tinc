/*
    control.c -- Control socket handling.
    Copyright (C) 2007 Guus Sliepen <guus@tinc-vpn.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

    $Id$
*/

#include <sys/un.h>

#include "system.h"
#include "conf.h"
#include "control.h"
#include "control_common.h"
#include "logger.h"
#include "xalloc.h"

static int control_socket = -1;
static struct event control_event;
static splay_tree_t *control_socket_tree;
extern char *controlsocketname;

static void handle_control_data(struct bufferevent *event, void *data) {
	tinc_ctl_request_t req;
	size_t size;
	tinc_ctl_request_t res;
	struct evbuffer *res_data = NULL;

	if(EVBUFFER_LENGTH(event->input) < sizeof(tinc_ctl_request_t))
		return;

	/* Copy the structure to ensure alignment */
	memcpy(&req, EVBUFFER_DATA(event->input), sizeof(tinc_ctl_request_t));

	if(EVBUFFER_LENGTH(event->input) < req.length)
		return;

	if(req.length < sizeof(tinc_ctl_request_t))
		goto failure;

	memset(&res, 0, sizeof res);
	res.type = req.type;
	res.id = req.id;

	res_data = evbuffer_new();
	if (res_data == NULL) {
		res.res_errno = ENOMEM;
		goto respond;
	}

	if(req.type == REQ_STOP) {
		logger(LOG_NOTICE, _("Got stop command"));
		event_loopexit(NULL);
		goto respond;
	}

	logger(LOG_DEBUG, _("Malformed control command received"));
	res.res_errno = EINVAL;

respond:
	res.length = (sizeof res)
				 + ((res_data == NULL) ? 0 : EVBUFFER_LENGTH(res_data));
	evbuffer_drain(event->input, req.length);
	if(bufferevent_write(event, &res, sizeof res) == -1)
		goto failure;
	if(res_data != NULL) {
		if(bufferevent_write_buffer(event, res_data) == -1)
			goto failure;
		evbuffer_free(res_data);
	}
	return;

failure:
	logger(LOG_INFO, _("Closing control socket on error"));
	evbuffer_free(res_data);
	close(event->ev_read.ev_fd);
	splay_delete(control_socket_tree, event);
}

static void handle_control_error(struct bufferevent *event, short what, void *data) {
	if(what & EVBUFFER_EOF)
		logger(LOG_DEBUG, _("Control socket connection closed by peer"));
	else
		logger(LOG_DEBUG, _("Error while reading from control socket: %s"), strerror(errno));

	close(event->ev_read.ev_fd);
	splay_delete(control_socket_tree, event);
}

static void handle_new_control_socket(int fd, short events, void *data) {
	int newfd;
	struct bufferevent *ev;
	tinc_ctl_greeting_t greeting;

	newfd = accept(fd, NULL, NULL);

	if(newfd < 0) {
		logger(LOG_ERR, _("Accepting a new connection failed: %s"), strerror(errno));
		event_del(&control_event);
		return;
	}

	ev = bufferevent_new(newfd, handle_control_data, NULL, handle_control_error, NULL);
	if(!ev) {
		logger(LOG_ERR, _("Could not create bufferevent for new control connection: %s"), strerror(errno));
		close(newfd);
		return;
	}

	memset(&greeting, 0, sizeof greeting);
	greeting.version = TINC_CTL_VERSION_CURRENT;
	if(bufferevent_write(ev, &greeting, sizeof greeting) == -1) {
		logger(LOG_ERR,
			   _("Cannot send greeting for new control connection: %s"),
			   strerror(errno));
		bufferevent_free(ev);
		close(newfd);
		return;
	}

	bufferevent_enable(ev, EV_READ);
	splay_insert(control_socket_tree, ev);

	logger(LOG_DEBUG, _("Control socket connection accepted"));
}

static int control_compare(const struct event *a, const struct event *b) {
	return a < b ? -1 : a > b ? 1 : 0;
}

bool init_control() {
	int result;
	struct sockaddr_un addr;

	if(strlen(controlsocketname) >= sizeof addr.sun_path) {
		logger(LOG_ERR, _("Control socket filename too long!"));
		return false;
	}

	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, controlsocketname, sizeof addr.sun_path - 1);

	control_socket = socket(PF_UNIX, SOCK_STREAM, 0);

	if(control_socket < 0) {
		logger(LOG_ERR, _("Creating UNIX socket failed: %s"), strerror(errno));
		return false;
	}

	//unlink(controlsocketname);
	result = bind(control_socket, (struct sockaddr *)&addr, sizeof addr);
	
	if(result < 0 && errno == EADDRINUSE) {
		result = connect(control_socket, (struct sockaddr *)&addr, sizeof addr);
		if(result < 0) {
			logger(LOG_WARNING, _("Removing old control socket."));
			unlink(controlsocketname);
			result = bind(control_socket, (struct sockaddr *)&addr, sizeof addr);
		} else {
			close(control_socket);
			if(netname)
				logger(LOG_ERR, _("Another tincd is already running for net `%s'."), netname);
			else
				logger(LOG_ERR, _("Another tincd is already running."));
			return false;
		}
	}

	if(result < 0) {
		logger(LOG_ERR, _("Can't bind to %s: %s\n"), controlsocketname, strerror(errno));
		close(control_socket);
		return false;
	}

	if(listen(control_socket, 3) < 0) {
		logger(LOG_ERR, _("Can't listen on %s: %s\n"), controlsocketname, strerror(errno));
		close(control_socket);
		return false;
	}

	control_socket_tree = splay_alloc_tree((splay_compare_t)control_compare, (splay_action_t)bufferevent_free);

	event_set(&control_event, control_socket, EV_READ | EV_PERSIST, handle_new_control_socket, NULL);
	event_add(&control_event, NULL);

	return true;
}

void exit_control() {
	event_del(&control_event);
	close(control_socket);
	unlink(controlsocketname);
}
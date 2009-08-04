/*
 * uhub - A tiny ADC p2p connection hub
 * Copyright (C) 2007-2009, Jan Vidar Krey
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "uhub.h"

extern struct hub_info* g_hub;

static inline int net_con_flag_get(struct net_connection* con, unsigned int flag)
{
    return con->flags & flag;
}

static inline void net_con_flag_set(struct net_connection* con, unsigned int flag)
{
    con->flags |= flag;
}

static inline void net_con_flag_unset(struct net_connection* con, unsigned int flag)
{
    con->flags &= ~flag;
}

static inline int net_con_convert_to_libevent_mask(int ev)
{
	int events = 0;
	if (ev & NET_EVENT_READ)  events |= EV_READ;
	if (ev & NET_EVENT_WRITE) events |= EV_WRITE;
	return events;
}

static inline int net_con_convert_from_libevent_mask(int ev)
{
	int events = 0;
	if (ev & EV_TIMEOUT)    events |= NET_EVENT_TIMEOUT;
	if (ev & EV_READ)       events |= NET_EVENT_READ;
	if (ev & EV_WRITE)      events |= NET_EVENT_WRITE;
	return events;
}


static void net_con_event(int fd, short ev, void *arg)
{
	struct net_connection* con = (struct net_connection*) arg;
	int events = net_con_convert_from_libevent_mask(ev);

#ifdef SSL_SUPPORT
	if (!con->ssl)
	{
#endif
		con->callback(con, events, con->ptr);
#ifdef SSL_SUPPORT
	}
	else
	{
#ifdef NETWORK_DUMP_DEBUG
		LOG_PROTO("net_con_event: events=%d, con=%p", ev, con);
#endif
		if (ev & (EV_READ | EV_WRITE))
		{
			if (net_con_flag_get(con, NET_WANT_SSL_ACCEPT))
			{
				if (net_con_ssl_accept(con) < 0)
					con->callback(con, NET_EVENT_SOCKERROR, con->ptr);
			}
			else if (net_con_flag_get(con, NET_WANT_SSL_CONNECT))
			{
				if (net_con_ssl_connect(con) < 0)
					con->callback(con, NET_EVENT_SOCKERROR, con->ptr);
			}
			else if (ev == EV_READ && net_con_flag_get(con, NET_WANT_SSL_READ))
			{
				con->callback(con, NET_EVENT_WRITE, con->ptr);
			}
			else if (ev == EV_WRITE && net_con_flag_get(con, NET_WANT_SSL_WRITE))
			{
				con->callback(con, events & NET_EVENT_READ, con->ptr);
			}
			else
			{
				con->callback(con, events, con->ptr);
			}
		}
		else
		{
			con->callback(con, events, con->ptr);
		}
	}
#endif
}

void net_con_initialize(struct net_connection* con, int sd, struct ip_addr_encap* addr, net_connection_cb callback, const void* ptr, int ev)
{
	int events = net_con_convert_to_libevent_mask(ev);
	if (ev & NET_EVENT_READ)  net_con_flag_set(con, NET_WANT_READ);
	if (ev & NET_EVENT_WRITE) net_con_flag_set(con, NET_WANT_WRITE);

	con->sd = sd;
	con->ptr = (void*) ptr;
	con->callback = callback;
	con->last_send = time(0);
	con->last_recv = con->last_send;

	/** IP address of peer */
	if (addr)
	{
		memcpy(&con->ipaddr, addr, sizeof(struct ip_addr_encap));
	}

	event_set(&con->event, con->sd, events | EV_PERSIST, net_con_event, con);
	event_add(&con->event, 0);

	net_set_nonblocking(sd, 1);
	net_set_nosigpipe(sd, 1);

#ifdef SSL_SUPPORT
	con->ssl = NULL;
	con->write_len = 0;
#endif
}

void net_con_update(struct net_connection* con, int ev)
{
	int events = net_con_convert_to_libevent_mask(ev);
	if (events & EV_READ)  net_con_flag_set(con, NET_WANT_READ);
	if (events & EV_WRITE) net_con_flag_set(con, NET_WANT_WRITE);

	if (con->sd == -1 || event_pending(&con->event, EV_READ | EV_WRITE, 0) == events)
		return;

	event_del(&con->event);
	event_set(&con->event, con->sd, events | EV_PERSIST, net_con_event, con);
	event_add(&con->event, 0);
}

void net_con_close(struct net_connection* con)
{
	if (event_pending(&con->event, EV_READ | EV_WRITE, 0))
	{
		event_del(&con->event);
	}

	net_con_clear_timeout(con);
	net_close(con->sd);
	con->sd = -1;
}

#ifdef SSL_SUPPORT
static int handle_openssl_error(struct net_connection* con, int ret)
{
	int error = SSL_get_error(con->ssl, ret);
	switch (error)
	{
		case SSL_ERROR_ZERO_RETURN:
			LOG_PROTO("SSL_get_error: ret=%d, error=%d: SSL_ERROR_ZERO_RETURN", ret, error);
			return -1;

		case SSL_ERROR_WANT_READ:
			LOG_PROTO("SSL_get_error: ret=%d, error=%d: SSL_ERROR_WANT_READ", ret, error);
			net_con_update(con, EV_READ);
			net_con_flag_set(con, NET_WANT_SSL_READ);
			return 0;

		case SSL_ERROR_WANT_WRITE:
			LOG_PROTO("SSL_get_error: ret=%d, error=%d: SSL_ERROR_WANT_WRITE", ret, error);
			net_con_update(con, EV_READ | EV_WRITE);
			net_con_flag_set(con, NET_WANT_SSL_WRITE);
			return 0;

		case SSL_ERROR_WANT_CONNECT:
			LOG_PROTO("SSL_get_error: ret=%d, error=%d: SSL_ERROR_WANT_CONNECT", ret, error);
			net_con_update(con, EV_READ | EV_WRITE);
			net_con_flag_set(con, NET_WANT_SSL_CONNECT);
			return 0;

		case SSL_ERROR_WANT_ACCEPT:
			LOG_PROTO("SSL_get_error: ret=%d, error=%d: SSL_ERROR_WANT_ACCEPT", ret, error);
			net_con_update(con, EV_READ | EV_WRITE);
			net_con_flag_set(con, NET_WANT_SSL_ACCEPT);
			return 0;

		case SSL_ERROR_WANT_X509_LOOKUP:
			LOG_PROTO("SSL_get_error: ret=%d, error=%d: SSL_ERROR_WANT_X509_LOOKUP", ret, error);
			return 0;

		case SSL_ERROR_SYSCALL:
			LOG_PROTO("SSL_get_error: ret=%d, error=%d: SSL_ERROR_SYSCALL", ret, error);
			/* if ret == 0, connection closed, if ret == -1, check with errno */
			if (ret == 0)
				return -1;
			else
				return -net_error();

		case SSL_ERROR_SSL:
			LOG_PROTO("SSL_get_error: ret=%d, error=%d: SSL_ERROR_SSL", ret, error);
			/* internal openssl error */
			return -1;
	}

	return -1;
}
#endif


ssize_t net_con_send(struct net_connection* con, const void* buf, size_t len)
{
#ifdef SSL_SUPPORT
	if (!con->ssl)
	{
#endif
		int ret = net_send(con->sd, buf, len, UHUB_SEND_SIGNAL);
#ifdef NETWORK_DUMP_DEBUG
		LOG_PROTO("net_send: ret=%d", ret);
#endif
		if (ret > 0)
		{
			con->last_send = time(0);
		}
		else if (ret == -1 && (net_error() == EWOULDBLOCK || net_error() == EINTR))
		{
			return 0;
		}
		else
		{
			return -1;
		}
		return ret;
#ifdef SSL_SUPPORT
	}
	else
	{
		if (net_con_flag_get(con, NET_WANT_SSL_READ) && con->write_len)
			len = con->write_len;

		int ret = SSL_write(con->ssl, buf, len);
#ifdef NETWORK_DUMP_DEBUG
		LOG_PROTO("net_send: ret=%d", ret);
#endif
		if (ret > 0)
		{
			con->last_send = time(0);
			net_con_flag_unset(con, NET_WANT_SSL_READ);
			con->write_len = 0;
		}
		else
		{
			con->write_len = len;
			return handle_openssl_error(con, ret);
		}
		return ret;
	}
#endif


}

ssize_t net_con_recv(struct net_connection* con, void* buf, size_t len)
{
#ifdef SSL_SUPPORT
	if (!con->ssl)
	{
#endif
		int ret = net_recv(con->sd, buf, len, 0);
#ifdef NETWORK_DUMP_DEBUG
		LOG_PROTO("net_recv: ret=%d", ret);
#endif
		if (ret > 0)
		{
			con->last_recv = time(0);
		}
		else if (ret == -1 && (net_error() == EWOULDBLOCK || net_error() == EINTR))
		{
			return 0;
		}
		else
		{
			return -1;
		}
		return ret;
#ifdef SSL_SUPPORT
	}
	else
	{
		int ret = SSL_read(con->ssl, buf, len);
#ifdef NETWORK_DUMP_DEBUG
		LOG_PROTO("net_recv: ret=%d", ret);
#endif
		if (ret > 0)
		{
			con->last_recv = time(0);
			net_con_flag_unset(con, NET_WANT_SSL_WRITE);
		}
		else
		{
			return handle_openssl_error(con, ret);
		}
		return ret;
	}
#endif
}

void net_con_set_timeout(struct net_connection* con, int seconds)
{
	struct timeval timeout = { seconds, 0 };
	evtimer_set(&con->timeout, net_con_event, con);
	evtimer_add(&con->timeout, &timeout);
}

void net_con_clear_timeout(struct net_connection* con)
{
	if (evtimer_initialized(&con->timeout) && evtimer_pending(&con->timeout, 0))
	{
		evtimer_del(&con->timeout);
	}
}


#ifdef SSL_SUPPORT
ssize_t net_con_ssl_accept(struct net_connection* con)
{
	net_con_flag_set(con, NET_WANT_SSL_ACCEPT);
	ssize_t ret = SSL_accept(con->ssl);
#ifdef NETWORK_DUMP_DEBUG
	LOG_PROTO("SSL_accept() ret=%d", ret);
#endif
	if (ret > 0)
	{
		net_con_flag_unset(con, NET_WANT_SSL_ACCEPT);
		net_con_flag_unset(con, NET_WANT_SSL_READ);
	}
	else
	{
		return handle_openssl_error(con, ret);
	}
	return ret;
}

ssize_t net_con_ssl_connect(struct net_connection* con)
{
	net_con_flag_set(con, NET_WANT_SSL_CONNECT);
	ssize_t ret = SSL_connect(con->ssl);
#ifdef NETWORK_DUMP_DEBUG
	LOG_PROTO("SSL_connect() ret=%d", ret);
#endif
	if (ret > 0)
	{
		net_con_flag_unset(con, NET_WANT_SSL_CONNECT);
		net_con_flag_unset(con, NET_WANT_SSL_WRITE);
	}
	else
	{
		return handle_openssl_error(con, ret);
	}
	return ret;
}

ssize_t net_con_ssl_handshake(struct net_connection* con, int ssl_mode)
{
	if (ssl_mode == NET_CON_SSL_MODE_SERVER)
	{
		con->ssl = SSL_new(g_hub->ssl_ctx);
		SSL_set_fd(con->ssl, con->sd);
		return net_con_ssl_accept(con);
	}
	else
	{
		con->ssl = SSL_new(SSL_CTX_new(TLSv1_method()));
		SSL_set_fd(con->ssl, con->sd);
		return net_con_ssl_connect(con);
	}
}
#endif /* SSL_SUPPORT */


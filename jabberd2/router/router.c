/*
 * jabberd - Jabber Open Source Server
 * Copyright (c) 2002 Jeremie Miller, Thomas Muldowney,
 *                    Ryan Eatmon, Robert Norris
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA02111-1307USA
 */

#include "router.h"

#define MAX_JID 3072    // node(1023) + '@'(1) + domain(1023) + '/'(1) + resource(1023) + '\0'(1)
#define MAX_MESSAGE 65535
#define SECS_PER_DAY 86400
#define BYTES_PER_MEG 1048576

/** info for broadcasts */
typedef struct broadcast_st {
    router_t      r;
    component_t   src;
    nad_t         nad;
} *broadcast_t;

/** broadcast a packet */
static void _router_broadcast(xht routes, const char *key, void *val, void *arg) {
    broadcast_t bc = (broadcast_t) arg;
    component_t comp = (component_t) val;

    /* I don't care about myself or the elderly (!?) */
    if(comp == bc->src || comp->legacy)
        return;

    sx_nad_write(comp->s, nad_copy(bc->nad));
}

/** domain advertisement */
static void _router_advertise(router_t r, char *domain, component_t src, int unavail) {
    struct broadcast_st bc;
    int ns;

    log_debug(ZONE, "advertising %s to all routes (unavail=%d)", domain, unavail);

    bc.r = r;
    bc.src = src;

    /* create a new packet */
    bc.nad = nad_new(src->s->nad_cache);
    ns = nad_add_namespace(bc.nad, uri_COMPONENT, NULL);
    nad_append_elem(bc.nad, ns, "presence", 0);
    nad_append_attr(bc.nad, -1, "from", domain);
    if(unavail)
        nad_append_attr(bc.nad, -1, "type", "unavailable");

    xhash_walk(r->routes, _router_broadcast, (void *) &bc);

    nad_free(bc.nad);
}

/** tell a component about all the others */
static void _router_advertise_reverse(xht routes, const char *key, void *val, void *arg) {
    component_t dest = (component_t) arg, comp = (component_t) val;
    int ns;
    nad_t nad;

    /* don't tell me about myself */
    if(comp == dest)
        return;

    log_debug(ZONE, "informing component about %s", key);

    /* create a new packet */
    nad = nad_new(dest->s->nad_cache);
    ns = nad_add_namespace(nad, uri_COMPONENT, NULL);
    nad_append_elem(nad, ns, "presence", 0);
    nad_append_attr(nad, -1, "from", (char *) key);

    sx_nad_write(dest->s, nad);
}

static void _router_process_handshake(component_t comp, nad_t nad) {
    char *hash;
    int hashlen;

    /* must have a hash as cdata */
    if(NAD_CDATA_L(nad, 0) != 40) {
        log_debug(ZONE, "handshake isn't long enough to be a sha1 hash");
        sx_error(comp->s, stream_err_NOT_AUTHORIZED, "handshake isn't long enough to be a sha1 hash");
        sx_close(comp->s);

        nad_free(nad);
        return;
    }

    /* make room for shahash_r to work .. needs at least 41 chars */
    hashlen = strlen(comp->s->id) + strlen(comp->r->local_secret) + 1;
    if(hashlen < 41)
        hashlen = 41;

    /* build the creds and hash them */
    hash = (char *) malloc(sizeof(char) * hashlen);
    sprintf(hash, "%s%s", comp->s->id, comp->r->local_secret);
    shahash_r(hash, hash);

    /* check */
    log_debug(ZONE, "checking their hash %.*s against our hash %s", 40, NAD_CDATA(nad, 0), hash);

    if(strncmp(hash, NAD_CDATA(nad, 0), 40) == 0) {
        log_debug(ZONE, "handshake succeeded");

        free(hash);

        /* respond */
        nad->elems[0].icdata = nad->elems[0].itail = -1;
        nad->elems[0].lcdata = nad->elems[0].ltail = 0;
        sx_nad_write(comp->s, nad);

        sx_auth(comp->s, "handshake", comp->s->req_to);
        
        return;
    }
    
    log_debug(ZONE, "auth failed");

    free(hash);

    /* failed, let them know */
    sx_error(comp->s, stream_err_NOT_AUTHORIZED, "hash didn't match, auth failed");
    sx_close(comp->s);

    nad_free(nad);
}

static void _router_process_bind(component_t comp, nad_t nad) {
    int attr;
    jid_t name;
    alias_t alias;
    char *user, *c;

    attr = nad_find_attr(nad, 0, -1, "name", NULL);
    if(attr < 0 || (name = jid_new(comp->r->pc, NAD_AVAL(nad, attr), NAD_AVAL_L(nad, attr))) == NULL) {
        log_debug(ZONE, "no or invalid 'name' on bind packet, bouncing");
        nad_set_attr(nad, 0, -1, "error", "400", 3);
        sx_nad_write(comp->s, nad);
        return;
    }

    user = strdup(comp->s->auth_id);
    c = strchr(user, '@');
    if(c != NULL) *c = '\0';

    if(strcmp(user, name->domain) != 0 && !aci_check(comp->r->aci, "bind", user)) {
        log_write(comp->r->log, LOG_NOTICE, "[%s, port=%d] tried to bind '%s', but their username (%s) is not permitted to bind other names", comp->ip, comp->port, name->domain, user);
        nad_set_attr(nad, 0, -1, "name", NULL, 0);
        nad_set_attr(nad, 0, -1, "error", "403", 3);
        sx_nad_write(comp->s, nad);
        jid_free(name);
        free(user);
        return;
    }

    if(xhash_get(comp->r->routes, name->domain) != NULL) {
        log_write(comp->r->log, LOG_NOTICE, "[%s, port=%d] tried to bind '%s', but it's already bound", comp->ip, comp->port, name->domain);
        nad_set_attr(nad, 0, -1, "name", NULL, 0);
        nad_set_attr(nad, 0, -1, "error", "409", 3);
        sx_nad_write(comp->s, nad);
        jid_free(name);
        free(user);
        return;
    }

    for(alias = comp->r->aliases; alias != NULL; alias = alias->next)
        if(strcmp(alias->name, name->domain) == 0) {
            log_write(comp->r->log, LOG_NOTICE, "[%s, port=%d] tried to bind '%s', but that name is aliased", comp->ip, comp->port);
            nad_set_attr(nad, 0, -1, "name", NULL, 0);
            nad_set_attr(nad, 0, -1, "error", "409", 3);
            sx_nad_write(comp->s, nad);
            jid_free(name);
            free(user);
            return;
        }

    /* default route */
    if(nad_find_elem(nad, 0, NAD_ENS(nad, 0), "default", 1) >= 0) {
        if(!aci_check(comp->r->aci, "default-route", user)) {
            log_write(comp->r->log, LOG_NOTICE, "[%s, port=%d] tried to bind '%s' as the default route, but their username (%s) is not permitted to set a default route", comp->ip, comp->port, name->domain, user);
            nad_set_attr(nad, 0, -1, "name", NULL, 0);
            nad_set_attr(nad, 0, -1, "error", "403", 3);
            sx_nad_write(comp->s, nad);
            jid_free(name);
            free(user);
            return;
        }

        if(comp->r->default_route != NULL) {
            log_write(comp->r->log, LOG_NOTICE, "[%s, port=%d] tried to bind '%s' as the default route, but one already exists", comp->ip, comp->port, name->domain);
            nad_set_attr(nad, 0, -1, "name", NULL, 0);
            nad_set_attr(nad, 0, -1, "error", "409", 3);
            sx_nad_write(comp->s, nad);
            jid_free(name);
            return;
        }

        log_write(comp->r->log, LOG_NOTICE, "[%s] set as default route", name->domain);

        comp->r->default_route = strdup(name->domain);
    }

    /* log sinks */
    if(nad_find_elem(nad, 0, NAD_ENS(nad, 0), "log", 1) >= 0) {
        if(!aci_check(comp->r->aci, "log", user)) {
            log_write(comp->r->log, LOG_NOTICE, "[%s, port=%d] tried to bind '%s' as a log sink, but their username (%s) is not permitted to do this", comp->ip, comp->port, name->domain, user);
            nad_set_attr(nad, 0, -1, "name", NULL, 0);
            nad_set_attr(nad, 0, -1, "error", "403", 3);
            sx_nad_write(comp->s, nad);
            jid_free(name);
            free(user);
            return;
        }

        log_write(comp->r->log, LOG_NOTICE, "[%s] set as log sink", name->domain);

        xhash_put(comp->r->log_sinks, pstrdup(xhash_pool(comp->r->log_sinks), name->domain), (void *) comp);
    }

    free(user);

    xhash_put(comp->r->routes, pstrdup(xhash_pool(comp->r->routes), name->domain), (void *) comp);
    xhash_put(comp->routes, pstrdup(xhash_pool(comp->routes), name->domain), (void *) comp);

    log_write(comp->r->log, LOG_NOTICE, "[%s] online (bound to %s, port %d)", name->domain, comp->ip, comp->port);

    nad_set_attr(nad, 0, -1, "name", NULL, 0);
    sx_nad_write(comp->s, nad);

    /* advertise name */
    _router_advertise(comp->r, name->domain, comp, 0);

    /* tell the new component about everyone else */
    xhash_walk(comp->r->routes, _router_advertise_reverse, (void *) comp);

    /* bind aliases */
    for(alias = comp->r->aliases; alias != NULL; alias = alias->next) {
        if(strcmp(alias->target, name->domain) == 0) {
            xhash_put(comp->r->routes, pstrdup(xhash_pool(comp->r->routes), alias->name), (void *) comp);
            xhash_put(comp->routes, pstrdup(xhash_pool(comp->r->routes), alias->name), (void *) comp);
            
            log_write(comp->r->log, LOG_NOTICE, "[%s] online (alias of '%s', bound to %s, port %d)", alias->name, name->domain, comp->ip, comp->port);

            /* advertise name */
            _router_advertise(comp->r, alias->name, comp, 0);
        }
    }

    /* done with this */
    jid_free(name);
}

static void _router_process_unbind(component_t comp, nad_t nad) {
    int attr;
    jid_t name;

    attr = nad_find_attr(nad, 0, -1, "name", NULL);
    if(attr < 0 || (name = jid_new(comp->r->pc, NAD_AVAL(nad, attr), NAD_AVAL_L(nad, attr))) == NULL) {
        log_debug(ZONE, "no or invalid 'name' on unbind packet, bouncing");
        nad_set_attr(nad, 0, -1, "error", "400", 3);
        sx_nad_write(comp->s, nad);
        return;
    }

    if(xhash_get(comp->routes, name->domain) == NULL) {
        log_write(comp->r->log, LOG_NOTICE, "[%s, port=%d] tried to unbind '%s', but it's not bound to this component", comp->ip, comp->port, name->domain);
        nad_set_attr(nad, 0, -1, "name", NULL, 0);
        nad_set_attr(nad, 0, -1, "error", "404", 3);
        sx_nad_write(comp->s, nad);
        jid_free(name);
        return;
    }

    xhash_zap(comp->r->log_sinks, name->domain);
    xhash_zap(comp->r->routes, name->domain);
    xhash_zap(comp->routes, name->domain);

    if(comp->r->default_route != NULL && strcmp(comp->r->default_route, name->domain) == 0) {
        log_write(comp->r->log, LOG_NOTICE, "[%s] default route offline", name->domain);
        free(comp->r->default_route);
        comp->r->default_route = NULL;
    }

    log_write(comp->r->log, LOG_NOTICE, "[%s] offline", name->domain);

    nad_set_attr(nad, 0, -1, "name", NULL, 0);
    sx_nad_write(comp->s, nad);

    /* deadvertise name */
    _router_advertise(comp->r, name->domain, comp, 1);

    jid_free(name);
}

static void _router_comp_write(component_t comp, nad_t nad) {
    int attr;

    if(comp->tq != NULL) {
        log_debug(ZONE, "%s port %d is throttled, jqueueing packet", comp->ip, comp->port);
        jqueue_push(comp->tq, nad, 0);
        return;
    }

    /* packets go raw to normal components */
    if(!comp->legacy) {
        sx_nad_write(comp->s, nad);
        return;
    }

    log_debug(ZONE, "packet for legacy component, munging");

    attr = nad_find_attr(nad, 0, -1, "error", NULL);
    if(attr >= 0) {
        if(NAD_AVAL_L(nad, attr) == 3 && strncmp("400", NAD_AVAL(nad, attr), 3) == 0)
            stanza_error(nad, 1, stanza_err_BAD_REQUEST);
        else
            stanza_error(nad, 1, stanza_err_SERVICE_UNAVAILABLE);
    }

    sx_nad_write_elem(comp->s, nad, 1);
}

static void _router_route_log_sink(xht log_sinks, const char *key, void *val, void *arg) {
    component_t comp = (component_t) val;
    nad_t nad = (nad_t) arg;

    log_debug(ZONE, "copying route to '%s' (%s, port %d)", key, comp->ip, comp->port);

    _router_comp_write(comp, nad_copy(nad));
}

static void _router_process_route(component_t comp, nad_t nad) {
    int atype, ato, afrom;
    struct jid_st sto, sfrom;
    jid_t to = NULL, from = NULL;
    component_t target;
    union xhashv xhv;
	int i;
	int attr_msg_to;
	int attr_msg_from;
	int attr_route_to;
	int attr_route_from;
	jid_t jid_msg_from = NULL;
	jid_t jid_msg_to = NULL;
	jid_t jid_route_from = NULL;
	jid_t jid_route_to = NULL;

    if(nad_find_attr(nad, 0, -1, "error", NULL) >= 0) {
        log_debug(ZONE, "dropping error packet, trying to avoid loops");
        nad_free(nad);
        return;
    }

    atype = nad_find_attr(nad, 0, -1, "type", NULL);
    ato = nad_find_attr(nad, 0, -1, "to", NULL);
    afrom = nad_find_attr(nad, 0, -1, "from", NULL);

    sto.pc = sfrom.pc = comp->r->pc;
    if(ato >= 0) to = jid_reset(&sto, NAD_AVAL(nad, ato), NAD_AVAL_L(nad, ato));
    if(afrom >= 0) from = jid_reset(&sfrom, NAD_AVAL(nad, afrom), NAD_AVAL_L(nad, afrom));

    /* unicast */
    if(atype < 0) {
        if(to == NULL || from == NULL) {
            log_debug(ZONE, "unicast route with missing or invalid to or from, bouncing");
            nad_set_attr(nad, 0, -1, "error", "400", 3);
            _router_comp_write(comp, nad);
			if (to != NULL)
				jid_free(to);
			if (from != NULL)
				jid_free(from);
            return;
        }
        
        log_debug(ZONE, "unicast route from %s to %s", from->domain, to->domain);

        /* check the from */
        if(xhash_get(comp->routes, from->domain) == NULL) {
            log_write(comp->r->log, LOG_NOTICE, "[%s, port=%d] tried to send a packet from '%s', but that name is not bound to this component", comp->ip, comp->port, from->domain);
            nad_set_attr(nad, 0, -1, "error", "401", 3);
            _router_comp_write(comp, nad);
			jid_free(to);
			jid_free(from);
            return;
        }

        /* find a target */
        target = xhash_get(comp->r->routes, to->domain);
        if(target == NULL) {
            if(comp->r->default_route != NULL && strcmp(from->domain, comp->r->default_route) == 0) {
                log_debug(ZONE, "%s is unbound, bouncing", from->domain);
                nad_set_attr(nad, 0, -1, "error", "404", 3);
                _router_comp_write(comp, nad);
				jid_free(to);
				jid_free(from);
                return;
            }
            target = xhash_get(comp->r->routes, comp->r->default_route);
        }

        if(target == NULL) {
            log_debug(ZONE, "%s is unbound, and no default route, bouncing", to->domain);
            nad_set_attr(nad, 0, -1, "error", "404", 3);
            _router_comp_write(comp, nad);
			jid_free(to);
			jid_free(from);
            return;
        }

        /* copy to any log sinks */
        if(xhash_count(comp->r->log_sinks) > 0)
            xhash_walk(comp->r->log_sinks, _router_route_log_sink, (void *) nad);

        /* push it out */
        log_debug(ZONE, "writing route for '%s' to %s, port %d", to->domain, target->ip, target->port);

		
        /* if logging enabled, log messages that match our criteria */
        if (comp->r->message_logging_enabled) {
            if ((NAD_ENAME_L(nad, 1) == 7 && strncmp("message", NAD_ENAME(nad, 1), 7) == 0) &&		// has a "message" element 
                ((attr_route_from = nad_find_attr(nad, 0, -1, "from", NULL)) >= 0) &&				
                ((nad_find_attr(nad, 1, -1, "type", "chat") >= 0) ||                                // has type='chat' attribute
                    ((comp->r->log_group_chats) &&                                            // logging is enabled for group chats
                        // ignore messages sent from mu-conference, if a filter is defined:
                        ((strncmp(NAD_AVAL(nad, attr_route_from), comp->r->filter_muc_messages_from, strlen(comp->r->filter_muc_messages_from)) != 0) &&
                        (nad_find_attr(nad, 1, -1, "type", "groupchat") >= 0)))) &&           // has type='groupchat' attribute
                ((attr_route_to = nad_find_attr(nad, 0, -1, "to", NULL)) >= 0) &&					
                ((strncmp(NAD_AVAL(nad, attr_route_to), "c2s", 3)) != 0) &&							// ignore messages to "c2s" or we'd have dups
                ((jid_route_from = jid_new(comp->r->pc, NAD_AVAL(nad, attr_route_from), NAD_AVAL_L(nad, attr_route_from))) != NULL) &&	// has valid JID source in route
                ((jid_route_to = jid_new(comp->r->pc, NAD_AVAL(nad, attr_route_to), NAD_AVAL_L(nad, attr_route_to))) != NULL) &&		// has valid JID destination in route
                ((attr_msg_from = nad_find_attr(nad, 1, -1, "from", NULL)) >= 0) &&
                ((attr_msg_to = nad_find_attr(nad, 1, -1, "to", NULL)) >= 0) &&
                ((jid_msg_from = jid_new(comp->r->pc, NAD_AVAL(nad, attr_msg_from), NAD_AVAL_L(nad, attr_msg_from))) != NULL) &&	// has valid JID source in message 
                ((jid_msg_to = jid_new(comp->r->pc, NAD_AVAL(nad, attr_msg_to), NAD_AVAL_L(nad, attr_msg_to))) != NULL))			// has valid JID dest in message
            {  
                message_log(nad, comp->r, jid_full(jid_msg_from), jid_full(jid_msg_to));
            }
            if (jid_msg_from != NULL)
                jid_free(jid_msg_from);
            if (jid_msg_to != NULL)
                jid_free(jid_msg_to);
            if (jid_route_from != NULL)
                jid_free(jid_route_from);
            if (jid_route_to != NULL)
                jid_free(jid_route_to);
        }

        _router_comp_write(target, nad);
        return;
    }

    /* broadcast */
    if(NAD_AVAL_L(nad, atype) == 9 && strncmp("broadcast", NAD_AVAL(nad, atype), 9) == 0) {
        if(from == NULL) {
            log_debug(ZONE, "broadcast route with missing or invalid from, bouncing");
            nad_set_attr(nad, 0, -1, "error", "400", 3);
            _router_comp_write(comp, nad);
			if (to != NULL)
				jid_free(to);
            return;
        }
        
        log_debug(ZONE, "broadcast route from %s", from->domain);

        /* check the from */
        if(xhash_get(comp->routes, from->domain) == NULL) {
            log_write(comp->r->log, LOG_NOTICE, "[%s, port=%d] tried to send a packet from '%s', but that name is not bound to this component", comp->ip, comp->port, from->domain);
            nad_set_attr(nad, 0, -1, "error", "401", 3);
            _router_comp_write(comp, nad);
			if (to != NULL)
				jid_free(to);
			if (from != NULL)
				jid_free(from);
            return;
        }

        /* loop the components and distribute */
        if(xhash_iter_first(comp->r->components))
            do {
                xhv.comp_val = &target;
                xhash_iter_get(comp->r->components, NULL, xhv.val);

                if(target != comp) {
                    log_debug(ZONE, "writing broadcast to %s, port %d", target->ip, target->port);
                    _router_comp_write(target, nad_copy(nad));
                }
            } while(xhash_iter_next(comp->r->components));

		if (to != NULL)
			jid_free(to);
		if (from != NULL)
			jid_free(from);
        nad_free(nad);
        return;
    }

    log_debug(ZONE, "unknown route type '%.*s', dropping", NAD_AVAL_L(nad, atype), NAD_AVAL(nad, atype));

	if (to != NULL)
		jid_free(to);
	if (from != NULL)
		jid_free(from);
    nad_free(nad);
}

static void _router_process_throttle(component_t comp, nad_t nad) {
    jqueue_t tq;
    nad_t pkt;

    if(comp->tq == NULL) {
        _router_comp_write(comp, nad);

        log_write(comp->r->log, LOG_NOTICE, "[%s, port=%d] throttling packets on request", comp->ip, comp->port);
        comp->tq = jqueue_new();
    }

    else {
        log_write(comp->r->log, LOG_NOTICE, "[%s, port=%d] unthrottling packets on request", comp->ip, comp->port);
        tq = comp->tq;
        comp->tq = NULL;

        _router_comp_write(comp, nad);

        while((pkt = jqueue_pull(tq)) != NULL)
            _router_comp_write(comp, pkt);

        jqueue_free(tq);
    }
}

static int _router_sx_callback(sx_t s, sx_event_t e, void *data, void *arg) {
    component_t comp = (component_t) arg;
    sx_buf_t buf = (sx_buf_t) data;
    int rlen, len, attr, ns, sns;
    sx_error_t *sxe;
    nad_t nad;
    struct jid_st sto, sfrom;
    jid_t to, from;
    alias_t alias;

    switch(e) {
        case event_WANT_READ:
            log_debug(ZONE, "want read");
            mio_read(comp->r->mio, comp->fd);
            break;

        case event_WANT_WRITE:
            log_debug(ZONE, "want write");
            mio_write(comp->r->mio, comp->fd);
            break;

        case event_READ:
            log_debug(ZONE, "reading from %d", comp->fd);

            /* check rate limits */
            if(comp->rate != NULL) {
                if(rate_check(comp->rate) == 0) {

                    /* inform the app if we haven't already */
                    if(!comp->rate_log) {
                        log_write(comp->r->log, LOG_NOTICE, "[%s, port=%d] is being byte rate limited", comp->ip, comp->port);

                        comp->rate_log = 1;
                    }

                    log_debug(ZONE, "%d is throttled, delaying read", comp->fd);

                    buf->len = 0;
                    return 0;
                }

                /* find out how much we can have */
                rlen = rate_left(comp->rate);
                if(rlen > buf->len)
                    rlen = buf->len;
            }

            /* no limit, just read as much as we can */
            else
                rlen = buf->len;
            
            /* do the read */
            len = recv(comp->fd, buf->data, rlen, 0);

            /* update rate limits */
            if(comp->rate != NULL && len > 0) {
                comp->rate_log = 0;
                rate_add(comp->rate, len);
            }

            if(len < 0) {
                if(errno == EWOULDBLOCK || errno == EINTR || errno == EAGAIN) {
                    buf->len = 0;
                    return 0;
                }

                log_debug(ZONE, "read failed: %s", strerror(errno));

                sx_kill(comp->s);
                
                return -1;
            }

            else if(len == 0) {
                /* they went away */
                sx_kill(comp->s);

                return -1;
            }

            log_debug(ZONE, "read %d bytes", len);

            buf->len = len;

            return len;

        case event_WRITE:
            log_debug(ZONE, "writing to %d", comp->fd);

            len = send(comp->fd, buf->data, buf->len, 0);
            if(len >= 0) {
                log_debug(ZONE, "%d bytes written", len);
                return len;
            }

            if(errno == EWOULDBLOCK || errno == EINTR || errno == EAGAIN)
                return 0;

            log_debug(ZONE, "write failed: %s", strerror(errno));
        
            sx_kill(comp->s);
        
            return -1;

        case event_ERROR:
            sxe = (sx_error_t *) data;
            log_write(comp->r->log, LOG_NOTICE, "[%s, port=%d] error: %s (%s)", comp->ip, comp->port, sxe->generic, sxe->specific);

            break;

        case event_STREAM:
            
            /* legacy check */
            if(s->ns == NULL || strcmp("jabber:component:accept", s->ns) != 0)
                return 0;

            /* component, old skool */
            comp->legacy = 1;

            /* enabled? */
            if(comp->r->local_secret == NULL) {
                sx_error(s, stream_err_INVALID_NAMESPACE, "support for legacy components not available");      /* !!! correct error? */
                sx_close(s);
                return 0;
            }
            
            /* sanity */
            if(s->req_to == NULL) {
                sx_error(s, stream_err_HOST_UNKNOWN, "no 'to' attribute on stream header");
                sx_close(s);
                return 0;
            }

            break;

        case event_OPEN:
            
            log_write(comp->r->log, LOG_NOTICE, "[%s, port=%d] authenticated as %s", comp->ip, comp->port, comp->s->auth_id);

            /* make a route for legacy components */
            if(comp->legacy) {
                /* make sure the name is available */
                if(xhash_get(comp->r->routes, s->req_to) != NULL) {
                    sx_error(s, stream_err_HOST_UNKNOWN, "requested name is already in use");   /* !!! correct error? */
                    sx_close(s);
                    return 0;
                }

                for(alias = comp->r->aliases; alias != NULL; alias = alias->next)
                    if(strcmp(alias->name, s->req_to) == 0) {
                        sx_error(s, stream_err_HOST_UNKNOWN, "requested name is already in use");   /* !!! correct error? */
                        sx_close(s);
                        return 0;
                    }


                xhash_put(comp->r->routes, pstrdup(xhash_pool(comp->r->routes), s->req_to), (void *) comp);
                xhash_put(comp->routes, pstrdup(xhash_pool(comp->routes), s->req_to), (void *) comp);

                log_write(comp->r->log, LOG_NOTICE, "[%s] online (bound to %s, port %d)", s->req_to, comp->ip, comp->port);

                /* advertise the name */
                _router_advertise(comp->r, s->req_to, comp, 0);

                /* this is a legacy component, so we don't tell it about other routes */

                /* bind aliases */
                for(alias = comp->r->aliases; alias != NULL; alias = alias->next) {
                    if(strcmp(alias->target, s->req_to) == 0) {
                        xhash_put(comp->r->routes, pstrdup(xhash_pool(comp->r->routes), alias->name), (void *) comp);
                        xhash_put(comp->routes, pstrdup(xhash_pool(comp->r->routes), alias->name), (void *) comp);
            
                        log_write(comp->r->log, LOG_NOTICE, "[%s] online (alias of '%s', bound to %s, port %d)", alias->name, s->req_to, comp->ip, comp->port);

                        /* advertise name */
                        _router_advertise(comp->r, alias->name, comp, 0);
                    }
                }
            }

            break;

        case event_PACKET:
            nad = (nad_t) data;

            /* preauth */
            if(comp->s->state == state_STREAM) {
                /* non-legacy components can't do anything before auth */
                if(!comp->legacy) {
                    log_debug(ZONE, "stream is preauth, dropping packet");
                    nad_free(nad);
                    return 0;
                }

                /* watch for handshake requests */
                if(NAD_ENAME_L(nad, 0) != 9 || strncmp("handshake", NAD_ENAME(nad, 0), NAD_ENAME_L(nad, 0)) != 0) { 
                    log_debug(ZONE, "unknown preauth packet %.*s, dropping", NAD_ENAME_L(nad, 0), NAD_ENAME(nad, 0));

                    nad_free(nad);
                    return 0;
                }

                /* process incoming handshakes */
                _router_process_handshake(comp, nad);

                return 0;
            }

            /* legacy processing */
            if(comp->legacy) {
                log_debug(ZONE, "packet from legacy component, munging it");

                sto.pc = sfrom.pc = comp->r->pc;

                attr = nad_find_attr(nad, 0, -1, "to", NULL);
                if(attr < 0 || (to = jid_reset(&sto, NAD_AVAL(nad, attr), NAD_AVAL_L(nad, attr))) == NULL) {
                    log_debug(ZONE, "invalid or missing 'to' address on legacy packet, dropping it");
                    nad_free(nad);
                    return 0;
                }

                attr = nad_find_attr(nad, 0, -1, "from", NULL);
                if(attr < 0 || (from = jid_reset(&sfrom, NAD_AVAL(nad, attr), NAD_AVAL_L(nad, attr))) == NULL) {
                    log_debug(ZONE, "invalid or missing 'from' address on legacy packet, dropping it");
                    nad_free(nad);
                    return 0;
                }

                /* rewrite component packets into client packets */
                ns = nad_find_namespace(nad, 0, "jabber:component:accept", NULL);
                if(ns >= 0) {
                    if(nad->elems[0].ns == ns)
                        nad->elems[0].ns = nad->nss[nad->elems[0].ns].next;
                    else {
                        for(sns = nad->elems[0].ns; sns >= 0 && nad->nss[sns].next != ns; sns = nad->nss[sns].next);
                        nad->nss[sns].next = nad->nss[nad->nss[sns].next].next;
                    }
                }

                ns = nad_find_namespace(nad, 0, uri_CLIENT, NULL);
                if(ns < 0) {
                    ns = nad_add_namespace(nad, uri_CLIENT, NULL);
                    nad->scope = -1;
                    nad->nss[ns].next = nad->elems[0].ns;
                    nad->elems[0].ns = ns;
                }
                nad->elems[0].my_ns = ns;

                /* wrap up the packet */
                ns = nad_add_namespace(nad, uri_COMPONENT, "comp");

                nad_wrap_elem(nad, 0, ns, "route");

                nad_set_attr(nad, 0, -1, "to", to->domain, 0);
                nad_set_attr(nad, 0, -1, "from", from->domain, 0);
            }

            /* top element must be router scoped */
            if(NAD_NURI_L(nad, NAD_ENS(nad, 0)) != strlen(uri_COMPONENT) || strncmp(uri_COMPONENT, NAD_NURI(nad, NAD_ENS(nad, 0)), strlen(uri_COMPONENT)) != 0) {
                log_debug(ZONE, "invalid packet namespace, dropping");
                nad_free(nad);
                return 0;
            }

            /* bind a name to this component */
            if(NAD_ENAME_L(nad, 0) == 4 && strncmp("bind", NAD_ENAME(nad, 0), 4) == 0) {
                _router_process_bind(comp, nad);
                return 0;
            }

            /* unbind a name from this component */
            if(NAD_ENAME_L(nad, 0) == 6 && strncmp("unbind", NAD_ENAME(nad, 0), 6) == 0) {
                _router_process_unbind(comp, nad);
                return 0;
            }

            /* route packets */
            if(NAD_ENAME_L(nad, 0) == 5 && strncmp("route", NAD_ENAME(nad, 0), 5) == 0) {
                _router_process_route(comp, nad);
                return 0;
            }

            /* throttle packets */
            if(NAD_ENAME_L(nad, 0) == 8 && strncmp("throttle", NAD_ENAME(nad, 0), 8) == 0) {
                _router_process_throttle(comp, nad);
                return 0;
            }

            log_debug(ZONE, "unknown packet, dropping");

            nad_free(nad);
            return 0;

        case event_CLOSED:
            mio_close(comp->r->mio, comp->fd);

            break;
    }

    return 0;
}

static int _router_accept_check(router_t r, int fd, char *ip) {
    rate_t rt;

    if(access_check(r->access, ip) == 0) {
        log_write(r->log, LOG_NOTICE, "[%d] [%s] access denied by configuration", fd, ip);
        return 1;
    }

    if(r->conn_rate_total != 0) {
        rt = (rate_t) xhash_get(r->conn_rates, ip);
        if(rt == NULL) {
            rt = rate_new(r->conn_rate_total, r->conn_rate_seconds, r->conn_rate_wait);
            xhash_put(r->conn_rates, pstrdup(xhash_pool(r->conn_rates), ip), (void *) rt);
        }

        if(rate_check(rt) == 0) {
            log_write(r->log, LOG_NOTICE, "[%d] [%s] is being rate limited", fd, ip);
            return 1;
        }

        rate_add(rt, 1);
    }

    return 0;
}

static void _router_route_unbind_walker(xht routes, const char *key, void *val, void *arg) {
    component_t comp = (component_t) arg;

    xhash_zap(comp->r->log_sinks, key);
    xhash_zap(comp->r->routes, key);
    xhash_zap(comp->routes, key);

    if(comp->r->default_route != NULL && strcmp(key, comp->r->default_route) == 0) {
        log_write(comp->r->log, LOG_NOTICE, "[%s] default route offline", key);
        free(comp->r->default_route);
        comp->r->default_route = NULL;
    }

    log_write(comp->r->log, LOG_NOTICE, "[%s] offline", key);

    /* deadvertise name */
    _router_advertise(comp->r, (char *) key, comp, 1);
}

int router_mio_callback(mio_t m, mio_action_t a, int fd, void *data, void *arg) {
    component_t comp = (component_t) arg;
    router_t r = (router_t) arg;
    struct sockaddr_storage sa;
    int namelen = sizeof(sa), port, nbytes;

    switch(a) {
        case action_READ:
            log_debug(ZONE, "read action on fd %d", fd);

            /* they did something */
            comp->last_activity = time(NULL);

            ioctl(fd, FIONREAD, &nbytes);
            if(nbytes == 0) {
                sx_kill(comp->s);
                return 0;
            }

            return sx_can_read(comp->s);

        case action_WRITE:
            log_debug(ZONE, "write action on fd %d", fd);

           /* update activity timestamp */
            comp->last_activity = time(NULL);

            return sx_can_write(comp->s);

        case action_CLOSE:
            log_debug(ZONE, "close action on fd %d", fd);

            r = comp->r;

            log_write(r->log, LOG_NOTICE, "[%s, port=%d] disconnect", comp->ip, comp->port);

            /* unbind names */
            xhash_walk(comp->routes, _router_route_unbind_walker, (void *) comp);

            /* deregister component */
            xhash_zap(r->components, comp->ipport);

            xhash_free(comp->routes);

            if(comp->tq != NULL)
                /* !!! bounce packets */
                jqueue_free(comp->tq);

            rate_free(comp->rate);

            jqueue_push(comp->r->dead, (void *) comp->s, 0);

            free(comp);

            break;

        case action_ACCEPT:
            log_debug(ZONE, "accept action on fd %d", fd);

            getpeername(fd, (struct sockaddr *) &sa, &namelen);
            port = j_inet_getport(&sa);

            log_write(r->log, LOG_NOTICE, "[%s, port=%d] connect", (char *) data, port);

            if(_router_accept_check(r, fd, (char *) data) != 0)
                return 1;

            comp = (component_t) malloc(sizeof(struct component_st));
            memset(comp, 0, sizeof(struct component_st));

            comp->r = r;

            comp->fd = fd;

            snprintf(comp->ip, INET6_ADDRSTRLEN, "%s", (char *) data);
            comp->port = port;

            snprintf(comp->ipport, INET6_ADDRSTRLEN, "%s:%d", comp->ip, comp->port);

            comp->s = sx_new(r->sx_env, fd, _router_sx_callback, (void *) comp);
            mio_app(m, fd, router_mio_callback, (void *) comp);

            if(r->byte_rate_total != 0)
                comp->rate = rate_new(r->byte_rate_total, r->byte_rate_seconds, r->byte_rate_wait);

            comp->routes = xhash_new(51);

            /* register component */
            xhash_put(r->components, comp->ipport, (void *) comp);

#ifdef HAVE_SSL
            sx_server_init(comp->s, SX_SSL_STARTTLS_OFFER | SX_SASL_OFFER);
#else
            sx_server_init(comp->s, SX_SASL_OFFER);
#endif

            break;
    }

    return 0;
}

int message_log(nad_t nad, router_t r, char *msg_from, char *msg_to) 
{
	time_t t;
	char *time_pos;
	int time_sz;
	struct stat filestat;
	FILE *message_file;
	short int new_msg_file = 0;
	int i;
	int nad_body_len = 0;
	long int nad_body_start = 0;
	int body_count; 
	char *nad_body = NULL;
	char body[MAX_MESSAGE*2];

	assert((int) nad);

	/* timestamp */
	t = time(NULL);
	time_pos = ctime(&t);
	time_sz = strlen(time_pos);
	/* chop off the \n */
	time_pos[time_sz-1]=' ';

	// Find the message body
	for (i = 0; NAD_ENAME_L(nad, i) > 0; i++) 
	{
		if((NAD_ENAME_L(nad, i) == 4) && (strncmp("body", NAD_ENAME(nad, i), 4) == 0))
		{
			nad_body_len = NAD_CDATA_L(nad, i);
			if (nad_body_len > 0) {
				nad_body = NAD_CDATA(nad, i);
			} else {
				log_write(r->log, LOG_NOTICE, "message_log received a message with empty body");
				return 0;
			}
			break;
		}
	}
	
        // Don't log anything if we found no NAD body
        if (nad_body == NULL) {
            return 0;
        }

        // Store original pointer address so that we know when to stop iterating through nad_body
        nad_body_start = nad_body;

	// replace line endings with "\n"
	for (body_count = 0; (nad_body < nad_body_start + nad_body_len) && (body_count < (MAX_MESSAGE*2)-3); nad_body++) {
		if (*nad_body == '\n') {
			body[body_count++] = '\\';
			body[body_count++] = 'n';
		} else {
			body[body_count++] = *nad_body;
		}
	}
	body[body_count] = '\0';

	// Log our message
	umask((mode_t) 0077);
	if (stat(r->message_logging_fullpath, &filestat)) {
		new_msg_file = 1;
	}

	if ((message_file = fopen(r->message_logging_fullpath, "a")) == NULL) 
	{
		log_write(r->log, LOG_ERR, "Unable to open message log for writing: %s", strerror(errno));
		return 1;
	}

	if (new_msg_file) {
		if (! fprintf(message_file, "# This message log is created by the jabberd router.\n"))
		{
			log_write(r->log, LOG_ERR, "Unable to write to message log: %s", strerror(errno));
			return 1;
		}
		fprintf(message_file, "# See router.xml for logging options.\n");
		fprintf(message_file, "# Format: (Date)<tab>(From JID)<tab>(To JID)<tab>(Message Body)<line end>\n");
	}

	if (! fprintf(message_file, "%s\t%s\t%s\t%s\n", time_pos, msg_from, msg_to, body))
	{
		log_write(r->log, LOG_ERR, "Unable to write to message log: %s", strerror(errno));
		return 1;
	}
		
	fclose(message_file);

	return 0;
}

/* Determine if message log needs to be rolled, and do so if necessary */
int roll_message_log(router_t r)
{
	char logfile_fullpath[243] = {0};	// path and filename, not including .xxxx.gz for rolled logs
	char logfile_compressed_path[255] = {0};	// path and filename + room for .xxxx.gz
	char logfile_uncompressed_path[243] = {0};	// path and filename of uncompressed (previously active) log
	char logfile_compressed_path_new[255] = {0};	// path and filename + room for .xxxx.gz
	char logfile_uncompressed_path_new[255] = {0};	// path and filename + room for .xxxx.gz
	char gzip_command[640] = {0};
	struct stat64 filestat;
	struct stat64 compressed_stat;
	short int bool_time_to_roll = 0;	
	long curr_time = time(NULL);
	int oldest_log_file;
	int num_old_files;
	pid_t pid;
	int i;
	FILE *message_file;

	snprintf(logfile_fullpath, sizeof(logfile_fullpath), "%s/%s", r->message_logging_dir, r->message_logging_file);

	if (stat64(logfile_fullpath, &filestat)) 
	{
		// Most likely the log hasn't been created yet.
		log_debug(ZONE, "cannot stat message log: %s", logfile_fullpath);
		return 0;
	}
	
	// determine if its time to roll the logs 
	if (((r->message_logging_roll_days > 0) && (filestat.st_birthtime + (r->message_logging_roll_days*SECS_PER_DAY) <= curr_time)) ||
		((r->message_logging_roll_megs > 0) && (filestat.st_size > 0) && (r->message_logging_roll_megs <= ((long long)filestat.st_size/BYTES_PER_MEG))))
	{
		// roll the logs	
		for (num_old_files = 0; ; num_old_files++) 
		{
			snprintf(logfile_compressed_path, sizeof(logfile_compressed_path), "%s/%s.%d.gz", r->message_logging_dir, r->message_logging_file, num_old_files);
			// check to see if compressed file exists
			if (stat64(logfile_compressed_path, &compressed_stat)) 
			{
				//cannot find the file so exit the loop
				break;
			}	
		}
		// make num_old_files an index to the oldest logfile. -1 value indicates that no compressed logfiles exist.
		num_old_files--;

		umask((mode_t) 0077);

		// from oldest to newest, rename logs by increasing their suffix by 1
		for (i = num_old_files; i >= 0; i--)
		{
			snprintf(logfile_compressed_path, sizeof(logfile_compressed_path), "%s/%s.%d.gz", r->message_logging_dir, r->message_logging_file, i);
			snprintf(logfile_compressed_path_new, sizeof(logfile_compressed_path_new), "%s/%s.%d.gz", r->message_logging_dir, r->message_logging_file, i+1);
			if ((rename(logfile_compressed_path, logfile_compressed_path_new)) != 0)
			{
				log_write(r->log, LOG_ERR, "Unable to rename message log: %s  ... to: %s", logfile_compressed_path, logfile_compressed_path_new);
				return 1;
			}
		}

		// add a .0 suffix to the uncompressed file
		snprintf(logfile_uncompressed_path, sizeof(logfile_uncompressed_path), "%s/%s", r->message_logging_dir, r->message_logging_file);
		snprintf(logfile_uncompressed_path_new, sizeof(logfile_uncompressed_path_new), "%s/%s.0", r->message_logging_dir, r->message_logging_file);
		if ((rename(logfile_uncompressed_path, logfile_uncompressed_path_new))  != 0)
		{
			log_write(r->log, LOG_ERR, "Unable to rename message log: %s  ... to: %s : %s", logfile_uncompressed_path, logfile_uncompressed_path_new, strerror(errno));
			return 1;
		}

		/*  gzip the uncompressed log file */
		// fork a child to do the gzip
		if ((pid = fork()) < 0)
		{
			log_write(r->log, LOG_ERR, "fork() problem when attempting to roll and compress logs: %s", strerror(errno));
		} else if (pid == 0)
		{		//child
			execl("/usr/bin/gzip", "gzip", "-q", logfile_uncompressed_path_new, (char *)NULL);
			log_write(r->log, LOG_ERR, "execl() error: %s", strerror(errno));
		}
		
		// initialize a new file
		if ((message_file = fopen(logfile_uncompressed_path, "w+")) == NULL) 
		{
			log_write(r->log, LOG_ERR, "Unable to open message log for writing: %s: %s", logfile_uncompressed_path, strerror(errno));
			return 1;
		}

		fprintf(message_file, "# This message log is created by the jabberd router.\n");
		fprintf(message_file, "# See router.xml for logging options.\n");
		fprintf(message_file, "# Format: (Date)<tab>(From JID)<tab>(To JID)<tab>(Message Body)<line end>\n");


		fclose(message_file);
	}
	return 0;
}



/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Monkey HTTP Daemon
 *  ------------------
 *  Copyright (C) 2001-2012, Eduardo Silva P.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <pthread.h>
#include "MKPlugin.h"

#include "duda.h"
#include "api.h"
#include "event.h"

int duda_event_register_write(duda_request_t *dr, short int event)
{
    struct mk_list *list;

    list = pthread_getspecific(duda_global_events_write);
    if (!list) {
        return -1;
    }

    dr->events_mask |= event;
    mk_list_add(&dr->_head_events_write, list);
    return 0;
}

int duda_event_unregister_write(duda_request_t *dr, short int event)
{
    struct mk_list *list, *head, *temp;
    duda_request_t *entry;

    list = pthread_getspecific(duda_global_events_write);
    mk_list_foreach_safe(head, temp, list) {
        entry = mk_list_entry(head, duda_request_t, _head_events_write);
        if (entry == dr && (entry->events_mask & event)) {
            dr->events_mask ^= event;
            mk_list_del(&entry->_head_events_write);
            pthread_setspecific(duda_global_events_write, list);
            return 0;
        }
    }

    return -1;
}

int duda_event_is_registered_write(duda_request_t *dr, short int event)
{
    struct mk_list *list;
    struct mk_list *head;
    duda_request_t *entry;

    list = pthread_getspecific(duda_global_events_write);
    mk_list_foreach(head, list) {
        entry = mk_list_entry(head, duda_request_t, _head_events_write);
        if (entry == dr && (entry->events_mask & event)) {
            return MK_TRUE;
        }
    }

    return MK_FALSE;
}

int duda_event_write_callback(int sockfd)
{
    struct mk_list *list, *temp, *head;
    duda_request_t *entry;

    list = pthread_getspecific(duda_global_events_write);
    mk_list_foreach_safe(head, temp, list) {
        entry = mk_list_entry(head, duda_request_t, _head_events_write);
        if (entry->cs->socket == sockfd) {
            if (entry->events_mask & DUDA_EVENT_BODYFLUSH) {
                if (__body_flush(entry) > 0) {
                    return MK_PLUGIN_RET_EVENT_OWNED;
                }
                else {
                    duda_service_end(entry);
                }
            }
        }
    }

    return MK_PLUGIN_RET_EVENT_CONTINUE;
}

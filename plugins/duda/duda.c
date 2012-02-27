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

#include <dlfcn.h>

#include "MKPlugin.h"
#include "duda.h"
#include "conf.h"
#include "request.h"
#include "event.h"

MONKEY_PLUGIN("duda",                                     /* shortname */
              "Duda Web Services Framework",              /* name */
              VERSION,                                    /* version */
              MK_PLUGIN_CORE_THCTX | MK_PLUGIN_STAGE_30); /* hooks */


void *duda_load_library(const char *path)
{
    void *handle;

    handle = dlopen(path, RTLD_LAZY);
    if (!handle) {
        mk_warn("dlopen() %s", dlerror());
    }

    return handle;
}

/* get specific symbol from the library */
void *duda_load_symbol(void *handler, const char *symbol)
{
    void *s;
    char *err;

    dlerror();
    s = dlsym(handler, symbol);
    if ((err = dlerror()) != NULL) {
        return NULL;
    }

    return s;
}

/* Register the service interfaces into the main list of web services */
int duda_service_register(struct duda_api_objects *api, struct web_service *ws)
{
    int (*service_init) (struct duda_api_objects *);
    struct mk_list *head_iface, *head_method;
    struct duda_interface *entry_iface;
    struct duda_method *entry_method;

    /* Load and invoke duda_init() */
    service_init = (int (*)()) duda_load_symbol(ws->handler, "duda_init");
    if (service_init(api) == 0) {
        PLUGIN_TRACE("[%s] duda_init()", ws->app_name);
        ws->map = (struct mk_list *) duda_load_symbol(ws->handler, "_duda_interfaces");

        /* Lookup callback functions for each registered method */
        mk_list_foreach(head_iface, ws->map) {
            entry_iface = mk_list_entry(head_iface, struct duda_interface, _head);
            mk_list_foreach(head_method, &entry_iface->methods) {
                entry_method = mk_list_entry(head_method, struct duda_method, _head);
                entry_method->func_cb = duda_load_symbol(ws->handler, entry_method->callback);
                if (!entry_method->func_cb) {
                    mk_err("%s / callback not found '%s'", entry_method->uid, entry_method);
                    exit(EXIT_FAILURE);
                }

            }
        }
    }

    return 0;
}

/*
 * Load the web service shared library for each definition found in the
 * virtual host
 */
int duda_load_services()
{
    char *service_path;
    unsigned long len;
    struct file_info finfo;
    struct mk_list *head_vh;
    struct mk_list *head_ws;
    struct vhost_services *entry_vs;
    struct web_service *entry_ws;
    struct duda_api_objects *api;

    mk_list_foreach(head_vh, &services_list) {
        entry_vs = mk_list_entry(head_vh, struct vhost_services, _head);
        mk_list_foreach(head_ws, &entry_vs->services) {
            entry_ws = mk_list_entry(head_ws, struct web_service, _head);

            service_path = NULL;
            mk_api->str_build(&service_path, &len,
                              "%s/%s.duda", services_root, entry_ws->app_name);

            /* Validate path, file and library load */
            if (mk_api->file_get_info(service_path, &finfo) != 0 ||
                finfo.is_file != MK_TRUE ||
                !(entry_ws->handler = duda_load_library(service_path))) {

                entry_ws->app_enabled = 0;
                mk_api->mem_free(service_path);
                continue;
            }

            /* Success */
            PLUGIN_TRACE("Library loaded: %s", entry_ws->app_name);
            mk_api->mem_free(service_path);

            /* Register service */
            api = duda_api_master();
            duda_service_register(api, entry_ws);
        }
    }

    return 0;
}

int _mkp_event_write(int sockfd)
{
    return duda_event_write_callback(sockfd);
}


void _mkp_core_prctx(struct server_config *config)
{
}

void _mkp_core_thctx()
{
    struct mk_list *list_events_write;

    list_events_write = mk_api->mem_alloc(sizeof(struct mk_list));
    mk_list_init(list_events_write);
    pthread_setspecific(duda_global_events_write, (void *) list_events_write);
}

int _mkp_init(void **api, char *confdir)
{
    mk_api = *api;

    /* Load configuration */
    duda_conf_main_init(confdir);
    duda_conf_vhost_init();
    duda_load_services();

    /* Global data / Thread scope */
    pthread_key_create(&duda_global_events_write, NULL);

    return 0;
}

int duda_request_parse(struct session_request *sr,
                       struct duda_request *dr)
{
    short int last_field = MAP_WS_APP_NAME;
    unsigned int i = 0, len, val_len;
    int end;

    len = sr->uri_processed.len;

    while (i < len) {
        end = mk_api->str_search_n(sr->uri_processed.data + i, "/",
                                   MK_STR_SENSITIVE, len - i);

        if (end >= 0 && end + i < len) {
            end += i;

            if (i == end) {
                i++;
                continue;
            }

            val_len = end - i;
        }
        else {
            val_len = len - i;
            end = len;
        }

        switch (last_field) {
        case MAP_WS_APP_NAME:
            dr->appname.data = sr->uri_processed.data + i;
            dr->appname.len  = val_len;
            last_field = MAP_WS_INTERFACE;
            break;
        case MAP_WS_INTERFACE:
            dr->interface.data = sr->uri_processed.data + i;
            dr->interface.len  = val_len;
            last_field = MAP_WS_METHOD;
            break;
        case MAP_WS_METHOD:
            dr->method.data    = sr->uri_processed.data + i;
            dr->method.len     = val_len;
            last_field = MAP_WS_PARAM;
            break;
        case MAP_WS_PARAM:
            if (dr->n_params >= MAP_WS_MAX_PARAMS) {
                PLUGIN_TRACE("too much parameters (max=%i)", MAP_WS_MAX_PARAMS);
                return -1;
            }
            dr->params[dr->n_params].data = sr->uri_processed.data + i;
            dr->params[dr->n_params].len  = val_len;
            dr->n_params++;
            last_field = MAP_WS_PARAM;
            break;
        }

        i = end + 1;
    }

    if (last_field < MAP_WS_METHOD) {
        return -1;
    }

    return 0;
}

int duda_service_end(duda_request_t *dr)
{
    /* call service end_callback() */
    if (dr->end_callback) {
        dr->end_callback(dr);
    }

    /* Finalize HTTP stuff with Monkey core */
    mk_api->http_request_end(dr->cs->socket);

    /* Free resources allocated by Duda */
    if (dr->body_buffer) {
        mk_api->mem_free(dr->body_buffer);
    }
    mk_api->mem_free(dr);

    return 0;
}

int duda_service_run(struct client_session *cs,
                     struct session_request *sr,
                     struct web_service *web_service)
{
    struct duda_request *dr;
    struct mk_list *head_iface, *head_method;
    struct duda_interface *entry_iface;
    struct duda_method *entry_method;
    void *(*callback) (duda_request_t *) = NULL;

    dr = mk_api->mem_alloc(sizeof(struct duda_request));
    if (!dr) {
        PLUGIN_TRACE("could not allocate enough memory");
        return -1;
    }

    /* service details */
    dr->web_service = web_service;
    dr->n_params = 0;
    dr->cs = cs;
    dr->sr = sr;
    dr->events_mask = 0;

    /* body buffer */
    dr->body_buffer = NULL;
    dr->body_buffer_size = 0;

    /* callbacks */
    dr->end_callback = NULL;

    /* statuses */
    dr->_st_http_headers_sent = MK_FALSE;
    dr->_st_body_writes = 0;

    /* Parse request */
    if (duda_request_parse(sr, dr) != 0) {
        mk_api->mem_free(dr);
        return -1;
    }

    /* Invoke the web service callback */
    mk_list_foreach(head_iface, dr->web_service->map) {
        entry_iface = mk_list_entry(head_iface, struct duda_interface, _head);
        if (strncmp(entry_iface->uid, dr->interface.data, dr->interface.len) == 0) {
            /* try to match method */
            mk_list_foreach(head_method, &entry_iface->methods) {
                entry_method = mk_list_entry(head_method, struct duda_method, _head);
                if (strncmp(entry_method->uid, dr->method.data, dr->method.len) == 0) {
                    callback = entry_method->func_cb;
                    break;
                }
            }
        }
    }

    if (!callback) {
        PLUGIN_TRACE("callback not found: '%s'", callback);
        return -1;
    }

    PLUGIN_TRACE("CB %s()", entry_method->callback);
    entry_method->func_cb(dr);

    return 0;
}

/*
 * Get webservice given the processed URI.
 *
 * Check the web services registered under the virtual host and try to do a
 * match with the web services name
 */
struct web_service *duda_get_service_from_uri(struct session_request *sr,
                                              struct vhost_services *vs_host)
{
    int pos;
    struct mk_list *head;
    struct web_service *ws_entry;

    /* get web service name limit */
    pos = mk_api->str_search_n(sr->uri_processed.data + 1, "/",
                               MK_STR_SENSITIVE,
                               sr->uri_processed.len - 1);
    if (pos <= 1) {
        return NULL;
    }

    /* match services */
    mk_list_foreach(head, &vs_host->services) {
        ws_entry = mk_list_entry(head, struct web_service, _head);
        if (strncmp(ws_entry->app_name,
                    sr->uri_processed.data + 1,
                    pos - 1) == 0) {
            PLUGIN_TRACE("WebService match: %s", ws_entry->app_name);
            return ws_entry;
        }
    }

    return NULL;
}


void _mkp_exit()
{

}

/*
 * Request handler: when the request arrives this callback is invoked.
 */
int _mkp_stage_30(struct plugin *plugin, struct client_session *cs,
                  struct session_request *sr)
{
    struct mk_list *head;
    struct vhost_services *vs_entry, *vs_match=NULL;
    struct web_service *web_service;

    /* we don't care about '/' request */
    if (sr->uri_processed.len > 1) {
        /* Match virtual host */
        mk_list_foreach(head, &services_list) {
            vs_entry = mk_list_entry(head, struct vhost_services, _head);
            if (sr->host_conf == vs_entry->host) {
                vs_match = vs_entry;
                break;
            }
        }

        if (!vs_match) {
            return MK_PLUGIN_RET_NOT_ME;
        }

        /* Match web service */
        web_service = duda_get_service_from_uri(sr, vs_match);
        if (!web_service) {
            return MK_PLUGIN_RET_NOT_ME;
        }

        duda_service_run(cs, sr, web_service);
        return MK_PLUGIN_RET_CONTINUE;
    }

    return MK_PLUGIN_RET_NOT_ME;
}

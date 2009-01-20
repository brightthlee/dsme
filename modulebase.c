/**
   @file modulebase.c

   Implements DSME plugin framework.
   <p>
   Copyright (C) 2004-2009 Nokia Corporation

   @author Ari Saastamoinen
   @author Semi Malinen <semi.malinen@nokia.com>

   This file is part of Dsme.

   Dsme is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License
   version 2.1 as published by the Free Software Foundation.

   Dsme is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with Dsme.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "dsme/modulebase.h"
#include "dsme/messages.h"
#include "dsme/protocol.h"
#include "dsme/logging.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <unistd.h>


/**
   Loaded module information.
*/
struct module_t {
    char* name;
    int   priority;
    void* handle;
};


/**
   Registered handler information.
*/
typedef struct {
    u_int32_t       msg_type;
    size_t          msg_size;
    const module_t* owner;
    handler_fn_t*   callback;
} msg_handler_info_t;


/**
    Sender of a queued message.
*/
struct endpoint_t {
    const module_t*        module;
    dsmesock_connection_t* conn;
};


/**
   Queued message.
*/
typedef struct {
    endpoint_t         from;
    const module_t*    to;
    dsmemsg_generic_t* data;
} queued_msg_t;


static int add_msghandlers(module_t* module);

static int remove_msghandlers(module_t* module);

/** 
   Comparison function; matches messages with handlers by message type.

   @param handler Handler to be compared
   @param msg_type Message type to be compared. Internally cast to integer.
   @return Non-0 if message type matches with given node, otherwise 0.
*/
static gint msg_comparator(gconstpointer a, gconstpointer b);


/**
   Comparison function; used to sort message handlers.

   Message handlers are sorted primarily by message type in descending order.
   Handlers for same message type are sorted by priority in descending order.

   @param handler   New handler to be added to the list of handlers.
   @param existing  Existing handler in list of handlers.
   @return Non-0 when the new handler should be inserted before existing
           handler, otherwise 0.
*/
static gint sort_comparator(gconstpointer a, gconstpointer b, gpointer unused);


/**
   Passes a message to all matching message handlers.

   @param from   Sender of the message
   @param msg	 Message to be handled
   @return 0
*/
static int handle_message(endpoint_t*              from,
                          const module_t*          to,
                          const dsmemsg_generic_t* msg);


static GSList*     modules       = 0;
static GSList*     callbacks     = 0;
static GSList*     message_queue = 0;


static int msg_comparator(gconstpointer a, gconstpointer b)
{
  const msg_handler_info_t* handler  = (msg_handler_info_t*)a;
  const void*               msg_type = b;

  return handler->msg_type - (u_int32_t)msg_type;
}


static gint sort_comparator(gconstpointer a, gconstpointer b, gpointer unused)
{
    const msg_handler_info_t* handler  = (msg_handler_info_t*)a;
    const msg_handler_info_t* existing = (msg_handler_info_t*)b;

    if (handler->msg_type > existing->msg_type)
        return 1;

    if (handler->msg_type < existing->msg_type)
        return 0;

    // TODO: double check that we get the ordering right here
    return handler->owner->priority > existing->owner->priority;
}


#ifdef OBSOLETE
/**
   Comparison function; match module by name

   @param node  Module node to be compared
   @param name  String to be compared
   @return 0 when name equas with node name, <>0 otherwise
*/
static int name_comparator(const module_t* node, const char* name)
{
    return !strcmp(node->name, name);
}
#endif


/**
   Add single hadler in message handlers list

   @param msg_type  Type of the message to be registered
   @param callback Function to be called when given msg_type is handled
   @param owner    Pointer to the module module who owns this callback
   @return 0 on OK, -1 on error
*/
int add_single_handler(u_int32_t       msg_type,
                       size_t          msg_size,
		       handler_fn_t*   callback,
	 	       const module_t* owner)
{
    msg_handler_info_t* handler = 0;

    handler = (msg_handler_info_t*)malloc(sizeof(msg_handler_info_t));
    if (!handler) {
        return -1;
    }
  
    handler->msg_type = msg_type;
    handler->msg_size = msg_size;
    handler->callback = callback;
    handler->owner    = owner;
  
    /* Insert into sorted list. */
    callbacks = g_slist_insert_sorted_with_data(callbacks,
                                                handler,
                                                sort_comparator,
                                                0);

    return 0;
}


/**
   Locates message handler table from module and adds all message handlers
   to global handler list.

   @param module	Pointer to the module to be initialised
   @return 0 when OK, or -1 on error
*/

static int add_msghandlers(module_t* module)
{
    module_fn_info_t* msg_handler_ptr;
  
    if (!module)
        return -1;
  
    for (msg_handler_ptr = (module_fn_info_t*)dlsym(module->handle,
                                                    "message_handlers");
	 msg_handler_ptr && msg_handler_ptr->callback;
	 msg_handler_ptr++)
    {
        if (add_single_handler(msg_handler_ptr->msg_type,
                               msg_handler_ptr->msg_size,
			       msg_handler_ptr->callback,
			       module))
            return -1;
    }

    return 0;
}


/** 
   This function is called when some module is unloaded.
   This removes all registered message callbacks of gimes module.

   @param module Module whose callbacks will be removed
   @return 0
*/
static int remove_msghandlers(module_t* module)
{
  GSList* node;
  GSList* next;

  for (node = callbacks; node != 0; node = next) {
    next = g_slist_next(node);
    if (((msg_handler_info_t *)(node->data))->owner == module) {
      if (node->data) {
        free(node->data);
	node->data = NULL;
      }
      callbacks = g_slist_delete_link(callbacks, node);
    }
  }
  
  return 0;
}


static const module_t* currently_handling_module = 0;


// TODO: all these returns and mallocs are a mess
static void queue_message(const endpoint_t* from,
                          const module_t*   to,
                          const void*       msg,
                          size_t            extra_size,
                          const void*       extra)
{
  queued_msg_t*      newmsg;
  dsmemsg_generic_t* genmsg = (dsmemsg_generic_t*)msg;

  if (!msg) return;
  if (genmsg->line_size_ < sizeof(dsmemsg_generic_t)) return;

  newmsg = (queued_msg_t*)malloc(sizeof(queued_msg_t));
  if (!newmsg) return;

  newmsg->data = (dsmemsg_generic_t*)malloc(genmsg->line_size_ + extra_size);
  if (newmsg->data) {
      memcpy(newmsg->data, genmsg, genmsg->line_size_);
      memcpy(((char*)newmsg->data)+genmsg->line_size_, extra, extra_size);
      newmsg->data->line_size_ += extra_size;

      newmsg->from = *from;
      newmsg->to   = to;

      // TODO: perhaps use GQueue for faster appending?
      message_queue = g_slist_append(message_queue, newmsg);
      return;
  }

  free(newmsg);
  newmsg = NULL;
}

void broadcast_internally(const void* msg)
{
  endpoint_t from = {
    .module = currently_handling_module,
    .conn   = 0
  };

  /* use 0 as recipient for broadcasting */
  queue_message(&from, 0, msg, 0, 0);
}

void broadcast_internally_from_socket(const void*            msg,
                                      dsmesock_connection_t* conn)
{
  endpoint_t from = {
    .module = 0,
    .conn   = conn
  };

  /* use 0 as recipient for broadcasting */
  queue_message(&from, 0, msg, 0, 0);
}

void broadcast_with_extra(const void* msg, size_t extra_size, const void* extra)
{
  endpoint_t from = {
    .module = currently_handling_module,
    .conn   = 0
  };

  queue_message(&from, 0, msg, extra_size, extra);
  dsmesock_broadcast_with_extra(msg, extra_size, extra);
}

void broadcast(const void* msg)
{
  broadcast_with_extra(msg, 0, 0);
}

static void queue_for_module_with_extra(const module_t* recipient,
                                        const void*     msg,
                                        size_t          extra_size,
                                        const void*     extra)
{
  endpoint_t from = {
    .module = currently_handling_module,
    .conn   = 0
  };

  queue_message(&from, recipient, msg, extra_size, extra);
}

void endpoint_send_with_extra(endpoint_t* recipient,
                              const void* msg,
                              size_t      extra_size,
                              const void* extra)
{
  if (recipient) {
    if (recipient->module) {
      queue_for_module_with_extra(recipient->module, msg, extra_size, extra);
    } else if (recipient->conn) {
      dsmesock_send_with_extra(recipient->conn,   msg, extra_size, extra);
    } else {
      dsme_log(LOG_DEBUG, "endpoint_send(): no endpoint");
    }
  } else {
    dsme_log(LOG_DEBUG, "endpoint_send(): null endpoint");
  }
}

void endpoint_send(endpoint_t* recipient, const void* msg)
{
  endpoint_send_with_extra(recipient, msg, 0, 0);
}


const struct ucred* endpoint_ucred(endpoint_t* sender)
{
  const struct ucred* u = 0;

  if (sender) {
    if (sender->module) {
      static struct ucred module_ucred;

      module_ucred.pid = getpid();
      module_ucred.uid = getuid();
      module_ucred.gid = getgid();

      u = &module_ucred;
    } else if (sender->conn) {
      u = dsmesock_getucred(sender->conn);
    }
  }

  if (!u) {
    static const struct ucred bogus_ucred = {
        .pid =  0,
        .uid = -1,
        .gid = -1
    };
    u = &bogus_ucred;
  }

  return u;
}

bool endpoint_same(const endpoint_t* a, const endpoint_t* b)
{
  bool same = false;

  if (a && b) {
    if ((a->module && a->module == b->module) ||
        (a->conn   && a->conn   == b->conn))
    {
      same = true;
    }
  }

  return same;
}

endpoint_t* endpoint_copy(const endpoint_t* endpoint)
{
  endpoint_t* copy = 0;
  
  if (endpoint) {
    copy = malloc(sizeof(endpoint_t));

    if (copy) {
        copy->module = endpoint->module;
        copy->conn   = endpoint->conn;
    }
  }

  return copy;
}

void endpoint_free(endpoint_t* endpoint)
{
  free(endpoint);
}


void process_message_queue(void)
{
	while (message_queue) {
		queued_msg_t* front = (queued_msg_t*)message_queue->data;
		
		message_queue = g_slist_delete_link(message_queue,
                                                    message_queue);
		handle_message(&front->from, front->to, front->data);
		free(front->data);
		free(front);
	}
}


static int handle_message(endpoint_t*              from,
                          const module_t*          to,
                          const dsmemsg_generic_t* msg)
{
  GSList*                   node;
  const msg_handler_info_t* handler;

  node = callbacks;
  while ((node = g_slist_find_custom(node,
                                     (void*)dsmemsg_id(msg),
                                     msg_comparator)))
  {
      handler = (msg_handler_info_t*)(node->data);
      if (handler && handler->callback) {
          if (!to || to == handler->owner) {
              if (msg->line_size_ >= handler->msg_size &&
                  msg->size_      == handler->msg_size)
              {
                  currently_handling_module = handler->owner;
                  handler->callback(from, msg);
                  currently_handling_module = 0;
              }
          }
      }
      node = g_slist_next(node);
  }

  return 0;
}


int unload_module(module_t* module)
{
    GSList*           node;
    module_fini_fn_t* finifunc;

    node = g_slist_find(modules, module);
    if (node == 0) return -1;

    remove_msghandlers(module);

    if (module->handle) {
        /* Call module_fini() -function if it exists */
        currently_handling_module = module;
        finifunc = (module_fini_fn_t *)dlsym(module->handle, "module_fini");
	if (finifunc) {
	    finifunc();
        }
        currently_handling_module = 0;

	dlclose(module->handle);
    }

	if (module->name) {
		free(module->name); 
		module->name = NULL;
	}
    if (module) {
		free(module);
		module = NULL;
	}
    modules = g_slist_delete_link(modules, node);

    return 0;
}


module_t* load_module(const char* filename, int priority)
{
    void*             dlhandle = 0;
    module_t*         module   = 0;
    module_init_fn_t* initfunc;

    /* Prepend ./ to non-absolute path */
    if (*filename != '/') {
        int     namelen;
        char *  newname;

        namelen = strlen(filename) + 3;
        newname = (char*)malloc(namelen);
        if (newname) {
            snprintf(newname, namelen, "./%s", filename);
            dlhandle = dlopen(newname, RTLD_NOW | RTLD_GLOBAL);
            free(newname);
        }
    }

    if (!dlhandle) {
        dlhandle = dlopen(filename, RTLD_NOW | RTLD_GLOBAL);
    }
 
    if (!dlhandle) {
        dsme_log(LOG_CRIT, "%s", dlerror());
        return 0;
    }

    /* Now the module should be open */
	
    module = (module_t*)malloc(sizeof(module_t));
    if (!module) {
        goto error;
    }
    
    module->handle = dlhandle;
    module->priority = priority;
    module->name = strdup(filename);
    if (!module->name) {
        goto error;
    }

    /* Call module_init() -function if it exists */
    initfunc = (module_init_fn_t *)dlsym(dlhandle, "module_init");
    if (initfunc) {
        currently_handling_module = module;
        initfunc(module);
        currently_handling_module = 0;
    }

    /* Add message handlers for the module */
    if(add_msghandlers(module)) {
        goto error;
    }

	/* Insert thee module to the modulelist */
    modules = g_slist_append(modules, module); /* Add module to list */

    return module;

error:
    dsme_log(LOG_CRIT, "%s", dlerror());
    if (module) {
        remove_msghandlers(module);
        free(module);
    }

    if (dlhandle) dlclose(dlhandle);
  
  return 0;
}


bool modulebase_init(const struct _GSList* module_names)
{
    const GSList* modname;

    for (modname = module_names; modname; modname = g_slist_next(modname)) {
        if (load_module((const char*)modname->data, 0) == NULL) {
            dsme_log(LOG_CRIT, "Error loading start-up module: %s", dlerror());
            return false;
        }
    }

    return true;
}


const char* module_name(const module_t* module)
{
  return module->name;
}

int modulebase_shutdown(void)
{
        while (modules) {
		process_message_queue();
		unload_module((module_t*)modules->data);
	}

	process_message_queue();

	return 0;
}
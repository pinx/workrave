// DBus.c
//
// Copyright (C) 2007, 2008 Rob Caelers <robc@krandor.nl>
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// Based on code from pidgin.
//
// Glib integration based on dbus-glib and libgdbus:
//
// Copyright (C) 2007-2008  Intel Corporation
// Copyright (C) 2002, 2003 CodeFactory AB
// Copyright (C) 2005 Red Hat, Inc.
//
//

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "debug.hh"
#include <string.h>

#include <string>
#include <list>
#include <map>

//#define DBUS_API_SUBJECT_TO_CHANGE
//#include <dbus/dbus-glib-lowlevel.h>

#include "DBus.hh"
#include "DBusBinding.hh"
#include "DBusException.hh"

using namespace std;
using namespace workrave;

//! Construct a new D-BUS bridge
DBus::DBus()
  : connection(NULL), owner(false), watches(NULL), timeouts(NULL)
{
}


//! Destruct the D-BUS bridge
DBus::~DBus()
{
  if (connection != NULL)
    {
      dbus_connection_unref(connection);
    }
}
  

//! Initialize D-BUS bridge
void
DBus::init()
{
	DBusError error;
  
	dbus_error_init(&error);
  
	connection = dbus_bus_get(DBUS_BUS_STARTER, &error);
  if (dbus_error_is_set(&error))
    {
      connection = NULL;
      dbus_error_free(&error);
      throw DBusSystemException("Unable to obtain session bus");
    }

	dbus_connection_set_exit_on_disconnect(connection, FALSE);

  connection_setup(NULL);
  //dbus_connection_setup_with_g_main(connection, NULL);
}


//! Registers the specified service
void
DBus::register_service(const std::string &service)
{
	DBusError error;
  int result = 0;
  
	dbus_error_init(&error);
  
	result = dbus_bus_request_name(connection,
                                 service.c_str(),
                                 DBUS_NAME_FLAG_ALLOW_REPLACEMENT | DBUS_NAME_FLAG_DO_NOT_QUEUE,
                                 &error);

	if (dbus_error_is_set(&error))
    {
      dbus_connection_unref(connection);
      connection = NULL;

      dbus_error_free(&error);
      throw DBusSystemException("Unable to request service");
    }

	if (result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
    {
      owner = false;
    }

  owner = true;
}


//! Registers the specified object path
void
DBus::register_object_path(const string &object_path)
{
	static DBusObjectPathVTable vtable = { NULL,
                                         &DBus::dispatch_static,
                                         NULL,
                                         NULL,
                                         NULL,
                                         NULL };

	if (!dbus_connection_register_object_path(connection,
                                            object_path.c_str(),
                                            &vtable,
                                            this))
	{
    throw DBusSystemException("Unable to register object");
	}
}


//! Connect a D-DBUS object/interface to a C object
void
DBus::connect(const std::string &object_path, const std::string &interface_name, void *cobject)
{
  DBusBindingBase *binding = find_binding(interface_name);
  if (binding == NULL)
    {
      throw DBusSystemException("No such interface");
    }

  ObjectIter it = objects.find(object_path);
  if (it != objects.end())
    {
      Interfaces &interfaces = it->second;

      interfaces[interface_name] = cobject;
    }
  else
    {
      Interfaces interfaces;
      interfaces[interface_name] = cobject;
      objects[object_path] = interfaces;
    }
}


//! Disconnect a D-DBUS object/interface to a C object
void
DBus::disconnect(const std::string &object_path, const std::string &interface_name)
{
  ObjectIter it = objects.find(object_path);
  if (it != objects.end())
    {
      Interfaces &interfaces = it->second;

      interfaces.erase(interface_name);
    }
}


//! Register an interface binding
void
DBus::register_binding(const std::string &name, DBusBindingBase *interface)
{
  bindings[name] = interface;
}


//! Find an interface binding
DBusBindingBase *
DBus::find_binding(const std::string &interface_name) const
{
  DBusBindingBase *ret = NULL;
  
  BindingCIter it = bindings.find(interface_name);
  if (it != bindings.end())
    {
      ret = it->second;
    }
  
  return ret;
}


void
DBus::send(DBusMessage *msg) const
{
  dbus_connection_send(connection, msg, NULL);
  dbus_message_unref(msg);
  dbus_connection_flush(connection);
}


void *
DBus::find_cobject(const std::string &path, const std::string &interface_name) const
{
  void *cobject = NULL;
  
  ObjectCIter object_it = objects.find(path);
  if (object_it != objects.end())
    {
      InterfaceCIter interface_it = object_it->second.find(interface_name);

      if (interface_it != object_it->second.end())
        {
          cobject = interface_it->second;
        }
    }
  
  
  return cobject;
}


bool
DBus::is_owner() const
{
  return owner;
}


bool
DBus::is_available() const
{
  return connection != NULL;
}


DBusHandlerResult
DBus::dispatch(DBusConnection *connection, DBusMessage *message)
{
  try
    {
      if (dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_METHOD_CALL &&
          dbus_message_has_interface(message, DBUS_INTERFACE_INTROSPECTABLE) &&
          dbus_message_has_member(message, "Introspect"))
        {
          return handle_introspect(connection, message);
        }
      else if (dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_METHOD_CALL)
        {
          return handle_method(connection, message);
        }
    }
  catch (DBusException &e)
    {
			DBusMessage *reply;
      reply = dbus_message_new_error(message,	e.id().c_str(), e.details().c_str());
			if (reply != NULL)
			{
				dbus_connection_send(connection, reply, NULL);
				dbus_message_unref(reply);
			}
    }
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


DBusHandlerResult
DBus::dispatch_static(DBusConnection *connection,
                      DBusMessage *message,
                      void *user_data)
{
  DBus *dbus = (DBus *) user_data;
  return dbus->dispatch(connection, message);
}


static const char *
dbus_gettext(const char **ptr)
{
	const char *text = *ptr;
	*ptr += strlen(text) + 1;
	return text;
}


DBusHandlerResult
DBus::handle_introspect(DBusConnection *connection, DBusMessage *message)
{
	string str;

  string path = dbus_message_get_path(message);

  ObjectCIter object_it = objects.find(path);
  if (object_it != objects.end())
    {
      str = "<!DOCTYPE node PUBLIC '-//freedesktop//DTD D-BUS Object Introspection 1.0//EN' 'http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd'>\n";
      str += "<node name='" + path + "'>\n";

      str += "<interface name=\"org.freedesktop.DBus.Introspectable\">\n";
      str += "<method name=\"Introspect\">\n";
      str += "<arg name=\"data\" direction=\"out\" type=\"s\"/>\n";
      str += "</method>\n";
      str += "</interface>\n";
      
      for (InterfaceCIter interface_it = object_it->second.begin();
           interface_it != object_it->second.end();
           interface_it++)
        {
          string interface_name = interface_it->first;
          
          DBusBindingBase *binding = find_binding(interface_name);
          if (binding == NULL)
            {
              throw DBusSystemException("Internal error, unknown interface");
            }
        
          str += "<interface name='" + interface_name + "'>\n";

          DBusIntrospect *table = binding->get_method_introspect();
          while (table->name != NULL)
            {
              str += string("<method name='") + table->name + "'>\n";

              const char *text = table->signature;
              while (*text)
                {
                  const char *name, *direction, *type;

                  direction = dbus_gettext(&text);
                  type = dbus_gettext(&text);
                  name = dbus_gettext(&text);
              
                  str += string("<arg name='") + name + "' type='" + type +"' direction='"+ direction +"'/>\n";
                }
              str += "</method>\n";
              table++;
            }
      
          table = binding->get_signal_introspect();
          while (table->name != NULL)
            {
              str += string("<signal name='") + table->name + "'>\n";

              const char *text = table->signature;
              while (*text)
                {
                  const char *name, *type;

                  type = dbus_gettext(&text);
                  name = dbus_gettext(&text);
              
                  str += string("<arg name='") + name + "' type='" + type +"'/>\n";
                }
              str += "</signal>\n";
              table++;
            }

          str += "</interface>\n";
        }
      str += "</node>\n";


      const char *cstr = str.c_str();

      DBusMessage *reply = dbus_message_new_method_return(message);

      dbus_message_append_args(reply, DBUS_TYPE_STRING, &cstr, DBUS_TYPE_INVALID);
      dbus_connection_send(connection, reply, NULL);
      dbus_message_unref(reply);
    
      return DBUS_HANDLER_RESULT_HANDLED;
    }

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


DBusHandlerResult
DBus::handle_method(DBusConnection *connection, DBusMessage *message)
{
  string path = dbus_message_get_path(message);
  string interface_name = dbus_message_get_interface(message);
  string method = dbus_message_get_member(message);

  void *cobject = find_cobject(path, interface_name);
  if (cobject == NULL)
    {
      throw DBusUsageException(string("no such object: ") + path + " " + interface_name );
    }
  
  DBusBindingBase *binding = find_binding(interface_name);
  if (binding == NULL)
    {
      throw DBusSystemException(string("No such binding: ") + interface_name );
    }

  DBusMessage *reply = binding->call(method, cobject, message);
  if (reply == NULL)
    {
      throw DBusUsageException("Call failure");
    }
  
  dbus_connection_send(connection, reply, NULL);
  dbus_message_unref(reply);
                
  return DBUS_HANDLER_RESULT_HANDLED;
}


typedef struct {
	DBusWatch *watch;
	GSource *source;
  DBus *dbus;
} WatchData;

typedef struct {
	DBusTimeout *timeout;
	guint id;
  DBus *dbus;
} TimeoutData;

typedef struct {
	GSource source;
	DBusConnection *connection;
} QueueData;



gboolean
DBus::queue_prepare(GSource *source, gint *timeout)
{
	DBusConnection *connection = ((QueueData *) source)->connection;

	*timeout = -1;

	return (dbus_connection_get_dispatch_status(connection) == DBUS_DISPATCH_DATA_REMAINS);
}


gboolean
DBus::queue_check(GSource *source)
{
  (void) source;
	return FALSE;
}


gboolean
DBus::queue_dispatch(GSource *source, GSourceFunc callback, gpointer data)
{
  (void) data;
  (void) callback;
  
	DBusConnection *connection = ((QueueData *) source)->connection;
  
	dbus_connection_ref(connection);
	dbus_connection_dispatch(connection);
	dbus_connection_unref(connection);

	return TRUE;
}

gboolean
DBus::watch_dispatch(GIOChannel *source, GIOCondition condition, gpointer data)
{
  (void) source;

	WatchData *watch_data = (WatchData *)data;
	unsigned int flags = 0;

	if (condition & G_IO_IN)
		flags |= DBUS_WATCH_READABLE;

	if (condition & G_IO_OUT)
		flags |= DBUS_WATCH_WRITABLE;

	if (condition & G_IO_ERR)
		flags |= DBUS_WATCH_ERROR;

	if (condition & G_IO_HUP)
		flags |= DBUS_WATCH_HANGUP;

	dbus_watch_handle(watch_data->watch, flags);

	return TRUE;
}

GSourceFuncs DBus::queue_funcs = {
	DBus::queue_prepare,
	DBus::queue_check,
	DBus::queue_dispatch,
	NULL,
  NULL,
  NULL,
};

void
DBus::watch_finalize(gpointer data)
{
	WatchData *watch_data = (WatchData*) data;

	if (watch_data->watch)
		dbus_watch_set_data(watch_data->watch, NULL, NULL);

	g_free(watch_data);
}

void
DBus::watch_free(void *data)
{
	WatchData *watch_data = (WatchData*) data;

	if (watch_data->source == NULL)
		return;

	watch_data->dbus->watches = g_slist_remove(watch_data->dbus->watches, watch_data);

	g_source_destroy(watch_data->source);
	g_source_unref(watch_data->source);
}

dbus_bool_t
DBus::watch_add(DBusWatch *watch, void *data)
{
	DBus *dbus = (DBus*)data;
	GIOCondition condition = (GIOCondition) (G_IO_ERR | G_IO_HUP);
	GIOChannel *channel;
	GSource *source;
	WatchData *watch_data;
	unsigned int flags;
	int fd;

	if (dbus_watch_get_enabled(watch) == FALSE)
		return TRUE;

	flags = dbus_watch_get_flags(watch);

	if (flags & DBUS_WATCH_READABLE)
		condition = (GIOCondition) (condition | G_IO_IN);

	if (flags & DBUS_WATCH_WRITABLE)
		condition = (GIOCondition) (condition | G_IO_OUT);

	fd = dbus_watch_get_unix_fd(watch);

	watch_data = g_new0(WatchData, 1);

	channel = g_io_channel_unix_new(fd);

	source = g_io_create_watch(channel, condition);

	watch_data->watch = watch;
	watch_data->source = source;
  watch_data->dbus = dbus;

	g_source_set_callback(source, (GSourceFunc) watch_dispatch,
                        watch_data, watch_finalize);

	g_source_attach(source, dbus->context);

	watch_data->dbus->watches = g_slist_prepend(watch_data->dbus->watches, watch_data);

	dbus_watch_set_data(watch, watch_data, watch_free);

	g_io_channel_unref(channel);

	return TRUE;
}

void
DBus::watch_remove(DBusWatch *watch, void *data)
{
	WatchData *watch_data = (WatchData*) dbus_watch_get_data(watch);
	DBus *dbus = (DBus*)data;

	dbus_watch_set_data(watch, NULL, NULL);

	if (watch_data != NULL)
    {
      dbus->watches = g_slist_remove(dbus->watches, watch_data);

      g_source_destroy(watch_data->source);
      g_source_unref(watch_data->source);
    }
}

void
DBus::watch_toggled(DBusWatch *watch, void *data)
{
	if (dbus_watch_get_enabled(watch) == TRUE)
		watch_add(watch, data);
	else
		watch_remove(watch, data);
}

gboolean
DBus::timeout_dispatch(gpointer data)
{
	TimeoutData *timeout_data = (TimeoutData*) data;

	dbus_timeout_handle(timeout_data->timeout);

	return FALSE;
}

void
DBus::timeout_free(void *data)
{
	TimeoutData *timeout_data = (TimeoutData*)data;

	if (timeout_data->id > 0)
		g_source_remove(timeout_data->id);

	g_free(timeout_data);
}


dbus_bool_t
DBus::timeout_add(DBusTimeout *timeout, void *data)
{
	TimeoutData *timeout_data;
  DBus *dbus = (DBus *) data;
  
	if (dbus_timeout_get_enabled(timeout) == FALSE)
		return TRUE;

	timeout_data = g_new0(TimeoutData, 1);

	timeout_data->timeout = timeout;
  timeout_data->dbus = dbus;
  
	timeout_data->id = g_timeout_add(dbus_timeout_get_interval(timeout),
                                   timeout_dispatch, timeout_data);

	dbus->timeouts = g_slist_prepend(dbus->timeouts, timeout_data);

	dbus_timeout_set_data(timeout, timeout_data, timeout_free);

	return TRUE;
}

void
DBus::timeout_remove(DBusTimeout *timeout, void *data)
{
	TimeoutData *timeout_data = (TimeoutData*)dbus_timeout_get_data(timeout);
  DBus *dbus = (DBus *) data;
  
	if (timeout_data == NULL)
		return;

	dbus->timeouts = g_slist_remove(dbus->timeouts, timeout_data);

	if (timeout_data->id > 0)
		g_source_remove(timeout_data->id);

	timeout_data->id = 0;
}


void
DBus::timeout_toggled(DBusTimeout *timeout, void *data)
{
	if (dbus_timeout_get_enabled(timeout) == TRUE)
		timeout_add(timeout, data);
	else
		timeout_remove(timeout, data);
}


void
DBus::wakeup(void *data)
{
	DBus *dbus = (DBus*) data;
	g_main_context_wakeup(dbus->context);
}


void
DBus::connection_setup(GMainContext *context)
{
  if (context == NULL)
    context = g_main_context_default();

	context = g_main_context_ref(context);

	queue = g_source_new(&queue_funcs, sizeof(QueueData));
	((QueueData*)queue)->connection = connection;

	g_source_attach(queue, context);

  dbus_connection_set_watch_functions(connection,
                                      watch_add,
                                      watch_remove,
                                      watch_toggled,
                                      this, NULL);
  
  dbus_connection_set_timeout_functions(connection,
                                        timeout_add,
                                        timeout_remove,
                                        timeout_toggled,
                                        this, NULL);
  
  dbus_connection_set_wakeup_main_function(connection,
                                           wakeup,
                                           this, NULL);
}


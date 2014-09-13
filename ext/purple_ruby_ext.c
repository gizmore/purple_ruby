/*
 * Author: yong@intridea.com
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 *
 */

#include <libpurple/account.h>
#include <libpurple/accountopt.h>
#include <libpurple/conversation.h>
#include <libpurple/core.h>
#include <libpurple/debug.h>
#include <libpurple/cipher.h>
#include <libpurple/eventloop.h>
#include <libpurple/ft.h>
#include <libpurple/log.h>
#include <libpurple/notify.h>
#include <libpurple/prefs.h>
#include <libpurple/prpl.h>
#include <libpurple/pounce.h>
#include <libpurple/request.h>
#include <libpurple/savedstatuses.h>
#include <libpurple/sound.h>
#include <libpurple/status.h>
#include <libpurple/util.h>
#include <libpurple/whiteboard.h>
#include <libpurple/network.h>

#include <ruby.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>

#define PURPLE_GLIB_READ_COND  (G_IO_IN | G_IO_HUP | G_IO_ERR)
#define PURPLE_GLIB_WRITE_COND (G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL)

typedef struct _PurpleGLibIOClosure {
  PurpleInputFunction function;
  guint result;
  gpointer data;
} PurpleGLibIOClosure;

static void purple_glib_io_destroy(gpointer data)
{
  g_free(data);
}

static gboolean purple_glib_io_invoke(GIOChannel *source, GIOCondition condition, gpointer data)
{
  PurpleGLibIOClosure *closure = data;
  PurpleInputCondition purple_cond = 0;

  if (condition & PURPLE_GLIB_READ_COND)
    purple_cond |= PURPLE_INPUT_READ;
  if (condition & PURPLE_GLIB_WRITE_COND)
    purple_cond |= PURPLE_INPUT_WRITE;

  closure->function(closure->data, g_io_channel_unix_get_fd(source),
        purple_cond);

  return TRUE;
}

static guint glib_input_add(gint fd, PurpleInputCondition condition, PurpleInputFunction function,
                 gpointer data)
{
  PurpleGLibIOClosure *closure = g_new0(PurpleGLibIOClosure, 1);
  GIOChannel *channel;
  GIOCondition cond = 0;

  closure->function = function;
  closure->data = data;

  if (condition & PURPLE_INPUT_READ)
    cond |= PURPLE_GLIB_READ_COND;
  if (condition & PURPLE_INPUT_WRITE)
    cond |= PURPLE_GLIB_WRITE_COND;

  channel = g_io_channel_unix_new(fd);
  closure->result = g_io_add_watch_full(channel, G_PRIORITY_DEFAULT, cond,
                purple_glib_io_invoke, closure, purple_glib_io_destroy);

  g_io_channel_unref(channel);
  return closure->result;
}

static PurpleEventLoopUiOps glib_eventloops =
{
  g_timeout_add,
  g_source_remove,
  glib_input_add,
  g_source_remove,
  NULL,
#if GLIB_CHECK_VERSION(2,14,0)
  g_timeout_add_seconds,
#else
  NULL,
#endif

  /* padding */
  NULL,
  NULL,
  NULL
};

static VALUE cPurpleRuby, cProtocol, cConversation;
VALUE cAccount;
const char* UI_ID = "purplegw";
static GMainLoop *main_loop = NULL;
static GHashTable* data_hash_table = NULL;
static GHashTable* fd_hash_table = NULL;
ID CALL, USER_DIR, DEBUG;
extern PurpleAccountUiOps account_ops;

static VALUE im_handler = Qnil;
static VALUE signed_on_handler = Qnil;
static VALUE connection_error_handler = Qnil;
static VALUE notify_message_handler = Qnil;
static VALUE request_handler = Qnil;
static VALUE ipc_handler = Qnil;
static VALUE timer_handler = Qnil;
guint timer_timeout = 0;
VALUE new_buddy_handler = Qnil;

extern void
finch_connection_report_disconnect(PurpleConnection *gc, PurpleConnectionError reason,
    const char *text);

extern void finch_connections_init();

VALUE inspect_rb_obj(VALUE obj)
{
  return rb_funcall(obj, rb_intern("inspect"), 0, 0);
}

void set_callback(VALUE* handler, const char* handler_name)
{
  if (!rb_block_given_p()) {
    rb_raise(rb_eArgError, "%s: no block", handler_name);
  }

  if (Qnil != *handler) {
    rb_raise(rb_eArgError, "%s should only be assigned once", handler_name);
  }

  *handler = rb_block_proc();
  /*
  * If you create a Ruby object from C and store it in a C global variable without
  * exporting it to Ruby, you must at least tell the garbage collector about it,
  * lest ye be reaped inadvertently:
  */
  rb_global_variable(handler);

  if (rb_obj_class(*handler) != rb_cProc) {
    rb_raise(rb_eTypeError, "%s got unexpected value: %s", handler_name,
       RSTRING_PTR(inspect_rb_obj(*handler)));
  }
}

void check_callback(VALUE handler, const char* handler_name){
  if (rb_obj_class(handler) != rb_cProc) {
    rb_raise(rb_eTypeError, "%s has unexpected value: %s",
      handler_name,
      RSTRING_PTR(inspect_rb_obj(handler)));
  }
}

void report_disconnect(PurpleConnection *gc, PurpleConnectionError reason, const char *text)
{
  if (Qnil != connection_error_handler) {
    VALUE args[3];
    args[0] = Data_Wrap_Struct(cAccount, NULL, NULL, purple_connection_get_account(gc));
    args[1] = INT2FIX(reason);
    args[2] = rb_str_new2(text);
    check_callback(connection_error_handler, "connection_error_handler");
    VALUE v = rb_funcall2(connection_error_handler, CALL, 3, args);

    if (v != Qnil && v != Qfalse) {
      finch_connection_report_disconnect(gc, reason, text);
    }
  }
}

static void* notify_message(PurpleNotifyMsgType type,
  const char *title,
  const char *primary,
  const char *secondary)
{
  if (notify_message_handler != Qnil) {
    VALUE args[4];
    args[0] = INT2FIX(type);
    args[1] = rb_str_new2(NULL == title ? "" : title);
    args[2] = rb_str_new2(NULL == primary ? "" : primary);
    args[3] = rb_str_new2(NULL == secondary ? "" : secondary);
    check_callback(notify_message_handler, "notify_message_handler");
    rb_funcall2(notify_message_handler, CALL, 4, args);
  }

  return NULL;
}

static void write_conv(PurpleConversation *conv, const char *who, const char *alias,
      const char *message, PurpleMessageFlags flags, time_t mtime)
{
  if (im_handler != Qnil) {
    PurpleAccount* account = purple_conversation_get_account(conv);
    if (strcmp(purple_account_get_protocol_id(account), "prpl-msn") == 0 &&
         (strstr(message, "Message could not be sent") != NULL ||
          strstr(message, "Message was not sent") != NULL ||
          strstr(message, "Message may have not been sent") != NULL
         )
        ) {
      /* I have seen error like 'msn: Connection error from Switchboard server'.
       * In that case, libpurple will notify user with two regular im message.
       * The first message is an error message, the second one is the original message that failed to send.
       */
      notify_message(PURPLE_CONNECTION_ERROR_NETWORK_ERROR, message, purple_account_get_protocol_id(account), who);
    } else {
      VALUE args[3];
      args[0] = Data_Wrap_Struct(cAccount, NULL, NULL, account);
      args[1] = rb_str_new2(who);
      args[2] = rb_str_new2(message);
      check_callback(im_handler, "im_handler");
      rb_funcall2(im_handler, CALL, 3, args);
    }
  }
}

static PurpleConversationUiOps conv_uiops =
{
  NULL,                      /* create_conversation  */
  NULL,                      /* destroy_conversation */
  NULL,                      /* write_chat           */
  NULL,                      /* write_im             */
  write_conv,           /* write_conv           */
  NULL,                      /* chat_add_users       */
  NULL,                      /* chat_rename_user     */
  NULL,                      /* chat_remove_users    */
  NULL,                      /* chat_update_user     */
  NULL,                      /* present              */
  NULL,                      /* has_focus            */
  NULL,                      /* custom_smiley_add    */
  NULL,                      /* custom_smiley_write  */
  NULL,                      /* custom_smiley_close  */
  NULL,                      /* send_confirm         */
  NULL,
  NULL,
  NULL,
  NULL
};

static PurpleConnectionUiOps connection_ops =
{
  NULL, /* connect_progress */
  NULL, /* connected */
  NULL, /* disconnected */
  NULL, /* notice */
  NULL,
  NULL, /* network_connected */
  NULL, /* network_disconnected */
  report_disconnect,
  NULL,
  NULL,
  NULL
};

static void* request_action(const char *title, const char *primary, const char *secondary,
                            int default_action,
                            PurpleAccount *account,
                            const char *who,
                            PurpleConversation *conv,
                            void *user_data,
                            size_t action_count,
                            va_list actions)
{
  if (request_handler != Qnil) {
    VALUE args[4];
    args[0] = rb_str_new2(NULL == title ? "" : title);
    args[1] = rb_str_new2(NULL == primary ? "" : primary);
    args[2] = rb_str_new2(NULL == secondary ? "" : secondary);
    args[3] = rb_str_new2(NULL == who ? "" : who);
    check_callback(request_handler, "request_handler");
    VALUE v = rb_funcall2(request_handler, CALL, 4, args);

    if (v != Qnil && v != Qfalse) {
      /*const char *text =*/ va_arg(actions, const char *);
      GCallback ok_cb = va_arg(actions, GCallback);
      ((PurpleRequestActionCb)ok_cb)(user_data, default_action);
    }
  }

  return NULL;
}

static PurpleRequestUiOps request_ops =
{
  NULL,           /*request_input*/
  NULL,           /*request_choice*/
  request_action, /*request_action*/
  NULL,           /*request_fields*/
  NULL,           /*request_file*/
  NULL,           /*close_request*/
  NULL,           /*request_folder*/
  NULL,
  NULL,
  NULL,
  NULL
};

static PurpleNotifyUiOps notify_ops =
{
  notify_message, /*notify_message*/
  NULL,           /*notify_email*/
  NULL,           /*notify_emails*/
  NULL,           /*notify_formatted*/
  NULL,           /*notify_searchresults*/
  NULL,           /*notify_searchresults_new_rows*/
  NULL,           /*notify_userinfo*/
  NULL,           /*notify_uri*/
  NULL,           /*close_notify*/
  NULL,
  NULL,
  NULL,
  NULL,
};

static PurpleCoreUiOps core_uiops =
{
  NULL,
  NULL,
  NULL,
  NULL,

  /* padding */
  NULL,
  NULL,
  NULL,
  NULL
};

//I have tried to detect Ctrl-C using ruby's trap method,
//but it does not work as expected: it can not detect Ctrl-C
//until a network event occurs
static void sighandler(int sig)
{
  switch (sig) {
  case SIGINT:
  case SIGQUIT:
  case SIGTERM:
    if (main_loop)
    {
      g_main_loop_quit(main_loop);
    }
    break;
  }
}

static VALUE init(int argc, VALUE *argv, VALUE self)
{
  VALUE debug, path, settings;

  rb_scan_args(argc, argv, "01", &settings);

  if (argc == 0) {
    debug = Qnil;
    path = Qnil;
  }
  else {
    settings = rb_convert_type(settings, T_HASH, "Hash", "to_hash");

    debug = rb_hash_aref(settings, ID2SYM(DEBUG));
    path = rb_hash_aref(settings, ID2SYM(USER_DIR));
  }

//  signal(SIGCHLD, SIG_IGN);
//  signal(SIGPIPE, SIG_IGN);
//  signal(SIGINT, sighandler);
//  signal(SIGQUIT, sighandler);
//  signal(SIGTERM, sighandler);

  data_hash_table = g_hash_table_new(NULL, NULL);
  fd_hash_table = g_hash_table_new(NULL, NULL);

  purple_debug_set_enabled(RTEST(debug) ? TRUE : FALSE);

  if (path != Qnil) {
    purple_util_set_user_dir(StringValueCStr(path));
  }

  purple_core_set_ui_ops(&core_uiops);
  purple_eventloop_set_ui_ops(&glib_eventloops);

  if (!purple_core_init(UI_ID)) {
    rb_raise(rb_eRuntimeError, "libpurple initialization failed");
  }

  /* Create and load the buddylist. */
  purple_set_blist(purple_blist_new());
  purple_blist_load();

  /* Load the preferences. */
  purple_prefs_load();

  /* Load the pounces. */
  purple_pounces_load();

  return Qnil;
}

static VALUE watch_incoming_im(VALUE self)
{
  purple_conversations_set_ui_ops(&conv_uiops);
  set_callback(&im_handler, "im_handler");
  return im_handler;
}

static VALUE watch_notify_message(VALUE self)
{
  purple_notify_set_ui_ops(&notify_ops);
  set_callback(&notify_message_handler, "notify_message_handler");
  return notify_message_handler;
}

static VALUE watch_request(VALUE self)
{
  purple_request_set_ui_ops(&request_ops);
  set_callback(&request_handler, "request_handler");
  return request_handler;
}

static VALUE watch_new_buddy(VALUE self)
{
  purple_accounts_set_ui_ops(&account_ops);
  set_callback(&new_buddy_handler, "new_buddy_handler");
  return new_buddy_handler;
}

static void signed_on(PurpleConnection* connection)
{
  VALUE args[1];
  args[0] = Data_Wrap_Struct(cAccount, NULL, NULL, purple_connection_get_account(connection));
  check_callback(signed_on_handler, "signed_on_handler");
  rb_funcall2(signed_on_handler, CALL, 1, args);
}

static VALUE watch_signed_on_event(VALUE self)
{
  set_callback(&signed_on_handler, "signed_on_handler");
  int handle;
  purple_signal_connect(purple_connections_get_handle(), "signed-on", &handle,
        PURPLE_CALLBACK(signed_on), NULL);
  return signed_on_handler;
}

static VALUE watch_connection_error(VALUE self)
{
  finch_connections_init();
  purple_connections_set_ui_ops(&connection_ops);

  set_callback(&connection_error_handler, "connection_error_handler");

  /*int handle;
  purple_signal_connect(purple_connections_get_handle(), "connection-error", &handle,
        PURPLE_CALLBACK(connection_error), NULL);*/
  return connection_error_handler;
}

static void _read_socket_handler(gpointer notused, int socket, PurpleInputCondition condition)
{
  char message[4096] = {0};
  int i = recv(socket, message, sizeof(message) - 1, 0);
  if (i > 0) {
    purple_debug_info("purple_ruby", "recv %d: %d\n", socket, i);

    gpointer str = g_hash_table_lookup(data_hash_table, (gpointer)socket);
    if (NULL == str) rb_raise(rb_eRuntimeError, "can not find socket: %d", socket);
    rb_str_append((VALUE)str, rb_str_new2(message));
  } else {
    purple_debug_info("purple_ruby", "close connection %d: %d %d\n", socket, i, errno);

    gpointer str = g_hash_table_lookup(data_hash_table, (gpointer)socket);
    if (NULL == str) {
      purple_debug_warning("purple_ruby", "can not find socket in data_hash_table %d\n", socket);
      return;
    }

    gpointer purple_fd = g_hash_table_lookup(fd_hash_table, (gpointer)socket);
    if (NULL == purple_fd) {
      purple_debug_warning("purple_ruby", "can not find socket in fd_hash_table %d\n", socket);
      return;
    }

    g_hash_table_remove(fd_hash_table, (gpointer)socket);
    g_hash_table_remove(data_hash_table, (gpointer)socket);
    purple_input_remove((guint)purple_fd);
    close(socket);

    VALUE args[1];
    args[0] = (VALUE)str;
    check_callback(ipc_handler, "ipc_handler");
    rb_funcall2(ipc_handler, CALL, 1, args);
  }
}

static void _accept_socket_handler(gpointer notused, int server_socket, PurpleInputCondition condition)
{
  /* Check that it is a read condition */
  if (condition != PURPLE_INPUT_READ)
    return;

  struct sockaddr_in their_addr; /* connector's address information */
  socklen_t sin_size = sizeof(struct sockaddr);
  int client_socket;
  if ((client_socket = accept(server_socket, (struct sockaddr *)&their_addr, &sin_size)) == -1) {
    purple_debug_warning("purple_ruby", "failed to accept %d: %d\n", client_socket, errno);
    return;
  }

  int flags = fcntl(client_socket, F_GETFL);
  fcntl(client_socket, F_SETFL, flags | O_NONBLOCK);
#ifndef _WIN32
  fcntl(client_socket, F_SETFD, FD_CLOEXEC);
#endif

  purple_debug_info("purple_ruby", "new connection: %d\n", client_socket);

  guint purple_fd = purple_input_add(client_socket, PURPLE_INPUT_READ, _read_socket_handler, NULL);

  g_hash_table_insert(data_hash_table, (gpointer)client_socket, (gpointer)rb_str_new2(""));
  g_hash_table_insert(fd_hash_table, (gpointer)client_socket, (gpointer)purple_fd);
}

static VALUE watch_incoming_ipc(VALUE self, VALUE serverip, VALUE port)
{
  struct sockaddr_in my_addr;
  int soc;
  int on = 1;

  /* Open a listening socket for incoming conversations */
  if ((soc = socket(PF_INET, SOCK_STREAM, 0)) < 0)
  {
    rb_raise(rb_eRuntimeError, "Cannot open socket: %s\n", g_strerror(errno));
    return Qnil;
  }

  if (setsockopt(soc, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
  {
    rb_raise(rb_eRuntimeError, "SO_REUSEADDR failed: %s\n", g_strerror(errno));
    return Qnil;
  }

  memset(&my_addr, 0, sizeof(struct sockaddr_in));
  my_addr.sin_family = AF_INET;
  my_addr.sin_addr.s_addr = inet_addr(StringValueCStr(serverip));
  my_addr.sin_port = htons(FIX2INT(port));
  if (bind(soc, (struct sockaddr*)&my_addr, sizeof(struct sockaddr)) != 0)
  {
    rb_raise(rb_eRuntimeError, "Unable to bind to port %d: %s\n", (int)FIX2INT(port), g_strerror(errno));
    return Qnil;
  }

  /* Attempt to listen on the bound socket */
  if (listen(soc, 10) != 0)
  {
    rb_raise(rb_eRuntimeError, "Cannot listen on socket: %s\n", g_strerror(errno));
    return Qnil;
  }

  set_callback(&ipc_handler, "ipc_handler");

  /* Open a watcher in the socket we have just opened */
  purple_input_add(soc, PURPLE_INPUT_READ, _accept_socket_handler, NULL);

  return port;
}

static gboolean
do_timeout(gpointer data)
{
  VALUE handler = data;
  check_callback(handler, "timer_handler");
  VALUE v = rb_funcall(handler, CALL, 0, 0);
  return (v == Qtrue);
}

static VALUE watch_timer(VALUE self, VALUE delay)
{
  set_callback(&timer_handler, "timer_handler");
  if (timer_timeout != 0)
    g_source_remove(timer_timeout);
  timer_timeout = g_timeout_add(delay, do_timeout, timer_handler);
  return delay;
}

static VALUE login(VALUE self, VALUE protocol, VALUE username, VALUE password)
{
  PurpleAccount* account = purple_account_new(StringValueCStr(username), StringValueCStr(protocol));
  if (NULL == account || NULL == account->presence) {
    rb_raise(rb_eRuntimeError, "No able to create account: %s", StringValueCStr(protocol));
  }

  purple_account_set_password(account, StringValueCStr(password));
  purple_account_set_remember_password(account, TRUE);
  purple_account_set_enabled(account, UI_ID, TRUE);
  PurpleSavedStatus *status = purple_savedstatus_new(NULL, PURPLE_STATUS_AVAILABLE);
  purple_savedstatus_activate(status);

  return Data_Wrap_Struct(cAccount, NULL, NULL, account);
}

static VALUE main_loop_run(VALUE self)
{
  main_loop = g_main_loop_new(NULL, FALSE);
  g_main_loop_run(main_loop);

#ifdef DEBUG_MEM_LEAK
  printf("QUIT!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
  purple_core_quit();
  if (im_handler == Qnil) rb_gc_unregister_address(&im_handler);
  if (signed_on_handler == Qnil) rb_gc_unregister_address(&signed_on_handler);
  if (connection_error_handler == Qnil) rb_gc_unregister_address(&connection_error_handler);
  if (notify_message_handler == Qnil) rb_gc_unregister_address(&notify_message_handler);
  if (request_handler == Qnil) rb_gc_unregister_address(&request_handler);
  if (ipc_handler == Qnil) rb_gc_unregister_address(&ipc_handler);
  if (timer_timeout != 0) g_source_remove(timer_timeout);
  if (timer_handler == Qnil) rb_gc_unregister_address(&timer_handler);
  if (new_buddy_handler == Qnil) rb_gc_unregister_address(&new_buddy_handler);
  rb_gc_start();
#endif

  return Qnil;
}

static VALUE main_loop_stop(VALUE self)
{
  g_main_loop_quit(main_loop);
  return Qnil;
}

static VALUE main_loop_step(VALUE self)
{
  if (g_main_pending())
  {
    g_main_context_iteration(NULL, 0);
  }
  return Qnil;
}


/*
 * call-seq:
 * send_im(contact, message)
 *
 * Sends an IM message.
 *
 */
static VALUE send_im(VALUE self, VALUE name, VALUE message)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);

  if (purple_account_is_connected(account)) {
    int i = serv_send_im(purple_account_get_connection(account), StringValueCStr(name), StringValueCStr(message), 0);
    return INT2FIX(i);
  } else {
    return Qnil;
  }
}

/*
 * call-seq:
 * chat_send(chat_id, message)
 *
 * Send an message to chat
 */
static VALUE chat_send(VALUE self, VALUE id, VALUE message) 
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);

  if (purple_account_is_connected(account)) {
    int i = serv_chat_send(purple_account_get_connection(account), rb_Integer(id), StringValueCStr(message), 0);
    return INT2FIX(i);
  } else {
    return Qnil;
  }
}

/* 
 * call-seq:
 * find_conversation(name)
 *
 * Find conversation with given name
 */
static VALUE account_find_conversation(VALUE self, VALUE name)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  
  if (purple_account_is_connected(account)) {
    PurpleConversation *conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_ANY, StringValueCStr(name), account);

    if (conv != NULL) {
      VALUE conversation = Data_Wrap_Struct(cConversation, NULL, NULL, conv);

      return conversation;
    }
  }
    
  return Qnil;
}

/*
 * call-seq:
 * join_chat(chat_id)
 *
 * Join chat
 */
static VALUE account_join_chat(VALUE self, VALUE name)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  GHashTable *data;

  if (purple_account_is_connected(account)) {
    data = g_hash_table_new(g_str_hash, g_str_equal);

    g_hash_table_insert(data, "chat_id", StringValueCStr(name));

    serv_join_chat(purple_account_get_connection(account), data);

    g_hash_table_destroy(data);

    return Qtrue;
  }

  return Qfalse;
}

/*
 * Returns account username (String).
 *
 */
static VALUE username(VALUE self)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  return rb_str_new2(purple_account_get_username(account));
}

static VALUE protocol_id(VALUE self)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  return rb_str_new2(purple_account_get_protocol_id(account));
}

static VALUE protocol_name(VALUE self)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  return rb_str_new2(purple_account_get_protocol_name(account));
}

static VALUE get_bool_setting(VALUE self, VALUE name, VALUE default_value)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  gboolean value = purple_account_get_bool(account, StringValueCStr(name), RTEST(default_value) ? TRUE : FALSE);
  return (TRUE == value) ? Qtrue : Qfalse;
}

static VALUE set_bool_setting(VALUE self, VALUE name, VALUE value)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  purple_account_set_bool(account, StringValueCStr(name), RTEST(value) ? TRUE : FALSE);
  return value;
}

static VALUE get_int_setting(VALUE self, VALUE name, VALUE default_value)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  return INT2FIX(purple_account_get_int(account, StringValueCStr(name), FIX2INT(default_value)));
}

static VALUE set_int_setting(VALUE self, VALUE name, VALUE value)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  purple_account_set_int(account, StringValueCStr(name), FIX2INT(value));
  return value;
}

static VALUE get_string_setting(VALUE self, VALUE name, VALUE default_value)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  const char* value = purple_account_get_string(account, StringValueCStr(name), StringValueCStr(default_value));
  return (NULL == value) ? Qnil : rb_str_new2(value);
}

static VALUE set_string_setting(VALUE self, VALUE name, VALUE value)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  purple_account_set_string(account, StringValueCStr(name), StringValueCStr(value));
  return value;
}

/*
 * call-seq:
 * set_public_alias(nickname)
 *
 * Sets the public alias(nickname) of the account.
 *
 */
static VALUE set_public_alias(VALUE self, VALUE value)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  purple_account_set_public_alias(account, StringValueCStr(value), NULL, NULL);
  return value;
}

/*
 * Gets the public alias(nickname) of the account.
 * TODO: async
static VALUE get_public_alias(VALUE self)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  purple_account_get_public_alias(account, NULL, NULL);
  return Qnil;
}
 */

static VALUE list_protocols(VALUE self)
{
  VALUE array = rb_ary_new();

  GList *iter = purple_plugins_get_protocols();
  for (; iter; iter = iter->next) {
    VALUE protocol = Data_Wrap_Struct(cProtocol, NULL, NULL, iter->data);
    rb_ary_push(array, protocol);
  }

  return array;
}

static VALUE list_conversations(VALUE self)
{
  VALUE array = rb_ary_new();
  
  GList *iter = purple_get_conversations();
  for (; iter; iter = iter->next) {
    VALUE conversation = Data_Wrap_Struct(cConversation, NULL, NULL, iter->data);
    rb_ary_push(array, conversation);
  }

  return array;
}

static VALUE conversation_get_title(VALUE self) 
{
  PurpleConversation *conversation;
  Data_Get_Struct(self, PurpleConversation, conversation);
  const char* title = purple_conversation_get_title(conversation);

  return rb_str_new2(title);
}

static VALUE conversation_get_name(VALUE self)
{
  PurpleConversation *conversation;
  Data_Get_Struct(self, PurpleConversation, conversation);
  const char* name = purple_conversation_get_name(conversation);

  return rb_str_new2(name);
}

static VALUE conversation_get_chat_id(VALUE self)
{
  PurpleConversation *conversation;
  Data_Get_Struct(self, PurpleConversation, conversation);
  PurpleConvChat *chat_data = PURPLE_CONV_CHAT(conversation);

  if (chat_data != NULL) {
    int id = purple_conv_chat_get_id(chat_data);

    return INT2FIX(id);
  }

  return Qnil;
}

static VALUE conversation_im_send(VALUE self, VALUE message) 
{
  PurpleConversation *conversation;
  Data_Get_Struct(self, PurpleConversation, conversation);
  PurpleConvIm *im_data =  PURPLE_CONV_IM(conversation);

  if (im_data == NULL) {
    return Qnil;
  }
  else {
    purple_conv_im_send(im_data, StringValueCStr(message));
    return Qtrue;
  }
}

static VALUE conversation_get_type(VALUE self) 
{
  PurpleConversation *conversation;
  Data_Get_Struct(self, PurpleConversation, conversation);

  return INT2FIX(purple_conversation_get_type(conversation));
}

static VALUE conversation_chat_send(VALUE self, VALUE message)
{
  PurpleConversation *conversation;
  Data_Get_Struct(self, PurpleConversation, conversation);
  PurpleConvChat *chat_data =  PURPLE_CONV_CHAT(conversation);

  if (chat_data == NULL) {
    return Qnil;
  }
  else {
    purple_conv_chat_send(chat_data, StringValueCStr(message));
    return Qtrue;
  }
}

static VALUE add_buddy(VALUE self, VALUE buddy)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);

  PurpleBuddy* pb = purple_buddy_new(account, StringValueCStr(buddy), NULL);

  char* group = _("Buddies");
  PurpleGroup* grp = purple_find_group(group);
  if (!grp)
  {
    grp = purple_group_new(group);
    purple_blist_add_group(grp, NULL);
  }

  purple_blist_add_buddy(pb, NULL, grp, NULL);
  purple_account_add_buddy(account, pb);
  return Qtrue;
}

static VALUE remove_buddy(VALUE self, VALUE buddy)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);

  PurpleBuddy* pb = purple_find_buddy(account, StringValueCStr(buddy));
  if (NULL == pb) {
    rb_raise(rb_eRuntimeError, "Failed to remove buddy for %s : %s does not exist", purple_account_get_username(account), StringValueCStr(buddy));
  }

  char* group = _("Buddies");
  PurpleGroup* grp = purple_find_group(group);
  if (!grp)
    {
      grp = purple_group_new(group);
      purple_blist_add_group(grp, NULL);
    }

  purple_blist_remove_buddy(pb);
  purple_account_remove_buddy(account, pb, grp);

  return Qtrue;
}

static VALUE has_buddy(VALUE self, VALUE buddy)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  if (purple_find_buddy(account, StringValueCStr(buddy)) != NULL) {
    return Qtrue;
  } else {
    return Qfalse;
  }
}

static VALUE acc_delete(VALUE self)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  purple_accounts_delete(account);
  return Qnil;
}

static VALUE protocol_to_s(VALUE self)
{
  PurplePlugin *protocol;
  PurplePluginInfo *info;

  Data_Get_Struct(self, PurplePlugin, protocol);
  info = protocol->info;
  if (info && info->name) {
    char s[256];
    snprintf(s, sizeof(s) -1, "%s %s", info->id, info->name);
    return rb_str_new2(s);
  }

  return Qnil;
}

static VALUE protocol_get_id(VALUE self)
{
  PurplePlugin *protocol;
  PurplePluginInfo *info;

  Data_Get_Struct(self, PurplePlugin, protocol);
  info = protocol->info;
  return info && info->id ? rb_str_new2(info->id) : Qnil;
}

static VALUE protocol_get_name(VALUE self)
{
  PurplePlugin *protocol;
  PurplePluginInfo *info;

  Data_Get_Struct(self, PurplePlugin, protocol);
  info = protocol->info;
  return info && info->name ? rb_str_new2(info->name) : Qnil;
}

static VALUE protocol_get_default_options(VALUE self)
{
  PurplePlugin *protocol;
  PurplePluginProtocolInfo *prpl_info;
  PurpleAccountOption *opt;
  GList *opts;
  const char *str_val;
  VALUE h = rb_hash_new(), key, val;

  Data_Get_Struct(self, PurplePlugin, protocol);
  prpl_info = PURPLE_PLUGIN_PROTOCOL_INFO(protocol);
  opts = prpl_info->protocol_options;
  for (; opts; opts = opts->next) {
    opt = (PurpleAccountOption *)opts->data;
    key = rb_str_new2(opt->pref_name);
    switch (opt->type)
      {
      case PURPLE_PREF_BOOLEAN:
  val = purple_account_option_get_default_bool(opt) ? Qtrue : Qfalse;
  break;
      case PURPLE_PREF_INT:
  val = INT2FIX(purple_account_option_get_default_int(opt));
  break;
      case PURPLE_PREF_STRING:
  str_val = purple_account_option_get_default_string(opt);
  val = str_val == NULL ? rb_str_new2("") : rb_str_new2(str_val);
  break;
      case PURPLE_PREF_STRING_LIST:
  str_val = rb_str_new2(purple_account_option_get_default_list_value(opt));
  val = str_val == NULL ? rb_str_new2("") : rb_str_new2(str_val);
  break;
      default:
  key = Qnil;
      }

    if (key != Qnil)
      rb_hash_aset(h, key, val);
  }

  return h;
}

void Init_purple_ruby_ext()
{
  CALL = rb_intern("call");
  DEBUG = rb_intern("debug");
  USER_DIR = rb_intern("user_dir");

  /*
   * Module PurpleRuby
   *
   */
  cPurpleRuby = rb_define_class("PurpleRuby", rb_cObject);
  rb_define_singleton_method(cPurpleRuby, "init", init, -1);
  rb_define_singleton_method(cPurpleRuby, "list_protocols", list_protocols, 0);
  rb_define_singleton_method(cPurpleRuby, "list_conversations", list_conversations, 0);
  rb_define_singleton_method(cPurpleRuby, "watch_signed_on_event", watch_signed_on_event, 0);
  rb_define_singleton_method(cPurpleRuby, "watch_connection_error", watch_connection_error, 0);
  rb_define_singleton_method(cPurpleRuby, "watch_incoming_im", watch_incoming_im, 0);
  rb_define_singleton_method(cPurpleRuby, "watch_notify_message", watch_notify_message, 0);
  rb_define_singleton_method(cPurpleRuby, "watch_request", watch_request, 0);
  rb_define_singleton_method(cPurpleRuby, "watch_new_buddy", watch_new_buddy, 0);
  rb_define_singleton_method(cPurpleRuby, "watch_incoming_ipc", watch_incoming_ipc, 2);
  rb_define_singleton_method(cPurpleRuby, "watch_timer", watch_timer, 1);
  rb_define_singleton_method(cPurpleRuby, "login", login, 3);
  rb_define_singleton_method(cPurpleRuby, "main_loop_run", main_loop_run, 0);
  rb_define_singleton_method(cPurpleRuby, "main_loop_stop", main_loop_stop, 0);
  rb_define_singleton_method(cPurpleRuby, "main_loop_step", main_loop_step, 0);

  rb_define_const(cPurpleRuby, "NOTIFY_MSG_ERROR", INT2NUM(PURPLE_NOTIFY_MSG_ERROR));
  rb_define_const(cPurpleRuby, "NOTIFY_MSG_WARNING", INT2NUM(PURPLE_NOTIFY_MSG_WARNING));
  rb_define_const(cPurpleRuby, "NOTIFY_MSG_INFO", INT2NUM(PURPLE_NOTIFY_MSG_INFO));

  /*
   * Class Account
   *
   */
  cAccount = rb_define_class_under(cPurpleRuby, "Account", rb_cObject);
  rb_define_method(cAccount, "send_im", send_im, 2);
  rb_define_method(cAccount, "send_chat", chat_send, 2);
  rb_define_method(cAccount, "username", username, 0);
  rb_define_method(cAccount, "protocol_id", protocol_id, 0);
  rb_define_method(cAccount, "protocol_name", protocol_name, 0);
  rb_define_method(cAccount, "find_conversation", account_find_conversation, 1);
  rb_define_method(cAccount, "join_chat", account_join_chat, 1);
  rb_define_method(cAccount, "get_bool_setting", get_bool_setting, 2);
  rb_define_method(cAccount, "set_bool_setting", set_bool_setting, 2);
  rb_define_method(cAccount, "get_int_setting", get_int_setting, 2);
  rb_define_method(cAccount, "set_int_setting", set_int_setting, 2);
  rb_define_method(cAccount, "get_string_setting", get_string_setting, 2);
  rb_define_method(cAccount, "set_string_setting", set_string_setting, 2);
  rb_define_method(cAccount, "set_public_alias", set_public_alias, 1);
  /* rb_define_method(cAccount, "get_public_alias", get_public_alias, 0); */
  rb_define_method(cAccount, "add_buddy", add_buddy, 1);
  rb_define_method(cAccount, "remove_buddy", remove_buddy, 1);
  rb_define_method(cAccount, "has_buddy?", has_buddy, 1);
  rb_define_method(cAccount, "delete", acc_delete, 0);

  /*
   * Class Protocol
   *
   */
  cProtocol = rb_define_class_under(cPurpleRuby, "Protocol", rb_cObject);
  rb_define_method(cProtocol, "default_options", protocol_get_default_options, 0);
  rb_define_method(cProtocol, "id", protocol_get_id, 0);
  rb_define_method(cProtocol, "name", protocol_get_name, 0);
  rb_define_method(cProtocol, "to_str", protocol_to_s, 0);
  rb_define_method(cProtocol, "to_s", protocol_to_s, 0);

  /*
   * Class Conversation
   *
   */
  cConversation = rb_define_class_under(cPurpleRuby, "Conversation", rb_cObject);

  rb_define_const(cConversation, "TYPE_UNKNOWN", INT2NUM(PURPLE_CONV_TYPE_UNKNOWN));
  rb_define_const(cConversation, "TYPE_IM", INT2NUM(PURPLE_CONV_TYPE_IM));
  rb_define_const(cConversation, "TYPE_CHAT", INT2NUM(PURPLE_CONV_TYPE_CHAT));
  rb_define_const(cConversation, "TYPE_MISC", INT2NUM(PURPLE_CONV_TYPE_MISC));
  rb_define_const(cConversation, "TYPE_ANY", INT2NUM(PURPLE_CONV_TYPE_ANY));

  rb_define_method(cConversation, "title", conversation_get_title, 0);
  rb_define_method(cConversation, "to_s", conversation_get_title, 0);
  rb_define_method(cConversation, "name", conversation_get_name, 0);
  rb_define_method(cConversation, "chat_id", conversation_get_chat_id, 0);
  rb_define_method(cConversation, "im_send", conversation_im_send, 1);
  rb_define_method(cConversation, "chat_send", conversation_chat_send, 1);
  rb_define_method(cConversation, "type", conversation_get_type, 0);
}

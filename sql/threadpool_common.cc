/* Copyright (C) 2012 Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include <my_global.h>
#include <violite.h>
#include <sql_class.h>
#include <sql_connect.h>
#include <sql_audit.h>
#include <debug_sync.h>
#include <threadpool.h>
#include <probes_mysql.h>
#include <my_thread_local.h>
#include <mysql/psi/mysql_idle.h>
#include <conn_handler/channel_info.h>
#include <conn_handler/connection_handler_manager.h>
#include <mysqld_thd_manager.h>
#include <mysql/thread_pool_priv.h>

/* Threadpool parameters */

uint threadpool_min_threads;
uint threadpool_idle_timeout;
uint threadpool_size;
uint threadpool_stall_limit;
uint threadpool_max_threads;
uint threadpool_oversubscribe;

/* Stats */
TP_STATISTICS tp_stats;


extern bool do_command(THD*);

/*
  Worker threads contexts, and THD contexts.
  =========================================
  
  Both worker threads and connections have their sets of thread local variables 
  At the moment it is mysys_var (this has specific data for dbug, my_error and 
  similar goodies), and PSI per-client structure.

  Whenever query is executed following needs to be done:

  1. Save worker thread context.
  2. Change TLS variables to connection specific ones using thread_attach(THD*).
     This function does some additional work.
  3. Process query
  4. Restore worker thread context.

  Connection login and termination follows similar schema w.r.t saving and 
  restoring contexts. 

  For both worker thread, and for the connection, mysys variables are created 
  using my_thread_init() and freed with my_thread_end().

*/
struct Worker_thread_context
{
  PSI_thread *psi_thread;
#ifndef DBUG_OFF
  my_thread_id thread_id; //  TODO laurynas my_thread_var_dbug?
#endif

  void save()
  {
#ifdef HAVE_PSI_THREAD_INTERFACE
    psi_thread= PSI_THREAD_CALL(get_thread)();
#endif
#ifndef DBUG_OFF
    thread_id= my_thread_var_id();
#endif
  }

  void restore()
  {
#ifdef HAVE_PSI_THREAD_INTERFACE
    PSI_THREAD_CALL(set_thread)(psi_thread);
#endif
#ifndef DBUG_OFF
    set_my_thread_var_id(thread_id);
#endif
    pthread_setspecific(THR_THD, 0);
    pthread_setspecific(THR_MALLOC, 0);
  }
};


/*
  Attach/associate the connection with the OS thread,
*/
static bool thread_attach(THD* thd)
{
#ifndef DBUG_OFF
  set_my_thread_var_id(thd->thread_id()); // TODO laurynas dbug
#endif
  thd->thread_stack=(char*)&thd;
  thd->store_globals();
#ifdef HAVE_PSI_THREAD_INTERFACE
  PSI_THREAD_CALL(set_thread)(thd->event_scheduler.m_psi);
#endif
  mysql_socket_set_thread_owner(thd->get_protocol_classic()->get_vio()
                                ->mysql_socket);
  return 0;
}

#ifdef HAVE_PSI_STATEMENT_INTERFACE
extern PSI_statement_info stmt_info_new_packet;
#endif

void threadpool_net_before_header_psi_noop(struct st_net * /* net */,
                                           void * /* user_data */,
                                           size_t /* count */)
{ }

void threadpool_net_after_header_psi(struct st_net *net, void *user_data,
                                     size_t /* count */, my_bool rc)
{
  THD *thd;
  thd= static_cast<THD*> (user_data);
  DBUG_ASSERT(thd != NULL);

  if (thd->m_server_idle)
  {
    /*
      The server just got data for a network packet header,
      from the network layer.
      The IDLE event is now complete, since we now have a message to process.
      We need to:
      - start a new STATEMENT event
      - start a new STAGE event, within this statement,
      - start recording SOCKET WAITS events, within this stage.
      The proper order is critical to get events numbered correctly,
      and nested in the proper parent.
    */
    MYSQL_END_IDLE_WAIT(thd->m_idle_psi);

    if (! rc)
    {
      thd->m_statement_psi= MYSQL_START_STATEMENT(&thd->m_statement_state,
                                                  stmt_info_new_packet.m_key,
                                                  thd->db().str,
                                                  thd->db().length,
                                                  thd->charset(), NULL);

      THD_STAGE_INFO(thd, stage_init);
    }

    /*
      TODO: consider recording a SOCKET event for the bytes just read,
      by also passing count here.
    */
    MYSQL_SOCKET_SET_STATE(net->vio->mysql_socket, PSI_SOCKET_STATE_ACTIVE);

    thd->m_server_idle = false;
  }
}

void threadpool_init_net_server_extension(THD *thd)
{
// TODO laurynas duplicated parts with init_net_server_extension?
#ifdef HAVE_PSI_INTERFACE
  /* Start with a clean state for connection events. */
  thd->m_idle_psi= NULL;
  thd->m_statement_psi= NULL;
  thd->m_server_idle= false;
  /* Hook up the NET_SERVER callback in the net layer. */
  thd->m_net_server_extension.m_user_data= thd;
  thd->m_net_server_extension.m_before_header= threadpool_net_before_header_psi_noop;
  thd->m_net_server_extension.m_after_header= threadpool_net_after_header_psi;

  /* Activate this private extension for the mysqld server. */
  thd->get_protocol_classic()->get_net()->extension=
      &thd->m_net_server_extension;
#else
  thd->get_protocol_classic()->get_net()->extension= NULL;
#endif
}

int threadpool_add_connection(THD* thd)
{
  int retval=1;
  Worker_thread_context worker_context;
  worker_context.save();

  my_thread_init();

  /* Create new PSI thread for use with the THD. */
#ifdef HAVE_PSI_THREAD_INTERFACE
  thd->event_scheduler.m_psi=
    PSI_THREAD_CALL(new_thread)(key_thread_one_connection, thd,
                                thd->thread_id());
#endif


  /* Login. */
  thread_attach(thd);
  thd->start_utime= thd->thr_create_utime= my_micro_time();

  if (thd->store_globals())
  {
    close_connection(thd, ER_OUT_OF_RESOURCES);
    goto end;
  }

  if (thd_prepare_connection(thd, false))
  {
    goto end;
  }

  /*
    Check if THD is ok, as prepare_new_connection_state()
    can fail, for example if init command failed.
  */
  if (thd_connection_alive(thd))
  {
    retval= 0;
    thd_set_net_read_write(thd, 1);
    thd->skip_wait_timeout= true;
    MYSQL_SOCKET_SET_STATE(thd->get_protocol_classic()->get_vio()->mysql_socket,
                           PSI_SOCKET_STATE_IDLE);
    thd->m_server_idle= true;
    threadpool_init_net_server_extension(thd);
  }

end:
  if (retval)
  {
    Connection_handler_manager *handler_manager=
      Connection_handler_manager::get_instance();
    handler_manager->inc_aborted_connects();
  }
  worker_context.restore();
  return retval;
}


void threadpool_remove_connection(THD *thd)
{

  Worker_thread_context worker_context;
  worker_context.save();

  thread_attach(thd);
  thd_set_net_read_write(thd, 0);

  end_connection(thd);
  close_connection(thd, 0);

  thd->release_resources();
  Connection_handler_manager::dec_connection_count(false);

  Global_THD_manager::get_instance()->remove_thd(thd);
  delete thd;

  worker_context.restore();
}

/**
 Process a single client request or a single batch.
*/
int threadpool_process_request(THD *thd)
{
  int retval= 0;
  Worker_thread_context  worker_context;
  worker_context.save();

  thread_attach(thd);

  if (thd->killed == THD::KILL_CONNECTION)
  {
    /* 
      killed flag was set by timeout handler 
      or KILL command. Return error.
    */
    retval= 1;
    goto end;
  }


  /*
    In the loop below, the flow is essentially the copy of thead-per-connections
    logic, see do_handle_one_connection() in sql_connect.c

    The goal is to execute a single query, thus the loop is normally executed 
    only once. However for SSL connections, it can be executed multiple times 
    (SSL can preread and cache incoming data, and vio->has_data() checks if it 
    was the case).
  */
  for(;;)
  {
    Vio *vio;
    thd_set_net_read_write(thd, 0);
    mysql_audit_release(thd);

    if ((retval= do_command(thd)) != 0)
      goto end;

    if (!thd_connection_alive(thd))
    {
      retval= 1;
      goto end;
    }

    vio= thd->get_protocol_classic()->get_vio();
    if (!vio->has_data(vio))
    { 
      /* More info on this debug sync is in sql_parse.cc*/
      DEBUG_SYNC(thd, "before_do_command_net_read");
      thd_set_net_read_write(thd, 1);
      goto end;
    }
    if (!thd->m_server_idle) {
      MYSQL_SOCKET_SET_STATE(vio->mysql_socket, PSI_SOCKET_STATE_IDLE);
      MYSQL_START_IDLE_WAIT(thd->m_idle_psi, &thd->m_idle_state);
      thd->m_server_idle= true;
    }
  }

end:
  if (!retval && !thd->m_server_idle) {
    MYSQL_SOCKET_SET_STATE(thd->get_protocol_classic()->get_vio()
                           ->mysql_socket, PSI_SOCKET_STATE_IDLE);
    MYSQL_START_IDLE_WAIT(thd->m_idle_psi, &thd->m_idle_state);
    thd->m_server_idle= true;
  }

  worker_context.restore();
  return retval;
}

THD_event_functions tp_event_functions=
{
  tp_wait_begin, tp_wait_end, tp_post_kill_notification
};
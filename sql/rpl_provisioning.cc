// FIXME - Farnham header

#include "rpl_provisioning.h"
#include "log_event.h"

provisioning_send_info::provisioning_send_info(THD *thd_arg) : thd(thd_arg)
{
}

/**
  Sends a provisioning data to slave if there are some available.

  Sets binlog_send_info::should_stop flag if all data are sent.

  @return -1 - ok, more data waiting
           0 - ok, no more data
           1 - error
 */

int8 provisioning_send_info::send_provisioning_data()
{
  static int sent= 45;

  if (sent >= 50)
    return -1;

  ++sent;

  char query[128];
  sprintf(query, "INSERT INTO test.t1 VALUES (%d)", sent);

  Query_log_event evt(thd, query, strlen(query), false, true, true, 0);

  String packet;
  IO_CACHE buffer;
  open_cached_file(&buffer, mysql_tmpdir, TEMP_PREFIX, READ_RECORD_BUFFER,
                   MYF(MY_WME));

  evt.write(&buffer);
  
  packet.set("\0", 1, &my_charset_bin);
  packet.append(&buffer, buffer.write_pos - buffer.buffer);

  // Set current server as source of event, when slave registers on master,
  // it overwrites thd->variables.server_id with its own server id - and it is
  // then used when writing event
  packet[SERVER_ID_OFFSET + 1]= global_system_variables.server_id;

  my_net_write(&thd->net, (uchar*) packet.ptr(), packet.length());
  net_flush(&thd->net);

  close_cached_file(&buffer);

  return -1;
}

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
  static bool sent = false;

  if (sent)
    return -1;

  sent = true;

  Query_log_event evt(thd, STRING_WITH_LEN("INSERT INTO t1 VALUES (99)"), false, true, true, 0);

  String packet;
  IO_CACHE buffer;
  File file;
  open_cached_file(&buffer, mysql_tmpdir, TEMP_PREFIX, READ_RECORD_BUFFER,
                   MYF(MY_WME));

  evt.write(&buffer);
  
  packet.set("\0", 1, &my_charset_bin);
  packet.append(&buffer, buffer.write_pos - buffer.buffer);


  my_net_write(&thd->net, (uchar*) packet.ptr(), packet.length());
  net_flush(&thd->net);

  return -1;
}

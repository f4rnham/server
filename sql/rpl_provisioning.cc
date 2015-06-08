// FIXME - Farnham header

#include "rpl_provisioning.h"

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
  return -1;
}

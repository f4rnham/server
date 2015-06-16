// FIXME - Farnham header

#pragma once

#include "my_global.h"
#include "sql_class.h"

/*
  Helper structure, used to store info about state of ongoing provisioning
*/
class provisioning_send_info
{
  THD *thd;

public:
  provisioning_send_info(THD *thd_arg);

  int8 send_provisioning_data_hardcoded_data_test();

  int8 send_provisioning_data();

private:
  int8 send_event(Log_event& evt);

  void ed_connection_test();
  int8 send_table_data();
};

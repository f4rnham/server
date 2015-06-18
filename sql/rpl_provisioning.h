// FIXME - Farnham header

#pragma once

#include "my_global.h"
#include "sql_class.h"

class Ed_connection;

/*
  Helper structure, used to store info about state of ongoing provisioning
*/
class provisioning_send_info
{
  THD *thd;

  Ed_connection* connection;
  /*
    List of discovered databases for provisioning, entries from this list
    are removed after they are processed
  */
  List<char> databases;
  /*
    List of discovered tables for currently provisioned database (first in
    'databases' list)
  */
  List<char> *tables;

public:
  provisioning_send_info(THD *thd_arg);
  ~provisioning_send_info();

  int8 send_provisioning_data_hardcoded_data_test();

  int8 send_provisioning_data();

private:
  int8 send_event(Log_event& evt);

  // Initialization functions
  bool build_database_list();
  bool build_table_list();

  void clear_tables_list();

  void ed_connection_test();
  int8 send_table_data();
};

// FIXME - Farnham header
// Debug only sql_print_information all around

#pragma once

#include "my_global.h"
#include "sql_class.h"

class Ed_connection;

/*
  Provisioning phases

  'Tick' mentioned in phase descriptions means one call of
  send_provisioning_data() function
*/

enum provisioning_phase
{
  PROV_PHASE_INIT= 0,      // Initial phase, fetching list of databases
                           // Executed only once

  PROV_PHASE_DB_INIT,      // Fetching list of tables + DB creation
                           // Executed for each database
                           // Processed during one tick

  PROV_PHASE_TABLES,       // Table creation + data
                           // Executed for each table
                           // Processing of single table can be split into
                           // multiple ticks for large tables - state is
                           // stored in NYI

  PROV_PHASE_TRIGGERS,     // NYI

  PROV_PHASE_EVENTS,       // NYI

  PROV_PHASE_ROUTINES,     // NYI
};

/*
  Helper structure, used to store info about state of ongoing provisioning

  If we are processing database, it is always first (<code>head()</code>) one
  from <code>databases</code> list

  If we are processing table, it is always first  (<code>head()</code>) one
  from <code>tables</code> list

  Entries from both mentioned lists are removed after their processing
  is done => fully processed entries are removed from those lists
*/

class provisioning_send_info
{
  THD *thd;

  Ed_connection* connection;
  /*
    List of discovered databases for provisioning, entries from this list
    are removed after they are processed
  */
  List<LEX_STRING> databases;
  /*
    List of discovered tables for currently provisioned database (first in
    <code>databases</code> list)
  */
  List<char> tables;

  provisioning_phase phase;

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

  void ed_connection_test();
  int8 send_table_data();
};

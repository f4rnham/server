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

  PROV_PHASE_TABLE_INIT,   // Table creation
                           // Executed for each table

  PROV_PHASE_TABLE_DATA,   // Table row data
                           // Executed for each table
                           // Sending of row data can be split into multiple
                           // ticks for large tables - state is stored in
                           // <code>row_batch_end</code>

  PROV_PHASE_TRIGGERS,     // NYI

  PROV_PHASE_EVENTS,       // NYI

  PROV_PHASE_ROUTINES,     // NYI
};

/*
  How much rows from table will be sent in one provisioning tick
  FIXME - Farnham
  Dynamic value
*/
#define PROV_ROW_BATCH_SIZE 5

/*
  Default (minimum) size for row packing buffer - 1KB
*/
#define ROW_BUFFER_DEFAULT_SIZE 0x400

/*
  Helper structure, used to store info about state of ongoing provisioning

  If we are processing database, it is always first (<code>head()</code>) one
  from <code>databases</code> list

  If we are processing table, it is always first  (<code>head()</code>) one
  from <code>tables</code> list

  Entries from both mentioned lists are removed after their processing
  is done => fully processed entries are removed from those lists

  FIXME - Farnham
  Test need for quoting / escaping of database / table names
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
  /*
    Key of next row to be dumped and sent to slave
    NULL if start from beginning
  */
  key_range *row_batch_end;
  /*
    Buffer used for packing of row data
    Initialized with default size (<code>ROW_BUFFER_DEFAULT_SIZE</code>)
    and reallocated to larger capacity on demand - size is only increased
  */
  void *row_buffer;
  size_t row_buffer_size;

  provisioning_phase phase;

public:
  /*
    Stores error code which occurred in functions defined outside of
    provisioning code

    If <code>send_table_data()</code> returns 1 (error) either
    <code>error</code> or <code>error_text</code> contains better more 
    information
  */
  int error;
  /*
    Text describing error which occurred, usually in provisioning code

    If <code>send_table_data()</code> returns 1 (error) either
    <code>error</code> or <code>error_text</code> contains better more
    information
  */
  char const *error_text;

public:
  provisioning_send_info(THD *thd_arg);
  ~provisioning_send_info();

  int8 send_provisioning_data();

private:
  int8 send_event(Log_event& evt);

  // Initialization functions
  bool build_database_list();
  bool build_table_list();

  bool send_create_database();
  bool send_create_table();

  int8 send_table_data();

  bool prepare_row_buffer(TABLE *table, uchar const *data);
};

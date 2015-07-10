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

  Succession of phases if written in loops

  PROV_PHASE_INIT // Fetches databases and views
  while (databases)
  {
    PROV_PHASE_DB_INIT // Fetches tables
    while (tables)
    {
      PROV_PHASE_TABLE_INIT
      while (table data)
      {
        PROV_PHASE_TABLE_DATA
      }
      PROV_PHASE_TRIGGERS
    }
    PROV_PHASE_EVENTS
    PROV_PHASE_ROUTINES
  }
  while (views)
  {
    PROV_PHASE_VIEWS
  }

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

  PROV_PHASE_TRIGGERS,     // Trigger creation
                           // Executed for each table
                           // Processed during one tick

  PROV_PHASE_EVENTS,       // NYI

  PROV_PHASE_ROUTINES,     // Function / procedure creation
                           // Executed for each database
                           // Processed during one tick

  PROV_PHASE_VIEWS,        // View creation
                           // Executed at end, when all table data are sent
                           // One view per tick
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
    List of discovered views for provisioning, entries from this list are
    removed after they are processed
    View names here are in fully qualified quoted form, ie `db`.`view_name`
  */
  List<DYNAMIC_STRING> views;
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
  /*
    Buffer used when custom message needs to be formatted, for example our
    error message connected with one from <code>Ed_connection</code>

    If this buffer is used, then <code>error_text</code> points to it
  */
  char error_text_buffer[2000];

public:
  provisioning_send_info(THD *thd_arg);
  ~provisioning_send_info();

  int8 send_provisioning_data();

  void format_error_message(char buffer[], size_t buffer_size);

private:
  bool send_event(Log_event& evt);
  bool event_to_packet(Log_event &evt, String &packet);

  // Initialization functions
  bool build_database_list();
  bool build_table_list();

  // Retrieving of meta-data
  bool send_create_database();
  bool send_create_table();
  bool send_table_triggers();
  bool send_create_view();
  bool send_create_routines();

  int8 send_table_data();

  bool send_done_event();

  // Utility
  bool prepare_row_buffer(TABLE *table, uchar const *data);
  bool open_table(TABLE_LIST *table_list, LEX_STRING const *database,
                  char const *table);
  void close_tables();
  bool send_query_log_event(LEX_STRING const *query, bool suppress_use= true,
                            LEX_STRING const *database= NULL);

  bool allocate_key_range(TABLE *table);
  void free_key_range();

  void record_ed_connection_error(char const *msg);
};

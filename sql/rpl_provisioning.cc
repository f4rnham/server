// FIXME - Farnham
// Header
// Memory allocations - usage of my_* functions

#include "rpl_provisioning.h"
#include "log_event.h"
#include "sql_prepare.h"
#include "sql_base.h"
#include "key.h"

provisioning_send_info::provisioning_send_info(THD *thd_arg)
  : thd(thd_arg)
  , connection(new Ed_connection(thd))
  , row_batch_end(NULL)
  , row_buffer(malloc(ROW_BUFFER_DEFAULT_SIZE))
  , row_buffer_size(ROW_BUFFER_DEFAULT_SIZE)
  , phase(PROV_PHASE_INIT)
  , error(0)
  , error_text(NULL)
{
}

provisioning_send_info::~provisioning_send_info()
{
  delete connection;

  // FIXME - Farnham
  // <code>databases</code> were allocated by thd mem root - how does it work?

  List_iterator_fast<char> it(tables);
  void* name;
  while ((name= it.next_fast()) != NULL)
    // Names (char*) were allocated by strdup
    free(name);

  // If error occurred, this may not be freed
  if (row_batch_end)
  {
    delete[] row_batch_end->key;
    delete row_batch_end;
  }

  free(row_buffer);
}

/**
  Prepares row buffer for next row data - if buffer is too small,
  reallocates it to larger size, otherwise no-op

  FIXME - Farnham
  Realloc or free + malloc - we don't care about data in buffer

  @return true  - allocation failed
          false - ok
 */

bool provisioning_send_info::prepare_row_buffer(TABLE *table,
                                                uchar const *data)
{
  size_t req_size= max_row_length(table, data);

  if (req_size > row_buffer_size)
  {
    // Allocate a bit more than we need
    row_buffer_size= size_t(req_size * 1.1f);
    row_buffer= realloc(row_buffer, row_buffer_size);
  }

  return row_buffer ? true : false;
}

bool provisioning_send_info::build_database_list()
{
  if (connection->execute_direct({ C_STRING_WITH_LEN("SHOW DATABASES") }))
    return true;

  Ed_result_set *result= connection->use_result_set();

  // We are expecting exactly one result set with one column,
  // in any other case, something went wrong
  if (!result || result->get_field_count() != 1 ||
      connection->has_next_result())
  {
    return true;
  }

  List<Ed_row>& rows= *result;
  List_iterator_fast<Ed_row> it(rows);

  for (uint32 i= 0; i < rows.elements; ++i)
  {
    Ed_row *row= it++;

    // Field count for entire result set was already checked, recheck
    // in case of internal inconsistency
    DBUG_ASSERT(row->size() == 1);

    Ed_column const *column= row->get_column(0);

    // Skip test run, mysql, information_schema and performance_schema
    // databases
    if (!strcmp(column->str, "mtr") ||
        !strcmp(column->str, INFORMATION_SCHEMA_NAME.str) ||
        !strcmp(column->str, PERFORMANCE_SCHEMA_DB_NAME.str) ||
        !strcmp(column->str, MYSQL_SCHEMA_NAME.str))
    {
      continue;
    }

    sql_print_information("Discovered database %s", column->str);
    databases.push_back(thd->make_lex_string(column->str, column->length));
  }

  return false;
}

/**
  Builds list of tables for currently provisioned database - first in
  <code>databases</code> list
 */

bool provisioning_send_info::build_table_list()
{
  // If tables for some database were fetched previously, they were all
  // processed (removed from list)
  DBUG_ASSERT(tables.is_empty());
  // There is at least one database left for provisioning
  DBUG_ASSERT(databases.elements > 0);

  DYNAMIC_STRING query;
  LEX_STRING const *database= databases.head();

  init_dynamic_string(&query, "SHOW TABLES FROM ", 0, 0);
  dynstr_append(&query, database->str);

  if (connection->execute_direct({ query.str, query.length }))
    return true;

  Ed_result_set *result= connection->use_result_set();

  // We are expecting exactly one result set with one column,
  // in any other case, something went wrong
  if (!result || result->get_field_count() != 1 ||
      connection->has_next_result())
  {
    return true;
  }

  List<Ed_row>& rows= *result;
  List_iterator_fast<Ed_row> it(rows);

  for (uint32 i= 0; i < rows.elements; ++i)
  {
    Ed_row *row= it++;

    // Field count for entire result set was already checked, recheck
    // in case of internal inconsistency
    DBUG_ASSERT(row->size() == 1);

    Ed_column const *column= row->get_column(0);

    sql_print_information("Discovered table %s.%s", database->str,
                          column->str);
    tables.push_back(strdup(column->str));
  }

  return false;
}

// Helper for conversion of Log_event into data which can be sent
// to slave with terrible implementation
void event_to_packet(Log_event &evt, String &packet)
{
  IO_CACHE buffer;
  DBUG_ASSERT(!open_cached_file(&buffer, mysql_tmpdir,
                                TEMP_PREFIX, READ_RECORD_BUFFER, MYF(MY_WME)));

  DBUG_ASSERT(!evt.write(&buffer));

  reinit_io_cache(&buffer, READ_CACHE, 0, false, false);

  packet.set("\0", 1, &my_charset_bin);
  DBUG_ASSERT(!packet.append(&buffer, buffer.write_pos - buffer.buffer));

  // Set current server as source of event, when slave registers on master,
  // it overwrites thd->variables.server_id with its own server id - and it is
  // then used when writing event
  packet[SERVER_ID_OFFSET + 1]= global_system_variables.server_id;

  close_cached_file(&buffer);
}

int8 provisioning_send_info::send_event(Log_event &evt)
{
  String packet;
  event_to_packet(evt, packet);

  return my_net_write(&thd->net, (uchar*)packet.ptr(), packet.length());
}

/**
  Creates and sends 'CREATE DATABASE' <code>Query_log_event</code> for
  first database in <code>databases</code> list

  @return false - ok
          true  - error
 */

bool provisioning_send_info::send_create_database()
{
  DBUG_ASSERT(!databases.is_empty());

  DYNAMIC_STRING query;
  LEX_STRING const *database= databases.head();

  // FIXME - Farnham
  // Quoting / escaping
  init_dynamic_string(&query, "SHOW CREATE DATABASE ", 0, 0);
  dynstr_append(&query, database->str);

  if (connection->execute_direct({ query.str, query.length }))
    return true;

  Ed_result_set *result= connection->use_result_set();

  // We are expecting exactly one result set with one row and two columns,
  // in any other case, something went wrong
  if (!result || result->size() != 1 || result->get_field_count() != 2 ||
      connection->has_next_result())
  {
    return true;
  }

  
  List<Ed_row>& rows= *result;
  // First column is name of database, second is create query
  Ed_column const *column= rows.head()->get_column(1);

  sql_print_information("Got create database query for %s\n%s",
                        database->str, column->str);

  // FIXME - Farnham
  // Double check exact meaning of all constructor parameters
  Query_log_event evt(thd, column->str, column->length,
                      false, // using_trans
                      true,  // direct
                      true,  // suppress_use
                      0);    // error

  send_event(evt);
  net_flush(&thd->net);

  return false;
}

/**
  Creates and sends 'CREATE TABLE' <code>Query_log_event</code> for
  first table in <code>tables</code> list

  @return false - ok
          true  - error
 */

bool provisioning_send_info::send_create_table()
{
  DBUG_ASSERT(!tables.is_empty());

  DYNAMIC_STRING query;
  LEX_STRING const *database= databases.head();
  char const *table= tables.head();

  // FIXME - Farnham
  // Quoting / escaping
  init_dynamic_string(&query, "SHOW CREATE TABLE ", 0, 0);
  dynstr_append(&query, database->str);
  dynstr_append(&query, ".");
  dynstr_append(&query, table);

  if (connection->execute_direct({ query.str, query.length }))
    return true;

  Ed_result_set *result= connection->use_result_set();

  // We are expecting exactly one result set with one row and two columns,
  // in any other case, something went wrong
  if (!result || result->size() != 1 || result->get_field_count() != 2 ||
      connection->has_next_result())
  {
    return true;
  }

  List<Ed_row>& rows= *result;
  // First column is name of table, second is create query
  Ed_column const *column= rows.head()->get_column(1);

  sql_print_information("Got create table query for %s.%s\n%s",
                        database->str, table, column->str);


  // Set database, Query_log_event uses it for generation of use statement
  char *old_db= thd->db;
  size_t old_db_length= thd->db_length;
  thd->reset_db(database->str, database->length);

  // FIXME - Farnham
  // Double check exact meaning of all constructor parameters
  Query_log_event evt(thd, column->str, column->length,
                      false, // using_trans
                      true,  // direct
                      false, // suppress_use
                      0);    // error

  send_event(evt);

  thd->reset_db(old_db, old_db_length);
  net_flush(&thd->net);

  return false;
}

/**
  Sends data from currently provisioned table - first in
  <code>tables</code> list

  @return -1 - ok, more data waiting
           0 - ok, no more data
           1 - error
 */

int8 provisioning_send_info::send_table_data()
{
  // Ensure that tables were prepared
  DBUG_ASSERT(!tables.is_empty());

  sql_print_information("send_table_data() - %s.%s", databases.head()->str,
                        tables.head());

  TABLE_LIST table_list;
  table_list.init_one_table(databases.head()->str, databases.head()->length,
                            tables.head(), strlen(tables.head()),
                            NULL, TL_READ);

  // FIXME - Farnham
  // Check for permissions

  if (open_and_lock_tables(thd, &table_list, FALSE, 0))
    return 0;

  TABLE *table= table_list.table;
  handler *hdl= table->file;
  DBUG_ASSERT(hdl);

  // Initialize scan from last remembered position - can be NULL but it is
  // valid function argument
  if ((error= hdl->prepare_range_scan(row_batch_end, NULL)) != 0)
  {
    error_text= "Range scan preparation failed";
    return 1;
  }

  // FIXME - Farnham
  // Check for index existence
  if ((error= hdl->ha_index_init(0, true)) != 0)
  {
    error_text= "Index initialization failed";
    return 1;
  }

  if ((error= hdl->read_range_first(row_batch_end, NULL, false, true)) == 0)
  {
    uint32 packed_rows= 0;

    Table_map_log_event map_evt= Table_map_log_event(thd, table,
                                                     table->s->table_map_id,
                                                     false);

    Write_rows_log_event evt= Write_rows_log_event(thd, table,
                                                   table->s->table_map_id,
                                                   &table->s->all_set, false);
    evt.set_flags(Rows_log_event::STMT_END_F);

    do
    {
      if (prepare_row_buffer(table, table->record[0]))
      {
        error_text= "Out of memory";
        return 1;
      }

      size_t len= pack_row(table, &table->s->all_set, (uchar*)row_buffer,
                           table->record[0]);

      if ((error= evt.add_row_data((uchar*)row_buffer, len)) != 0)
      {
        error_text= "Failed to add row data to event";
        return 1;
      }

      ++packed_rows;
    }
    while (packed_rows < PROV_ROW_BATCH_SIZE &&
           (error= hdl->read_range_next()) == 0);

    if (error && error != HA_ERR_END_OF_FILE)
    {
      return 1;
    }

    if (packed_rows > 0)
    {
      send_event(map_evt);
      send_event(evt);
    }

    // Read one more row and store its key for next call
    // On this place, variable error can be only 0 or HA_ERR_END_OF_FILE
    if (error != HA_ERR_END_OF_FILE && (error= hdl->read_range_next()) == 0)
    {
      // First time saving key for this table, allocate structure for it
      if (!row_batch_end)
      {
        row_batch_end= new key_range();
        row_batch_end->length= table->key_info->key_length;
        row_batch_end->keypart_map= HA_WHOLE_KEY;
        row_batch_end->flag= HA_READ_KEY_OR_NEXT;
        row_batch_end->key= new uchar[row_batch_end->length];
      }

      key_copy(const_cast<uchar*>(row_batch_end->key), table->record[0],
               table->key_info, table->key_info->key_length);
    }
  }

  // Ok, more data - default result
  int8 result= -1;

  // EOF error means, that we processed all rows in table, shift to next
  // table and ignore it
  if (error == HA_ERR_END_OF_FILE)
  {
    error= 0;
    free(tables.pop());

    // Free key data for current table
    if (row_batch_end)
    {
      delete[] row_batch_end->key;
      delete row_batch_end;
      row_batch_end= NULL;
    }

    // Ok, no more data
    result= 0;
  }
  // Real error occurred
  else if (error)
  {
    error_text= "Error occurred while sending row data";
    return 1;
  }

  if ((error= hdl->ha_index_end()) != 0)
  {
    error_text= "ha_index_end() call failed";
    return 1;
  }

  close_thread_tables(thd);

  // FIXME - Farnham
  // Where were these locks acquired? Can we release them here?
  // If they are not released, cleanup drop table statement from test
  // causes deadlock during meta data locking
  thd->mdl_context.release_transactional_locks();

  return result;
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
  switch (phase)
  {
    case PROV_PHASE_INIT:
    {
      if (build_database_list())
        return 1;

      phase= PROV_PHASE_DB_INIT;
      break;
    }
    case PROV_PHASE_DB_INIT:
    {
      // No more data to send
      if (databases.is_empty())
        return 0;

      if (send_create_database() || build_table_list())
        return 1;

      phase= PROV_PHASE_TABLE_INIT;
      break;
    }
    case PROV_PHASE_TABLE_INIT:
    {
      // No more tables to process for this database, try next
      if (tables.is_empty())
      {
        // FIXME - Farnham
        // Deletion of LEX_STRING allocated by thd
        databases.pop();

        phase= PROV_PHASE_DB_INIT;
        break;
      }

      if (send_create_table())
        return 1;

      phase= PROV_PHASE_TABLE_DATA;
      break;
    }
    case PROV_PHASE_TABLE_DATA:
    {
      int8 res= send_table_data();
      // Error
      if (res == 1)
        return 1;
      // Ok, no more data - next phase
      else if (res == 0)
        phase= PROV_PHASE_TRIGGERS;
      // else if ( res == -1)
      //   Ok, more data - no phase change

      break;
    }
    case PROV_PHASE_TRIGGERS:
    {
      // NYI
      phase= PROV_PHASE_EVENTS;
      break;
    }
    case PROV_PHASE_EVENTS:
    {
      // NYI
      phase= PROV_PHASE_ROUTINES;
      break;
    }
    case PROV_PHASE_ROUTINES:
    {
      // NYI - last phase, try to work on next table
      phase= PROV_PHASE_TABLE_INIT;
      break;
    }
  }

  // There may be more tables or databases waiting, run this function at least
  // one more time
  return -1;
}

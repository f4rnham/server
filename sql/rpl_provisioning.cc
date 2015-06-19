// FIXME - Farnham header

#include "rpl_provisioning.h"
#include "log_event.h"
#include "sql_prepare.h"
#include "sql_base.h"

provisioning_send_info::provisioning_send_info(THD *thd_arg)
  : thd(thd_arg)
  , phase(PROV_PHASE_INIT)
{
  connection= new Ed_connection(thd);

  ed_connection_test();
}

provisioning_send_info::~provisioning_send_info()
{
  delete connection;

  // FIXME - Farnham
  // <code>databases</code> were allocated by thd mem root - how does it work?

  List_iterator_fast<char> it(tables);
  void* name;
  while (name= it.next_fast())
    // Names (char*) were allocated by strdup
    free(name);
}

bool provisioning_send_info::build_database_list()
{
  if (connection->execute_direct({ C_STRING_WITH_LEN("SHOW DATABASES") }))
  {
    return true;
  }

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

  int const max_query_length= sizeof("SHOW TABLES FROM ") + NAME_CHAR_LEN + 1;
  char query[max_query_length];
  LEX_STRING const *database= databases.head();
  int length= my_snprintf(query, max_query_length,
                          "SHOW TABLES FROM %s", database->str);

  if (length < 0 || length >= max_query_length)
  {
    return true;
  }

  if (connection->execute_direct({ query, length }))
  {
    return true;
  }

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

int8 provisioning_send_info::send_provisioning_data_hardcoded_data_test()
{
  static int sent= 45;

  if (sent >= 50)
    return 0;

  ++sent;

  sql_print_information("send %d", sent);

  char query[128];
  sprintf(query, "INSERT INTO test.t1 VALUES (%d)", sent);

  Query_log_event evt(thd, query, strlen(query), false, true, true, 0);

  send_event(evt);
  net_flush(&thd->net);

  return -1;
}

void provisioning_send_info::ed_connection_test()
{
  connection->execute_direct({ C_STRING_WITH_LEN("SHOW create DATABASE test") });

  sql_print_information("Field cnt %u", connection->get_field_count());

  Ed_result_set *result= connection->use_result_set();
  List<Ed_row>& rows= *result;
  for (uint32 i= 0; i < result->size(); ++i)
  {
    sql_print_information("Result %u:", i);

    List_iterator<Ed_row> it(rows);
    for (uint32 j= 0; j < rows.elements; ++j)
    {
      sql_print_information("Row %u:", j);

      Ed_row *row= it++;
      for (uint32 k= 0; k < row->size(); ++k)
      {
        sql_print_information("Column %u: %s", k, row->get_column(k)->str);
      }
    }
  }
}

/**
  Sends data from currently provisioned table - first in
  <code>tables</code> list
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

  // FIXME - Farnham
  // Send table meta data

  hdl->prepare_range_scan(NULL, NULL);
  hdl->ha_index_init(0, true);
  DBUG_ASSERT(hdl->read_range_first(0, 0, false, true) == 0);

  // Fix buffer size
  uint8 buff2[4 * 512];

  Table_map_log_event map_evt= Table_map_log_event(thd, table,
                                                   table->s->table_map_id,
                                                   false);

  Write_rows_log_event evt= Write_rows_log_event(thd, table,
                                                 table->s->table_map_id,
                                                 &table->s->all_set, false);
  evt.set_flags(Rows_log_event::STMT_END_F);

  do
  {
    size_t len= pack_row(table, &table->s->all_set, buff2, table->record[0]);
    DBUG_ASSERT(len < 4 * 512);
    evt.add_row_data(buff2, len);
  }
  while (hdl->read_range_next() == 0);

  hdl->ha_index_end();

  send_event(map_evt);
  send_event(evt);

  net_flush(&thd->net);

  close_thread_tables(thd);
  // FIXME - Farnham
  // Where were these locks acquired? Can we release them here?
  // If they are not released, cleanup drop table statement from test
  // causes deadlock during meta data locking
  thd->mdl_context.release_transactional_locks();

  // FIXME - Farnham
  // Don't send whole table at once

  // Continue with next table
  free(tables.pop());

  return 0;
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
  DBUG_EXECUTE_IF("provisioning_hardcoded_data_test",
                  return send_provisioning_data_hardcoded_data_test(););

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

      if (build_table_list())
        return 1;

      phase= PROV_PHASE_TABLES;
      break;
    }
    case PROV_PHASE_TABLES:
    {
      // No more tables to process for this DB, move to next step
      if (tables.is_empty())
      {
        phase= PROV_PHASE_TRIGGERS;
        break;
      }

      if (send_table_data())
        return 1;

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
      // NYI - last phase, try to work on next DB
      phase= PROV_PHASE_DB_INIT;
      // FIXME - Farnham
      // Deletion of LEX_STRING allocated by thd
      databases.pop();
      break;
    }
  }

  // There may be more tables or databases waiting, run this function at least
  // one more time
  return -1;
}

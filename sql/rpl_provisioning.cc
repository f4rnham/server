// FIXME - Farnham header

#include "rpl_provisioning.h"
#include "log_event.h"
#include "sql_prepare.h"
#include "sql_base.h"

provisioning_send_info::provisioning_send_info(THD *thd_arg)
  : thd(thd_arg)
  , tables(NULL)
{
  connection= new Ed_connection(thd);

  ed_connection_test();

  DBUG_ASSERT(!build_database_list());

  if (!databases.is_empty())
    DBUG_ASSERT(!build_table_list());
}

provisioning_send_info::~provisioning_send_info()
{
  delete connection;

  List_iterator_fast<char> it(databases);
  void* name;
  while (name= it.next_fast())
    // Names (char*) were allocated by strdup
    free(name);

  clear_tables_list();
}

/**
  Clears list of discovered tables for currently provisioned database
 */

void provisioning_send_info::clear_tables_list()
{
  if (!tables)
    return;

  List_iterator_fast<char> it(*tables);
  void* name;
  while (name= it.next_fast())
    // Names (char*) were allocated by strdup
    free(name);

  delete tables;
  tables= NULL;
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

    sql_print_information("Discovered database %s", row->get_column(0)->str);
    databases.push_back(strdup(row->get_column(0)->str));
  }

  return false;
}

/**
  Builds list of tables for currently provisioned database - first in
  'databases' list
 */

bool provisioning_send_info::build_table_list()
{
  // No database is currently provisioned
  DBUG_ASSERT(tables == NULL);
  // There is at least one database left for provisioning
  DBUG_ASSERT(databases.elements > 0);

  int const max_query_length= sizeof("SHOW TABLES FROM ") + NAME_CHAR_LEN + 1;
  char query[max_query_length];
  int length= my_snprintf(query, max_query_length,
                          "SHOW TABLES FROM %s", databases.head());

  if (length < 0 || length >= max_query_length)
  {
    return true;
  }

  if (connection->execute_direct({ query, length }))
  {
    return true;
  }

  tables= new List<char>();
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

    sql_print_information("Discovered table %s", row->get_column(0)->str);
    tables->push_back(strdup(row->get_column(0)->str));
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

int8 provisioning_send_info::send_table_data()
{
  TABLE_LIST tables;
  tables.init_one_table(STRING_WITH_LEN("test"),
                        STRING_WITH_LEN("t1"),
                        NULL, TL_READ);

  // FIXME - Farnham
  // Check for permissions and system tables

  if (open_and_lock_tables(thd, &tables, FALSE, 0))
    return 0;

  TABLE *table= tables.table;
  handler *hdl= table->file;
  DBUG_ASSERT(hdl);

  hdl->prepare_range_scan(NULL, NULL);
  hdl->ha_index_init(0, true);
  DBUG_ASSERT(hdl->read_range_first(0, 0, false, true) == 0);

  // Fix buffer size
  uint8 buff2[4 * 512];

  Table_map_log_event map_evt= Table_map_log_event(thd, table,
                                                   table->s->table_map_id,
                                                   false);

  do
  {
    Write_rows_log_event evt= Write_rows_log_event(thd, table,
                                                   table->s->table_map_id,
                                                   &table->s->all_set, false);

    size_t len= pack_row(table, &table->s->all_set, buff2, table->record[0]);
    DBUG_ASSERT(len < 4 * 512);
    evt.add_row_data(buff2, len);
    evt.set_flags(Rows_log_event::STMT_END_F);

    send_event(map_evt);
    send_event(evt);

    sql_print_information("Dumped one row");
  } while (hdl->read_range_next() == 0);

  hdl->ha_index_end();

  net_flush(&thd->net);

  close_thread_tables(thd);
  // FIXME - Farnham
  // Where were these locks acquired? Can we release them here?
  // If they are not released, cleanup drop table statement from test
  // causes deadlock during meta data locking
  thd->mdl_context.release_transactional_locks();
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

  return send_table_data();
}

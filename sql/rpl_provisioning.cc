// FIXME - Farnham
// Header
// Character sets, collations
//  documentation, formatting, tests
//  triggers - done
//  events
//  routines

#include "rpl_provisioning.h"
#include "log_event.h"
#include "sql_prepare.h"
#include "sql_base.h"
#include "key.h"
#include "sql_db.h"
#include "set_var.h"

/*
  Quotes a string using backticks

  @param[in] name   String to quote
  @param[out] buff  Destination buffer
*/

void quote_name(char const *name, char *buff)
{
  char *to= buff;
  char qtype= '`';

  *to++= qtype;
  while (*name)
  {
    if (*name == qtype)
      *to++= qtype;
    *to++= *name++;
  }
  to[0]= qtype;
  to[1]= 0;
}

provisioning_send_info::provisioning_send_info(THD *thd_arg)
  : thd(thd_arg)
  , connection(new Ed_connection(thd))
  , row_batch_end(NULL)
  , row_buffer(my_malloc(ROW_BUFFER_DEFAULT_SIZE, MYF(0)))
  , row_buffer_size(ROW_BUFFER_DEFAULT_SIZE)
  , phase(PROV_PHASE_INIT)
  , error(0)
  , error_text(NULL)
  , event_conversion_cache(NULL)
{
}

provisioning_send_info::~provisioning_send_info()
{
  delete connection;

  List_iterator_fast<char> it_char(tables);
  void* name;
  while ((name= it_char.next_fast()) != NULL)
    // Names (char*) were allocated by my_strdup
    my_free(name);

  List_iterator_fast<DYNAMIC_STRING> it_dynstr(views);
  while ((name= it_dynstr.next_fast()) != NULL)
  {
    dynstr_free((DYNAMIC_STRING*)name);
    // Names (DYNAMIC_STRING*) were allocated by my_malloc
    my_free(name);
  }

  // If error occurred, this may not be freed
  if (row_batch_end)
    free_key_range();

  my_free(row_buffer);

  if (event_conversion_cache)
  {
    close_cached_file(event_conversion_cache);
    my_free(event_conversion_cache);
  }
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

    if ((row_buffer= my_realloc(row_buffer, row_buffer_size, MYF(0))) == NULL)
    {
      error_text= "Out of memory";
      return true;
    }
  }

  return false;
}

/**
  Opens one table and performs all necessary checks

  @param [out] table_list List which will contain opened table if function
                          succeeds
  @param [in]  database   Name of database containing table to be opened
  @param [in]  table      Name of table to be opened

  @return false - ok
          true  - error
 */

bool provisioning_send_info::open_table(TABLE_LIST* table_list,
                                        LEX_STRING const* database,
                                        char const *table)
{
  table_list->init_one_table(database->str, database->length,
                             table, strlen(table),
                             NULL, TL_READ);

  // FIXME - Farnham
  // Check for permissions

  if (open_and_lock_tables(thd, table_list, FALSE, 0))
  {
    error_text= "Failed to open table";
    return true;
  }

  return false;
}

/**
  Closes all tables opened by provisioning and releases locks
 */

void provisioning_send_info::close_tables()
{
  close_thread_tables(thd);

  // FIXME - Farnham
  // Where were these locks acquired? Can we release them here?
  // If they are not released, cleanup drop table statement from test
  // causes deadlock during meta data locking
  thd->mdl_context.release_transactional_locks();
}

bool provisioning_send_info::send_query_log_event(
  DYNAMIC_STRING const *query,
  bool suppress_use,
  LEX_STRING const *database,
  provisioning_cs_info *cs_info)
{
  return send_query_log_event(query->str, query->length, suppress_use, database, cs_info);
}

bool provisioning_send_info::send_query_log_event(
  LEX_STRING const *query,
  bool suppress_use,
  LEX_STRING const *database,
  provisioning_cs_info *cs_info)
{
  return send_query_log_event(query->str, query->length, suppress_use, database, cs_info);
}

/**
  Create query log event from provided query and send it to slave

  @param query        Query
  @param suppress_use Suppress 'USE' statement, if false, function temporarily
                      switches thread database
  @param database     Database in which query has to be executed, not null
                      only if suppress_use == false
  @param cs_info      If not NULL, information about required character set
                      settings

  @return false - ok
          true  - error
 */

bool provisioning_send_info::send_query_log_event(char const *query,
                                                  size_t const query_length,
                                                  bool suppress_use,
                                                  LEX_STRING const *database,
                                                  provisioning_cs_info *cs_info)
{
  char *old_db;
  size_t old_db_length;

  if (!suppress_use)
  {
    // Set database, Query_log_event uses it for generation of use statement
    old_db= thd->db;
    old_db_length= thd->db_length;
    thd->reset_db(database->str, database->length);
  }

  Query_log_event evt(thd, query, query_length,
                      false,        // using_trans
                      true,         // direct
                      suppress_use, // suppress_use
                      0);           // error

  if (cs_info)
  {
    int2store(evt.charset, cs_info->cs_client); // thd_arg->variables.character_set_client->number
    int2store(evt.charset + 2, cs_info->cl_connection); // thd_arg->variables.collation_connection->number
    int2store(evt.charset + 4, /*cs_info->cl_server*/ thd->variables.collation_server->number);
    evt.charset_database_number = cs_info->cl_db; // thd_arg->variables.collation_database->number

    evt.sql_mode_inited = 1;
    evt.sql_mode = cs_info->sql_mode;
  }

  bool res= false;
  if (send_event(evt))
    res= true;

  if (!suppress_use)
    thd->reset_db(old_db, old_db_length);

  return res;
}

/**
  Function responsible for allocation of <code>row_batch_end</code>

  @param table Table for which is key range allocated

  @return false - ok
          true  - error
 */

bool provisioning_send_info::allocate_key_range(TABLE *table)
{
  // Prevent allocation without free
  DBUG_ASSERT(row_batch_end == NULL);

  row_batch_end= (key_range*)my_malloc(sizeof(key_range), MYF(0));
  if (!row_batch_end)
    return true;

  row_batch_end->length= table->key_info->key_length;
  row_batch_end->keypart_map= HA_WHOLE_KEY;
  row_batch_end->flag= HA_READ_KEY_OR_NEXT;
  row_batch_end->key= (uchar*)my_malloc(row_batch_end->length * sizeof(uchar),
                                        MYF(0));

  return row_batch_end->key ? false : true;
}

/**
  Function responsible for freeing of memory allocated by
  <code>provisioning_send_info::allocate_key_range</code>
 */

void provisioning_send_info::free_key_range()
{
  my_free((void*)row_batch_end->key);
  my_free(row_batch_end);
  row_batch_end= NULL;
}

/**
  Builds list of databases and stores it in <code>databases</code> list
  Run only once per provisioning

  @return false - ok
          true  - error
 */

bool provisioning_send_info::build_database_list()
{
  if (connection->execute_direct({ C_STRING_WITH_LEN("SHOW DATABASES") }))
  {
    record_ed_connection_error("Failed to query existing databases");
    return true;
  }

  Ed_result_set *result= connection->use_result_set();

  // We are expecting exactly one result set with one column,
  // in any other case, something went wrong
  if (!result || result->get_field_count() != 1 ||
      connection->has_next_result())
  {
    error_text= "Failed to read list of existing databases";
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

    // This variable is here only because we can not use continue for outer
    // loop in DBUG_EXECUTE_IF
    bool skip= false;

    // Skip mysql, information_schema and performance_schema databases
    if (!my_strcasecmp(system_charset_info, column->str, INFORMATION_SCHEMA_NAME.str) ||
        !my_strcasecmp(system_charset_info, column->str, PERFORMANCE_SCHEMA_DB_NAME.str) ||
        !my_strcasecmp(system_charset_info, column->str, MYSQL_SCHEMA_NAME.str))
    {
      skip= true;
    }

    // Skip test run database only if we are running test - there is small
    // chance, that regular user database is called 'mtr'
    DBUG_EXECUTE_IF("provisioning_test_running",
    {
      if (!my_strcasecmp(system_charset_info, column->str, "mtr"))
        skip= true;
    });

    if (skip)
      continue;

    sql_print_information("Discovered database `%s`", column->str);
    databases.push_back(thd->make_lex_string(column->str, column->length));
  }

  return false;
}

/**
  Builds list of tables for currently provisioned database - first in
  <code>databases</code> list

  @return false - ok
          true  - error
 */

bool provisioning_send_info::build_table_list()
{
  // If tables for some database were fetched previously, they were all
  // processed (removed from list)
  DBUG_ASSERT(tables.is_empty());
  // There is at least one database left for provisioning
  DBUG_ASSERT(!databases.is_empty());

  DYNAMIC_STRING query;
  LEX_STRING const *database= databases.head();
  char name_buff[NAME_LEN * 2 + 3];

  init_dynamic_string(&query, "SHOW FULL TABLES FROM ", 0, 0);

  quote_name(database->str, name_buff);
  dynstr_append(&query, name_buff);

  if (connection->execute_direct({ query.str, query.length }))
  {
    record_ed_connection_error("Failed to query tables from database");
    dynstr_free(&query);
    return true;
  }

  dynstr_free(&query);

  Ed_result_set *result= connection->use_result_set();

  // We are expecting exactly one result set with two columns,
  // in any other case, something went wrong
  if (!result || result->get_field_count() != 2 ||
      connection->has_next_result())
  {
    error_text= "Failed to read list of tables";
    return true;
  }

  List<Ed_row>& rows= *result;
  List_iterator_fast<Ed_row> it(rows);

  for (uint32 i= 0; i < rows.elements; ++i)
  {
    Ed_row *row= it++;

    // Field count for entire result set was already checked, recheck
    // in case of internal inconsistency
    DBUG_ASSERT(row->size() == 2);

    Ed_column const *name= row->get_column(0);
    Ed_column const *type= row->get_column(1);

    sql_print_information("Discovered %s `%s`.`%s`",
                          my_strcasecmp(system_charset_info, "VIEW", type->str) ?
                          "table" : "view",
                          database->str,
                          name->str);

    if (my_strcasecmp(system_charset_info, "VIEW", type->str) == 0)
    {
      char name_buff[NAME_LEN * 2 + 3];
      DYNAMIC_STRING *str= (DYNAMIC_STRING*)my_malloc(sizeof(DYNAMIC_STRING),
                                                      MYF(0));

      quote_name(database->str, name_buff);
      init_dynamic_string(str, name_buff,
                          // Both lengths + dot and quotes, will be reallocated
                          // only in special cases
                          database->length + name->length + 5,
                          5);
      dynstr_append(str, ".");
      quote_name(name->str, name_buff);
      dynstr_append(str, name_buff);

      views.push_back(str);
    }
    else if (my_strcasecmp(system_charset_info, "BASE TABLE", type->str) == 0)
      tables.push_back(my_strdup(name->str, MYF(0)));
    else
      DBUG_ASSERT(false &&
                  "Unexpected type of table returned by 'SHOW FULL TABLES'");
  }

  return false;
}

/**
  Helper for conversion of Log_event into data which can be sent to slave

  @return false - ok
          true  - error
 */

bool provisioning_send_info::event_to_packet(Log_event &evt, String &packet)
{
  // Reset cache for writing
  reinit_io_cache(event_conversion_cache, WRITE_CACHE, 0, false, true);

  if (evt.write(event_conversion_cache))
  {
    error_text= "Failed to write event to conversion cache";
    return true;
  }

  if (reinit_io_cache(event_conversion_cache, READ_CACHE, 0, false, false))
  {
    error_text= "Failed to switch event conversion cache mode";
    return true;
  }

  packet.set("\0", 1, &my_charset_bin);
  if (packet.append(event_conversion_cache,
      event_conversion_cache->write_pos - event_conversion_cache->buffer))
  {
    error_text= "Failed to write event data to packet";
    return true;
  }

  // Set current server as source of event, when slave registers on master,
  // it overwrites thd->variables.server_id with its own server id - and it is
  // then used when writing event
  int4store(&packet[SERVER_ID_OFFSET + 1], global_system_variables.server_id);
  return false;
}

/**
  Converts <code>Log_event</code> to replication packet and sends it to slave

  @return false - ok
          true  - error
 */

bool provisioning_send_info::send_event(Log_event &evt)
{
  String packet;

  if (event_to_packet(evt, packet))
    return true;

  if (my_net_write(&thd->net, (uchar*)packet.ptr(), packet.length()))
  {
    error_text= "Failed to send event packet to slave";
    return true;
  }

  return false;
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
  char name_buff[NAME_LEN * 2 + 3];

  init_dynamic_string(&query, "SHOW CREATE DATABASE ", 0, 0);

  quote_name(database->str, name_buff);
  dynstr_append(&query, name_buff);

  if (connection->execute_direct({ query.str, query.length }))
  {
    record_ed_connection_error("Failed to retrieve database structure");
    return true;
  }

  Ed_result_set *result= connection->use_result_set();

  // We are expecting exactly one result set with one row and two columns,
  // in any other case, something went wrong
  if (!result || result->size() != 1 || result->get_field_count() != 2 ||
      connection->has_next_result())
  {
    error_text= "Failed to read database structure";
    return true;
  }


  List<Ed_row>& rows= *result;
  // First column is name of database, second is create query
  Ed_column const *column= rows.head()->get_column(1);

  sql_print_information("Got create database query for `%s`\n%s",
                        database->str, column->str);

  return send_query_log_event(column);
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
  char name_buff[NAME_LEN * 2 + 3];

  init_dynamic_string(&query, "SHOW CREATE TABLE ", 0, 0);

  quote_name(database->str, name_buff);
  dynstr_append(&query, name_buff);

  dynstr_append(&query, ".");

  quote_name(table, name_buff);
  dynstr_append(&query, name_buff);

  if (connection->execute_direct({ query.str, query.length }))
  {
    record_ed_connection_error("Failed to retrieve table structure");
    return true;
  }

  Ed_result_set *result= connection->use_result_set();

  // We are expecting exactly one result set with one row and two columns,
  // in any other case, something went wrong
  if (!result || result->size() != 1 || result->get_field_count() != 2 ||
      connection->has_next_result())
  {
    error_text= "Failed to read table structure";
    return true;
  }

  List<Ed_row>& rows= *result;
  // First column is name of table, second is create query
  Ed_column const *column= rows.head()->get_column(1);

  sql_print_information("Got create table query for `%s`.`%s`\n%s",
                        database->str, table, column->str);


  return send_query_log_event(column, false, database);
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

  sql_print_information("send_table_data() - `%s`.`%s`", databases.head()->str,
                        tables.head());

  TABLE_LIST table_list;
  if (open_table(&table_list, databases.head(), tables.head()))
    return 1;

  TABLE *table= table_list.table;
  handler *hdl= table->file;
  DBUG_ASSERT(hdl);

  // Initialize scan from last remembered position - can be NULL but it is
  // valid function argument
  if ((error= hdl->prepare_range_scan(row_batch_end, NULL)) != 0)
  {
    error_text= "Range scan preparation failed";
    close_tables();
    return 1;
  }

  if (table->s->primary_key == MAX_KEY)
  {
    error_text= "Table does not contain primary key";
    close_tables();
    return 1;
  }

  if ((error= hdl->ha_index_init(table->s->primary_key, true)) != 0)
  {
    error_text= "Index initialization failed";
    close_tables();
    return 1;
  }

  // Default result, error
  int8 result= 1;

  if ((error= hdl->read_range_first(row_batch_end, NULL, false, true)) == 0)
  {
    uint32 packed_rows= 0;

    Table_map_log_event map_evt(thd, table, table->s->table_map_id, false);

    Write_rows_log_event evt(thd, table, table->s->table_map_id,
                             &table->s->all_set, false);

    evt.set_flags(Rows_log_event::STMT_END_F);

    do
    {
      if (prepare_row_buffer(table, table->record[0]))
        goto error;

      size_t len= pack_row(table, &table->s->all_set, (uchar*)row_buffer,
                           table->record[0]);

      if ((error= evt.add_row_data((uchar*)row_buffer, len)) != 0)
      {
        error_text= "Failed to add row data to event";
        goto error;
      }

      ++packed_rows;
    }
    while (packed_rows < PROV_ROW_BATCH_SIZE &&
           (error= hdl->read_range_next()) == 0);

    if (error && error != HA_ERR_END_OF_FILE)
      goto error;

    if (packed_rows > 0)
    {
      if (send_event(map_evt) || send_event(evt))
        goto error;
    }

    // Read one more row and store its key for next call
    // On this place, variable error can be only 0 or HA_ERR_END_OF_FILE
    if (error != HA_ERR_END_OF_FILE && (error= hdl->read_range_next()) == 0)
    {
      // First time saving key for this table, allocate structure for it
      if (!row_batch_end && allocate_key_range(table))
      {
        error_text= "Out of memory";
        goto error;
      }

      key_copy(const_cast<uchar*>(row_batch_end->key), table->record[0],
               table->key_info, table->key_info->key_length);
    }
  }

  // EOF error means, that we processed all rows in table, shift to next
  // table and ignore it
  if (error == HA_ERR_END_OF_FILE)
  {
    error= 0;

    // Free key data for current table
    if (row_batch_end)
      free_key_range();

    // Ok, no more data
    result= 0;
  }
  // Real error occurred, result= 1 is default value, no need to set it here
  else if (error)
    error_text= "Error occurred while sending row data";
  // More data
  else
    result= -1;

error:

  if ((error= hdl->ha_index_end()) != 0)
  {
    error_text= "ha_index_end() call failed";
    result= 1;
  }

  close_tables();
  return result;
}

/**
  Creates and sends 'CREATE TRIGGER' <code>Query_log_event</code> for
  first table in <code>databases</code> list

  @return false - ok
          true  - error
 */

bool provisioning_send_info::send_table_triggers()
{
  // There is at least one database left for provisioning and
  // table list is prepared
  DBUG_ASSERT(!tables.is_empty() && !databases.is_empty());

  LEX_STRING const *database= databases.head();
  TABLE_LIST table_list;
  if (open_table(&table_list, database, tables.head()))
    return 1;

  bool result= false;
  TABLE *table= table_list.table;
  Table_triggers_list *triggers= table->triggers;

  if (!triggers)
  {
    sql_print_information("No triggers discovered in `%s`.`%s`",
                          database->str, tables.head());

    close_tables();
    return 0;
  }

  List_iterator_fast<LEX_STRING> it_sql_orig_stmt(triggers->definitions_list);
  List_iterator_fast<LEX_STRING> it_db_cl_name(triggers->db_cl_names);
  List_iterator_fast<LEX_STRING> it_client_cs_name(triggers->client_cs_names);
  List_iterator_fast<LEX_STRING> it_connection_cl_name(triggers->connection_cl_names);
  List_iterator_fast<ulonglong> it_sql_mode(triggers->definition_modes_list);
  LEX_STRING *definition, *db_cl_name, *client_cs_name, *connection_cl_name;
  ulonglong *sql_mode;

  provisioning_cs_info cs_info;

  while ((definition= (LEX_STRING*)it_sql_orig_stmt.next_fast()) != NULL)
  {
    db_cl_name= (LEX_STRING*)it_db_cl_name.next_fast();
    client_cs_name= (LEX_STRING*)it_client_cs_name.next_fast();
    connection_cl_name = (LEX_STRING*)it_connection_cl_name.next_fast();
    sql_mode= (ulonglong*)it_sql_mode.next_fast();

    DBUG_ASSERT(db_cl_name && client_cs_name && connection_cl_name && sql_mode);

    sql_print_information("Found trigger\n%s", definition->str);

    cs_info.cl_connection= get_collation_number(connection_cl_name->str);
    cs_info.cl_db= get_collation_number(db_cl_name->str);
    
    cs_info.cs_client = get_charset_number(client_cs_name->str, MY_CS_PRIMARY);

    cs_info.sql_mode= (ulong)*sql_mode;

    if (send_query_log_event(definition, false, database, &cs_info))
    {
      result= true;
      break;
    }
  }

  close_tables();
  return result;
}

/**
  Creates and sends 'CREATE VIEW' <code>Query_log_event</code> for
  first view in <code>views</code> list

  @return false - ok
          true  - error
 */

bool provisioning_send_info::send_create_view()
{
  DBUG_ASSERT(!views.is_empty());

  DYNAMIC_STRING query;
  DYNAMIC_STRING *name= views.head();

  init_dynamic_string(&query, "SHOW CREATE VIEW ", 0, 0);
  dynstr_append(&query, name->str);

  if (connection->execute_direct({ query.str, query.length }))
  {
    record_ed_connection_error("Failed to retrieve view structure");
    return true;
  }

  Ed_result_set *result= connection->use_result_set();

  // We are expecting exactly one result set with one row and four columns,
  // in any other case, something went wrong
  if (!result || result->size() != 1 || result->get_field_count() != 4 ||
      connection->has_next_result())
  {
    error_text= "Failed to read view structure";
    return true;
  }

  List<Ed_row>& rows= *result;
  // First column is name of view, second is create query
  Ed_column const *column= rows.head()->get_column(1);

  sql_print_information("Got create view query for %s\n%s",
                        name->str, column->str);

  dynstr_free(name);
  my_free(name);
  views.pop();

  return send_query_log_event(column);
}

/**
  Creates and sends 'CREATE { PROCEDURE | FUNCTION }'
  <code>Query_log_event</code> s for first database in
  <code>databases</code> list

  @return false - ok
          true  - error
 */

bool provisioning_send_info::send_create_routines()
{
  DBUG_ASSERT(!databases.is_empty());

  LEX_STRING const *database= databases.head();
  char const *routine_type[]= { "FUNCTION", "PROCEDURE" };
  char name_buff[NAME_LEN * 2 + 3];
  char db_name_buff[NAME_LEN * 2 + 3];
  char db_name_buff_escaped[NAME_LEN * 2 + 3];

  quote_name(database->str, db_name_buff);
  escape_quotes_for_mysql(system_charset_info,
                          db_name_buff_escaped, sizeof(db_name_buff_escaped),
                          database->str, database->length);

  CHARSET_INFO* original_db_cl = get_default_db_collation(thd, database->str);

  for (uint8 i= 0; i <= 1; ++i)
  {
    DYNAMIC_STRING query;
    List<char> routines;

    // Fetch function / procedure names
    {
      init_dynamic_string(&query, "SHOW ", 0, 0);

      dynstr_append(&query, routine_type[i]);
      dynstr_append(&query, " STATUS WHERE Db = '");
      dynstr_append(&query, db_name_buff_escaped);
      dynstr_append(&query, "'");

      if (connection->execute_direct({ query.str, query.length }))
      {
        record_ed_connection_error("Failed to query existing routines");
        dynstr_free(&query);
        return true;
      }

      dynstr_free(&query);
      Ed_result_set *result= connection->use_result_set();

      // We are expecting exactly one result set with 11 columns,
      // in any other case, something went wrong
      if (!result || result->get_field_count() != 11 ||
          connection->has_next_result())
      {
        error_text= "Failed to read list of routines";
        return true;
      }

      List<Ed_row>& rows= *result;
      List_iterator_fast<Ed_row> it(rows);

      for (uint32 j= 0; j < rows.elements; ++j)
      {
        Ed_row *row= it++;

        // Field count for entire result set was already checked, recheck
        // in case of internal inconsistency
        DBUG_ASSERT(row->size() == 11);

        Ed_column const *name= row->get_column(1);

        sql_print_information("Discovered %s: %s", routine_type[i], name->str);
        routines.push_back(my_strdup(name->str, MYF(0)));
      }
    }

    // Fetch function / procedure definitions and send them to slave
    while (!routines.is_empty())
    {
      char *name= routines.pop();

      quote_name(name, name_buff);
      my_free(name);
      name= NULL;

      init_dynamic_string(&query, "SHOW CREATE ", 0, 0);
      dynstr_append(&query, routine_type[i]);
      dynstr_append(&query, " ");
      dynstr_append(&query, db_name_buff);
      dynstr_append(&query, ".");
      dynstr_append(&query, name_buff);

      if (connection->execute_direct({ query.str, query.length }))
      {
        record_ed_connection_error("Failed to query routine definition");
        dynstr_free(&query);
        return true;
      }

      dynstr_free(&query);
      Ed_result_set *result= connection->use_result_set();

      // We are expecting exactly one result set with one row and 6 columns,
      // in any other case, something went wrong
      if (!result || result->size() != 1 || result->get_field_count() != 6 ||
          connection->has_next_result())
      {
        error_text= "Failed to read definition of routine";
        return true;
      }

      List<Ed_row>& rows= *result;
      Ed_row *row= rows.head();
      Ed_column const *definition= row->get_column(2);
      provisioning_cs_info cs_info;

      cs_info.cl_connection = get_collation_number(row->get_column(4)->str);
      cs_info.cl_db = get_collation_number(row->get_column(5)->str);

      cs_info.cs_client = get_charset_number(row->get_column(3)->str, MY_CS_PRIMARY);

      // FIXME - Farnham
      cs_info.sql_mode= 0;//row->get_column(1)->str;

      if (send_query_log_event(definition, false, database, &cs_info))
        return true;
    }
  }

  return false;
}

/**
  Sends <code>Provisioning_done_log_event</code> to slave indicating
  end of provisioning

  @return false - ok
          true  - error
 */

bool provisioning_send_info::send_done_event()
{
  Provisioning_done_log_event evt;
  return send_event(evt);
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
  DBUG_EXECUTE_IF("provisioning_wait", return -1;);

  switch (phase)
  {
    case PROV_PHASE_INIT:
    {
      event_conversion_cache= (IO_CACHE*)my_malloc(sizeof(IO_CACHE), MYF(0));

      if (!event_conversion_cache ||
          open_cached_file(event_conversion_cache, mysql_tmpdir,
                           TEMP_PREFIX, READ_RECORD_BUFFER, MYF(MY_WME)))
      {
        error_text= "Failed to prepare event conversion cache";
        my_free(event_conversion_cache);
        event_conversion_cache= NULL;
        return 1;
      }

      if (build_database_list())
        return 1;

      phase= PROV_PHASE_DB_INIT;
      break;
    }
    case PROV_PHASE_DB_INIT:
    {
      // No more data to send, send view structures
      if (databases.is_empty())
      {
        phase= PROV_PHASE_VIEWS;
        break;
      }

      if (send_create_database() || build_table_list())
        return 1;

      phase= PROV_PHASE_TABLE_INIT;
      break;
    }
    case PROV_PHASE_TABLE_INIT:
    {
      // No more tables to process for this database, proceed with events and
      // routines
      if (tables.is_empty())
      {
        phase= PROV_PHASE_EVENTS;
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
      // Ok, no more data - send triggers
      else if (res == 0)
        phase= PROV_PHASE_TRIGGERS;
      // else if ( res == -1)
      //   Ok, more data - no phase change

      break;
    }
    case PROV_PHASE_TRIGGERS:
    {
      if (send_table_triggers())
        return 1;

      my_free(tables.pop());
      phase= PROV_PHASE_TABLE_INIT;
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
      if (send_create_routines())
        return 1;

      // Database provisioned, try next one
      databases.pop();
      phase= PROV_PHASE_DB_INIT;
      break;
    }
    case PROV_PHASE_VIEWS:
    {
      if (views.is_empty())
        return send_done_event() ? 1 : 0;

      if (send_create_view())
        return 1;

      break;
    }
  }

  // There may be more tables or databases waiting, run this function at least
  // one more time
  return -1;
}

/**
  Creates string describing last provisioning error

  @param[out] buffer   Destination buffer for formatted error message
  @param buffer_size   Length of destination buffer
 */

void provisioning_send_info::format_error_message(char buffer[],
                                                size_t buffer_size)
{
  // At least one better description of error must be available
  DBUG_ASSERT(error || error_text);

  if (error && error_text)
  {
    my_snprintf(buffer, buffer_size,
                "'%s', underlying error code %d while processing "
                "`%s`.`%s`", error_text, error,
                databases.is_empty() ? "<none>" : databases.head()->str,
                tables.is_empty() ? "<none>" : tables.head());
  }
  else if (error)
  {
    my_snprintf(buffer, buffer_size,
                "underlying error code %d while "
                "processing `%s`.`%s`", error,
                databases.is_empty() ? "<none>" : databases.head()->str,
                tables.is_empty() ? "<none>" : tables.head());
  }
  else // if (error_text)
  {
    my_snprintf(buffer, buffer_size,
                "'%s' while processing `%s`.`%s`", error_text,
                databases.is_empty() ? "<none>" : databases.head()->str,
                tables.is_empty() ? "<none>" : tables.head());
  }
}

/**
  Formats and stores error which occurred during <code>Ed_connection</code>
  usage, call of one of its member functions failed

  @param msg Message describing call which failed
 */

void provisioning_send_info::record_ed_connection_error(char const *msg)
{
  my_snprintf(error_text_buffer, sizeof(error_text_buffer),
              "%s, underlying error code %u, underlying error message: %s",
              msg, connection->get_last_errno(), connection->get_last_error());

  error_text= error_text_buffer;
}

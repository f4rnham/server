# Basic provisioning test, testing transfer of database / table structures,
# views, triggers, procedures

--source include/rpl_provisioning_init_row.inc

# Set provisioning_row_batch_size to small value because we want to test if provisioning
# can handle tables with large number of rows compared to provisioning_row_batch_size
--connection master
SET @old_provisioning_row_batch_size= @@global.provisioning_row_batch_size;
SET @@global.provisioning_row_batch_size= 5;

# Create databases and tables on master + fill them with data
--connection master
eval CREATE TABLE test.test_1 (a int, PRIMARY KEY (a)) ENGINE=$engine_type;
eval CREATE TABLE test.test_2 (a int, b int, c int, PRIMARY KEY (a)) ENGINE=$engine_type;

INSERT INTO test.test_1 VALUES (1), (2), (3), (4), (5);
INSERT INTO test.test_2 VALUES (1, 1, 1), (2, 2, 2), (3, 3, 3), (4, 4, 4);

CREATE VIEW test.v_1 AS SELECT test.test_1.a FROM test.test_1;

# Create table with 100 rows
eval CREATE TABLE big_table (a int auto_increment, PRIMARY KEY (a)) ENGINE=$engine_type;
DELIMITER |;
CREATE PROCEDURE test.fill_big_table()
BEGIN
  DECLARE v1 INT DEFAULT 100;
  WHILE v1 > 0 DO
    INSERT big_table VALUES (NULL);
    SET v1 = v1 - 1;
  END WHILE;
END|
DELIMITER ;|
CALL test.fill_big_table();

DELIMITER |;
CREATE TRIGGER test.trigger_1
AFTER UPDATE
  ON test_1 FOR EACH ROW
BEGIN
  SET @test_var= 1;
END|
DELIMITER ;|

# Remove part of binary logs from master to make sure that provisioning
# does not depend on them
FLUSH LOGS;
PURGE BINARY LOGS TO 'master-bin.000002';

CREATE DATABASE test2;
eval CREATE TABLE test2.test2_1 (a int, PRIMARY KEY (a)) ENGINE=$engine_type;
eval CREATE TABLE test2.test2_2 (a int, b int, PRIMARY KEY (a)) ENGINE=$engine_type;

INSERT INTO test2.test2_1 VALUES (1), (2), (3), (4), (5);
INSERT INTO test2.test2_2 VALUES (1, 1), (2, 2), (3, 3), (4, 4);

DELIMITER |;
CREATE EVENT test.ev
ON SCHEDULE EVERY 1024 YEAR STARTS '2038-01-19 03:14:07'
DO UPDATE test.test_1 SET a= 0 WHERE 0;|
DELIMITER ;|

# Start provisioning
if ($use_gtid)
{
  CHANGE MASTER TO master_use_gtid= slave_pos;
}
--connection slave
--source include/load_data_from_master.inc

--source include/rpl_provisioning_wait_to_stop.inc

# Start replication and perform changes on master
--source include/start_slave.inc

--connection master
UPDATE test2.test2_2 SET b = 5 WHERE a = 1;
DELETE FROM test2.test2_1 WHERE a = 3;
INSERT INTO test.test_1 VALUES (8);
save_master_pos;

--connection slave
sync_with_master;
--source include/stop_slave.inc

# Check if database, table / view structures and routines were transferred
SHOW CREATE DATABASE test;
SHOW CREATE DATABASE test2;
SHOW CREATE TABLE test.test_1;
SHOW CREATE TABLE test.test_2;
SHOW CREATE TABLE test.big_table;
SHOW CREATE TABLE test2.test2_1;
SHOW CREATE TABLE test2.test2_2;

SHOW CREATE VIEW test.v_1;

SHOW CREATE PROCEDURE test.fill_big_table;

SHOW CREATE EVENT test.ev;

# Check if row data were transferred
SELECT * FROM test.test_1 ORDER BY a;
SELECT * FROM test.test_2 ORDER BY a;
SELECT * FROM test.big_table ORDER BY a;
SELECT * FROM test2.test2_1 ORDER BY a;
SELECT * FROM test2.test2_2 ORDER BY a;

# Check if triggers were transferred
SHOW CREATE TRIGGER test.trigger_1;

# Cleanup
--connection master
DROP DATABASE test2;
SET @@global.provisioning_row_batch_size= @old_provisioning_row_batch_size;

--connection slave
DROP DATABASE test2;

--source include/rpl_provisioning_end.inc

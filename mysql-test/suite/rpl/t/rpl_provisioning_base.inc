# Basic provisioning test, testing transfer of database / table structures,
# views, triggers, procedures

--source include/rpl_provisioning_init.inc

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
--connection slave
LOAD DATA FROM MASTER;

# Provisioning is completed after sql thread was shut down
--source include/wait_for_slave_sql_to_stop.inc

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

--connection slave
DROP DATABASE test2;

--source include/rpl_provisioning_end.inc
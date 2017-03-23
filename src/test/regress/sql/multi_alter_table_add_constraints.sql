--
-- MULTI_ALTER__TABLE_ADD_CONSTRAINTS
--
-- Test checks whether constraints of distributed tables can be adjusted using
-- the ALTER TABLE ... ADD CONSTRAINT ... command.

ALTER SEQUENCE pg_catalog.pg_dist_shardid_seq RESTART 1400000;
ALTER SEQUENCE pg_catalog.pg_dist_jobid_seq RESTART 1400000;

-- Check "UNIQUE CONSTRAINT"
CREATE TABLE unique_test_table(id int, name varchar(20));
SELECT create_distributed_table('unique_test_table', 'id');

-- Can only add unique constraint on distribution column (or group 
-- of columns including distribution column)
ALTER TABLE unique_test_table ADD CONSTRAINT unn_name UNIQUE(name);
ALTER TABLE unique_test_table ADD CONSTRAINT unn_id UNIQUE(id);

-- Error out from the related shard
INSERT INTO unique_test_table VALUES(1, 'Ahmet');
INSERT INTO unique_test_table VALUES(1, 'Mehmet');

ALTER TABLE unique_test_table DROP CONSTRAINT unn_id;

-- Can create unique constraint over multiple columns including
-- distribution column
INSERT INTO unique_test_table VALUES(1, 'Mehmet');
ALTER TABLE unique_test_table ADD CONSTRAINT unn_id UNIQUE(id);
ALTER TABLE unique_test_table ADD CONSTRAINT unn_id_name UNIQUE(id, name);

INSERT INTO unique_test_table VALUES(1, 'Mehmet');
DROP TABLE unique_test_table;


-- Check "CHECK CONSTRAINT"
CREATE TABLE products (
    product_no integer,
    name text,
    price numeric,
    discounted_price numeric
);

SELECT create_distributed_table('products', 'product_no');

-- Can add column and table constraints
ALTER TABLE products ADD CONSTRAINT p_check CHECK(price > 0);
ALTER TABLE products ADD CONSTRAINT p_multi_check CHECK(price > discounted_price);

-- Error out from the related shard.
INSERT INTO products VALUES(1, 'product_1', -1, -2);
INSERT INTO products VALUES(1, 'product_1', 5, 3);
INSERT INTO products VALUES(1, 'product_1', 2, 3);

DROP TABLE products;


-- Check "PRIMARY KEY CONSTRAINT"
CREATE TABLE products (
    product_no integer,
    name text,
    price numeric
);

SELECT create_distributed_table('products', 'product_no');

-- Can add PRIMARY KEY to columns including distribution column
ALTER TABLE products ADD CONSTRAINT p_key PRIMARY KEY(name); 
ALTER TABLE products ADD CONSTRAINT p_key PRIMARY KEY(product_no);

-- Error out from the related shard
INSERT INTO products VALUES(1, 'product_1', 1);
INSERT INTO products VALUES(1, 'product_1', 1);

drop table products;


-- Check "EXCLUSION CONSTRAINT"



-- Check "FOREIGN CONSTRAINT"

CREATE TABLE products (
    product_no integer PRIMARY KEY,
    to_ref_id integer,
    name text,
    price numeric
);

CREATE TABLE for_orders (
    order_id integer,
    to_ref_id integer,
    product_no integer,
    quantity integer
);

SELECT create_distributed_table('products', 'product_no');
SELECT create_distributed_table('for_orders', 'product_no');

SET citus.shard_replication_factor = 1;
ALTER TABLE for_orders ADD CONSTRAINT f_key FOREIGN KEY (product_no) REFERENCES products (product_no);

INSERT INTO products VALUES(1, 1, 'product_1', 5);
INSERT INTO for_orders VALUES(1, 1, 1, 5);

-- Error out from the related shard
INSERT INTO for_orders VALUES(1, 1, 2, 5);

DROP TABLE products CASCADE;
DROP TABLE for_orders;

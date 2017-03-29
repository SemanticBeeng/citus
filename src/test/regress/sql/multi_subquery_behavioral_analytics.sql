--
-- multi subquery behavioral analytics queries aims to expand existing subquery pushdown
-- regression tests to cover more cases
-- the tables that are used depends to multi_insert_select_behavioral_analytics_create_table.sql
--

-- We don't need shard id sequence here, so commented out to prevent conflicts with concurrent tests
-- ALTER SEQUENCE pg_catalog.pg_dist_shardid_seq RESTART 1430000;
-- ALTER SEQUENCE pg_catalog.pg_dist_jobid_seq RESTART 1430000;
 
SET citus.subquery_pushdown TO TRUE;
SET citus.enable_router_execution TO FALSE;

------------------------------------
-- Vanilla funnel query
------------------------------------
SELECT user_id, array_length(events_table, 1)
FROM (
  SELECT user_id, array_agg(event ORDER BY time) AS events_table
  FROM (
    SELECT u.user_id, e.event_type::text AS event, e.time
    FROM users_table AS u,
         events_table AS e
    WHERE u.user_id = e.user_id
      AND u.user_id >= 10
      AND u.user_id <= 25
      AND e.event_type IN (100, 101, 102)
  ) t
  GROUP BY user_id
) q
ORDER BY 2 DESC, 1;

------------------------------------
--  Funnel grouped by whether or not a user has done an event
--  This has multiple subqueries joinin at the top level
------------------------------------
SELECT user_id, sum(array_length(events_table, 1)), length(hasdone_event), hasdone_event
FROM (
  SELECT
    t1.user_id,
    array_agg(event ORDER BY time) AS events_table,
    COALESCE(hasdone_event, 'Has not done event') AS hasdone_event
  FROM (
    (
      SELECT u.user_id, 'step=>1'::text AS event, e.time
      FROM users_table AS u,
          events_table AS e
      WHERE  u.user_id = e.user_id
      AND u.user_id >= 10
      AND u.user_id <= 25
      AND e.event_type IN (100, 101, 102)
    )
    UNION
    (
      SELECT u.user_id, 'step=>2'::text AS event, e.time
      FROM users_table AS u,
         events_table AS e
      WHERE  u.user_id = e.user_id
      AND u.user_id >= 10
      AND u.user_id <= 25
      AND e.event_type IN (103, 104, 105)
    )
  ) t1 LEFT JOIN (
      SELECT DISTINCT user_id, 
        'Has done event'::TEXT AS hasdone_event
      FROM  events_table AS e
      
      WHERE  e.user_id >= 10
      AND e.user_id <= 25
      AND e.event_type IN (106, 107, 108)

  ) t2 ON (t1.user_id = t2.user_id)
  GROUP BY  t1.user_id, hasdone_event
) t GROUP BY user_id, hasdone_event
ORDER BY user_id;

-- same query but multiple joins are one level below, returns count of row instead of actual rows
SELECT count(*) 
FROM (
	SELECT user_id, sum(array_length(events_table, 1)), length(hasdone_event), hasdone_event
	FROM (
	  SELECT
	    t1.user_id,
	    array_agg(event ORDER BY time) AS events_table,
	    COALESCE(hasdone_event, 'Has not done event') AS hasdone_event
	  FROM (
	    (
	      SELECT u.user_id, 'step=>1'::text AS event, e.time
	      FROM users_table AS u,
	          events_table AS e
	      WHERE  u.user_id = e.user_id
	      AND u.user_id >= 10
	      AND u.user_id <= 25
	      AND e.event_type IN (100, 101, 102)
	    )
	    UNION
	    (
	      SELECT u.user_id, 'step=>2'::text AS event, e.time
	      FROM users_table AS u,
	         events_table AS e
	      WHERE  u.user_id = e.user_id
	      AND u.user_id >= 10
	      AND u.user_id <= 25
	      AND e.event_type IN (103, 104, 105)
	    )
	  ) t1 LEFT JOIN (
	      SELECT DISTINCT user_id, 
	        'Has done event'::TEXT AS hasdone_event
	      FROM  events_table AS e
	      
	      WHERE  e.user_id >= 10
	      AND e.user_id <= 25
	      AND e.event_type IN (106, 107, 108)
	
	  ) t2 ON (t1.user_id = t2.user_id)
	  GROUP BY  t1.user_id, hasdone_event
	) t GROUP BY user_id, hasdone_event
	ORDER BY user_id) u;

------------------------------------
-- Funnel, grouped by the number of times a user has done an event
------------------------------------
SELECT
  user_id,
  avg(array_length(events_table, 1)) AS event_average,
  count_pay
  FROM (
  SELECT
  subquery_1.user_id,
  array_agg(event ORDER BY time) AS events_table,
  COALESCE(count_pay, 0) AS count_pay
  FROM
  (
    (SELECT
      users_table.user_id,
      'action=>1'AS event,
      events_table.time
    FROM
      users_table,
      events_table
    WHERE
      users_table.user_id = events_table.user_id AND
      users_table.user_id >= 10 AND
      users_table.user_id <= 70 AND
      events_table.event_type > 10 AND events_table.event_type < 12
      )
    UNION
    (SELECT
      users_table.user_id,
      'action=>2'AS event,
      events_table.time
    FROM
      users_table,
      events_table
    WHERE
      users_table.user_id = events_table.user_id AND
      users_table.user_id >= 10 AND
      users_table.user_id <= 70 AND
      events_table.event_type > 12 AND events_table.event_type < 14
    )
  ) AS subquery_1
  LEFT JOIN
    (SELECT
       user_id,
      COUNT(*) AS count_pay
    FROM
      users_table
    WHERE
      user_id >= 10 AND
      user_id <= 70 AND    
      users_table.value_1 > 15 AND users_table.value_1 < 17
    GROUP BY
      user_id
    HAVING
      COUNT(*) > 1) AS subquery_2
  ON
    subquery_1.user_id = subquery_2.user_id
  GROUP BY
    subquery_1.user_id,
    count_pay) AS subquery_top
WHERE
  array_ndims(events_table) > 0
GROUP BY
  count_pay, user_id
ORDER BY
  event_average DESC, count_pay DESC, user_id DESC;

------------------------------------
-- Most recently seen users_table events_table
------------------------------------
-- Note that we don't use ORDER BY/LIMIT yet
------------------------------------
SELECT
    user_id,
    user_lastseen,
    array_length(event_array, 1)
FROM (
    SELECT
        user_id,
        max(u.time) as user_lastseen,
        array_agg(event_type ORDER BY u.time) AS event_array
    FROM (
        
        SELECT user_id, time
        FROM users_table
        WHERE
        user_id >= 10 AND
        user_id <= 70 AND
        users_table.value_1 > 10 AND users_table.value_1 < 12

        ) u LEFT JOIN LATERAL (
          SELECT event_type, time
          FROM events_table
          WHERE user_id = u.user_id AND
          events_table.event_type > 10 AND events_table.event_type < 12
        ) t ON true
        GROUP BY user_id
) AS shard_union
ORDER BY user_lastseen DESC, user_id;

------------------------------------
-- Count the number of distinct users_table who are in segment X and Y and Z
-- This query will be supported when we have subqueries in where clauses.
------------------------------------
SELECT DISTINCT user_id
FROM users_table
WHERE user_id IN (SELECT user_id FROM users_table WHERE value_1 >= 10 AND value_1 <= 20)
    AND user_id IN (SELECT user_id FROM users_table WHERE value_1 >= 30 AND value_1 <= 40)
    AND user_id IN (SELECT user_id FROM users_table WHERE  value_1 >= 50 AND value_1 <= 60);
   
------------------------------------
-- Find customers who have done X, and satisfy other customer specific criteria
-- This query will be supported when we have subqueries in where clauses.
------------------------------------
SELECT user_id, value_2 FROM users_table WHERE
  value_1 > 101 AND value_1 < 110
  AND value_2 >= 5
  AND EXISTS (SELECT user_id FROM events_table WHERE event_type>101  AND event_type < 110 AND value_3 > 100 AND user_id=users_table.user_id);

------------------------------------
-- Customers who haven’t done X, and satisfy other customer specific criteria
-- This query will be supported when we have subqueries in where clauses.
------------------------------------
SELECT user_id, value_2 FROM users_table WHERE
  value_1 = 101
  AND value_2 >= 5
  AND NOT EXISTS (SELECT user_id FROM events_table WHERE event_type=101 AND value_3 > 100 AND user_id=users_table.user_id);

------------------------------------
-- Customers who have done X and Y, and satisfy other customer specific criteria
-- This query will be supported when we have subqueries in where clauses.
------------------------------------
SELECT user_id, value_2 FROM users_table WHERE
  value_1 > 100
  AND value_2 >= 5
  AND  EXISTS (SELECT user_id FROM events_table WHERE event_type!=100 AND value_3 > 100 AND user_id=users_table.user_id)
  AND  EXISTS (SELECT user_id FROM events_table WHERE event_type=101 AND value_3 > 100 AND user_id=users_table.user_id);

------------------------------------
-- Customers who have done X and haven’t done Y, and satisfy other customer specific criteria
-- This query will be supported when we have subqueries in where clauses.
------------------------------------
SELECT user_id, value_2 FROM users_table WHERE
  value_2 >= 5
  AND  EXISTS (SELECT user_id FROM events_table WHERE event_type > 100 AND event_type <= 300 AND value_3 > 100 AND user_id=users_table.user_id)
  AND  NOT EXISTS (SELECT user_id FROM events_table WHERE event_type > 300 AND event_type <= 350  AND value_3 > 100 AND user_id=users_table.user_id);

------------------------------------
-- Customers who have done X more than 2 times, and satisfy other customer specific criteria
-- This query will be supported when we have subqueries in where clauses.
------------------------------------
SELECT user_id, 
         value_2 
  FROM   users_table
  WHERE  value_1 > 100 
         AND value_1 < 124 
         AND value_2 >= 5 
         AND EXISTS (SELECT user_id 
                     FROM   events_table
                     WHERE  event_type > 100 
                            AND event_type < 124 
                            AND value_3 > 100 
                            AND user_id = users_table.user_id 
                     GROUP  BY user_id 
                     HAVING Count(*) > 2);
                     
------------------------------------
-- Find me all users_table who logged in more than once
------------------------------------
SELECT user_id, value_1 from
(
  SELECT user_id, value_1 From users_table
  WHERE value_2 > 100 and user_id = 15 GROUP BY value_1, user_id HAVING count(*) > 1
) AS a
ORDER BY user_id ASC, value_1 ASC;

-- same query with additional filter to make it not router plannable
SELECT user_id, value_1 from
(
  SELECT user_id, value_1 From users_table
  WHERE value_2 > 100 and (user_id = 15 OR user_id = 16) GROUP BY value_1, user_id HAVING count(*) > 1
) AS a
ORDER BY user_id ASC, value_1 ASC;

------------------------------------
-- Find me all users_table who has done some event and has filters
-- This query will be supported when we have subqueries in where clauses.
------------------------------------
SELECT user_id
FROM events_table
WHERE
	event_type = 16 AND value_2 > 50
AND user_id IN
  (SELECT user_id
   FROM users_table
   WHERE
   	value_1 = 15 AND value_2 > 25);  

------------------------------------
-- Which events_table did people who has done some specific events_table
-- This query will be supported when we have subqueries in where clauses.
------------------------------------
SELECT user_id, event_type FROM events_table
WHERE user_id in (SELECT user_id from events_table WHERE event_type > 500 and event_type < 505)
GROUP BY user_id, event_type;

------------------------------------
-- Find me all the users_table who has done some event more than three times
------------------------------------    
SELECT user_id FROM
(
  SELECT 
     user_id
  FROM 
   	events_table
  WHERE event_type = 901
  GROUP BY user_id HAVING count(*) > 3
) AS a
ORDER BY user_id;
       
------------------------------------
-- Find my assets that have the highest probability and fetch their metadata
------------------------------------
CREATE TEMP TABLE assets AS
SELECT
    users_table.user_id, users_table.value_1, prob
FROM 
   users_table
        JOIN 
   (SELECT  
      ma.user_id, (GREATEST(coalesce(ma.value_4 / 250, 0.0) + GREATEST(1.0))) / 2 AS prob
    FROM 
    	users_table AS ma, events_table as short_list
    WHERE 
    	short_list.user_id = ma.user_id and ma.value_1 < 50 and short_list.event_type < 50
    ) temp 
  ON users_table.user_id = temp.user_id 
  WHERE users_table.value_1 < 50;

  -- get some statistics from the aggregated results to ensure the results are correct
SELECT count(*), count(DISTINCT user_id), avg(user_id) FROM assets;

DROP TABLE assets;

-- count number of distinct users who have value_1 equal to 5 or 13 but not 3
-- original query that fails
SELECT count(*) FROM
(
SELECT user_id
FROM users_table
WHERE (value_1 = '5'
       OR value_1 = '13')
AND user_id NOT IN (select user_id from users_table where value_1 = '3')
GROUP BY user_id
HAVING count(distinct value_1) = 2
) as foo;

-- previous push down query
SELECT subquery_count FROM
    (SELECT count(*) as subquery_count FROM                                                                         
        (SELECT 
            user_id
        FROM
            users_table
        WHERE
            (value_1 = '5' OR value_1 = '13')
        GROUP BY user_id                                                                          
        HAVING count(distinct value_1) = 2) as a                                                                       
        LEFT JOIN                                                                                      
        (SELECT
            user_id 
        FROM
            users_table            
        WHERE
            (value_1 = '3')
        GROUP BY user_id) as b on a.user_id = b.user_id WHERE b.user_id IS NULL
    GROUP BY a.user_id) AS inner_subquery;

-- new pushdown query without single range table entry at top requirement
SELECT count(*) as subquery_count
FROM (
	SELECT 
    	user_id
    FROM
		users_table
	WHERE
		(value_1 = '5' OR value_1 = '13')
	GROUP BY user_id                                                                          
	HAVING count(distinct value_1) = 2
	) as a
	LEFT JOIN (
	SELECT
		user_id 
	FROM
		users_table            
	WHERE
		(value_1 = '3')
	GROUP BY user_id) AS b 
	ON a.user_id = b.user_id 
WHERE b.user_id IS NULL
GROUP BY a.user_id;
   
SET citus.subquery_pushdown TO FALSE;
SET citus.enable_router_execution TO TRUE;
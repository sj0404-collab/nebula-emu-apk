create table t(id integer primary key, name text, v real);
insert into t(name, v) select char(65 + abs(random()) % 26), random() % 1000 from generate_series(1, 2000);
create index t_name on t(name);
select count(*), min(v), max(v), round(avg(v), 2) from t;
select name, count(*) n from t group by name order by n desc limit 5;
pragma integrity_check;

--
-- insert with DEFAULT in the target_list
--
create table inserttest (col1 int4, col2 int4 NOT NULL, col3 text default 'testing');
insert into inserttest (col1, col2, col3) values (DEFAULT, DEFAULT, DEFAULT);
insert into inserttest (col2, col3) values (3, DEFAULT);
insert into inserttest (col1, col2, col3) values (DEFAULT, 5, DEFAULT);
insert into inserttest values (DEFAULT, 5, 'test');
insert into inserttest values (DEFAULT, 7);

select * from inserttest;
drop table inserttest;

BEGIN;
ALTER TABLE staff ENABLE REPLICATION;
ALTER TABLE staff ENABLE REPLICATION ON SLAVE 0;
ALTER TABLE staff ENABLE REPLICATION ON SLAVE 1;
ALTER TABLE presence ENABLE REPLICATION;
ALTER TABLE presence ENABLE REPLICATION ON SLAVE 0;
ALTER TABLE presence ENABLE REPLICATION ON SLAVE 1;
COMMIT;

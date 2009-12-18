CREATE TRIGGER repl_relations BEFORE INSERT OR UPDATE OR DELETE ON pg_catalog.repl_relations FOR EACH ROW EXECUTE PROCEDURE replicate_relations();
CREATE TRIGGER repl_slave_relations BEFORE INSERT OR UPDATE OR DELETE ON pg_catalog.repl_slave_relations FOR EACH ROW EXECUTE PROCEDURE replicate_slave_relations();
CREATE TRIGGER repl_authid BEFORE INSERT OR UPDATE OR DELETE ON pg_catalog.repl_authid FOR EACH ROW EXECUTE PROCEDURE replicate_authid();
CREATE TRIGGER repl_auth_members BEFORE INSERT OR UPDATE OR DELETE ON pg_catalog.repl_auth_members FOR EACH ROW EXECUTE PROCEDURE replicate_auth_members();
CREATE TRIGGER repl_acl BEFORE INSERT OR UPDATE OR DELETE ON pg_catalog.repl_acl FOR EACH ROW EXECUTE PROCEDURE replicate_acl();
CREATE TRIGGER repl_lo_columns BEFORE INSERT OR UPDATE OR DELETE ON pg_catalog.repl_lo_columns FOR EACH ROW EXECUTE PROCEDURE replicate_lo_columns();
CREATE TRIGGER drop_replicated_role BEFORE DELETE ON pg_catalog.repl_slave_roles FOR EACH ROW EXECUTE PROCEDURE drop_replicated_role();

--- Grant permissions to all pg_catalog.repl_ relations using pg_class permissions as a template.
UPDATE pg_class SET relacl = (SELECT relacl FROM pg_class WHERE relname = 'pg_class') WHERE relkind IN ('r', 'v', 'S') AND relacl IS NULL AND relname LIKE 'repl_%';
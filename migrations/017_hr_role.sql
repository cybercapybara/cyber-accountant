-- Роль кадровика (спека P3 §5). CHECK в migrations/006_organizations.sql
-- безымянный, поэтому снимается по автоимени, которое Postgres ему дал:
-- <таблица>_<колонка>_check = org_members_role_check.
--
-- Расширение множества ролей БЕЗ матрицы прав (src/tenancy/OrgPermissions.hpp,
-- задача 6 плана) — это fail-open: старый контроль был денилистом
-- `role == 'viewer'`, и новая роль прошла бы его насквозь, получив полный
-- CRUD на журнал, налоги и зарплату. Матрица обязана существовать ДО этой
-- миграции — она появилась в d879f49, вместе с read-гейтами на каждый
-- org-scoped GET (задача 7), которые едут в одном коммите с этим файлом:
-- принять 'hr' в БД раньше, чем чтения загейтены, значит выдать кадровику
-- весь журнал, все зарплатные ведомости и всю налоговую отчётность.
--
-- NOTE: MigrationRunner wraps this file in ONE transaction (under an
-- advisory lock) and records schema_migrations in that same transaction.
-- Do NOT add BEGIN/COMMIT here.

ALTER TABLE org_members DROP CONSTRAINT IF EXISTS org_members_role_check;
ALTER TABLE org_members ADD CONSTRAINT org_members_role_check
    CHECK (role IN ('owner', 'accountant', 'hr', 'viewer'));

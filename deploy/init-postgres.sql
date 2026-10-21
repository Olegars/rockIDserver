SELECT format('CREATE ROLE %I LOGIN', 'oleg')
WHERE NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'oleg')
\gexec

SELECT format('CREATE DATABASE %I OWNER %I', 'rockid', 'oleg')
WHERE NOT EXISTS (SELECT FROM pg_database WHERE datname = 'rockid')
\gexec

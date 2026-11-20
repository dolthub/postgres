#!/bin/bash
# Writes pg19_orders.tsv from a Postgres 19 server, in a scratch database with the C collation. The server is chosen
# by the usual libpq variables (PGHOST, PGPORT, PGUSER, PGPASSWORD).
set -euo pipefail
cd "$(dirname "$0")"
psql -X -q -c "DROP DATABASE IF EXISTS encoding_orders" -c "CREATE DATABASE encoding_orders TEMPLATE template0 LOCALE 'C'"
trap 'psql -X -q -c "DROP DATABASE IF EXISTS encoding_orders"' EXIT
psql -X -q -At -F $'\t' -d encoding_orders -f pg19_orders.sql > pg19_orders.tsv

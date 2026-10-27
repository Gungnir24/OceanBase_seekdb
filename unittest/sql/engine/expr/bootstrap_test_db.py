#!/usr/bin/env python3
"""Create the competition test account and database on a fresh seekdb instance."""
import sys

import pymysql

STATEMENTS = [
    "CREATE USER IF NOT EXISTS 'admin' IDENTIFIED BY 'admin'",
    "GRANT ALL ON *.* TO 'admin' WITH GRANT OPTION",
    "CREATE DATABASE IF NOT EXISTS test",
]


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 2881
    conn = pymysql.connect(host='127.0.0.1', port=port, user='root', password='',
                           autocommit=True)
    with conn.cursor() as cursor:
        for statement in STATEMENTS:
            cursor.execute(statement)
            print('ok:', statement)
    conn.close()


if __name__ == '__main__':
    main()

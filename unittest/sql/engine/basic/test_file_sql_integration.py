#!/usr/bin/env python3
"""End-to-end file SQL regression against a dedicated seekdb test instance.
Usage: python3 test_file_sql_integration.py --root /allowed/fixture-dir --port 32847
Requires public basic.csv, basic.jsonl and simple.parquet in the fixture directory.
Uses the mysql client; no external SQL engine or runtime Python dependency.
"""
import argparse,subprocess,pathlib,sys,os,time
p=argparse.ArgumentParser();p.add_argument('--root',required=True);p.add_argument('--port',default='32847');p.add_argument('--mysql',default='mysql');a=p.parse_args()
root=pathlib.Path(a.root).resolve()
def q(s):return "'"+str(s).replace('\\','\\\\').replace("'","''")+"'"
def file(name):return q(root/name)
def run(sql,enabled=True,error=None):
 sql=('SET SESSION enable_file_sql=ON;\n' if enabled else '')+sql
 proc=subprocess.run([a.mysql,'-h127.0.0.1','-P'+a.port,'-uroot','--batch','--raw','--skip-column-names','--default-character-set=utf8mb4','-e',sql],capture_output=True,text=True)
 if error is not None:
  assert proc.returncode and (error==True or str(error) in proc.stderr),(sql,proc.stdout,proc.stderr);return proc.stderr
 assert proc.returncode==0,(sql,proc.stdout,proc.stderr)
 return proc.stdout.rstrip('\n')
def eq(sql,expected,**kw):
 value=run(sql,**kw);assert value==expected,(sql,expected,value)
csv=f'read_csv({file("basic.csv")})';js=f'read_json({file("basic.jsonl")})';pq=f'read_parquet({file("simple.parquet")})'
cases=[]
def test(name):
 def wrap(fn):cases.append((name,fn));return fn
 return wrap
@test('default disabled and ordinary SQL')
def _():
 eq('SELECT @@session.enable_file_sql','0',enabled=False)
 run('SELECT COUNT(*) FROM '+csv,enabled=False,error=1235)
 eq('SELECT 1+1, CONCAT(\'normal\',\' SQL\')','2\tnormal SQL',enabled=False)
@test('CSV values, aliases, filters, aggregates, HAVING')
def _():
 eq(f'SELECT id,name,id__2 FROM {csv} f ORDER BY id','1\tAlice\t101\n2\tBob\t102\n3\tChen\t103')
 eq(f'SELECT count(*),sum(id),round(avg(score),2),sum(active) FROM {csv}','3\t6\t91.92\t2')
 eq(f'SELECT id*2 AS doubled FROM {csv} WHERE score>=90 ORDER BY id DESC LIMIT 1','6')
 eq(f'SELECT active,count(*),sum(id) FROM {csv} GROUP BY active HAVING count(*)>=1 ORDER BY active','0\t1\t2\n1\t2\t4')
 eq(f"SELECT replace(note,char(10),'<NL>'),day,created FROM {csv} WHERE id=1",'hello,<NL>world\t2026-08-01\t2026-08-01 10:11:12')
@test('named options and JSON union schema')
def _():
 eq(f'SELECT count(*) FROM read_csv({file("basic.csv")},header=true,sample_size=-1)','3')
 eq(f'SELECT id,name,score,active,day,note FROM {js} ORDER BY id','1\tAlice\t98.5\t1\tNULL\tNULL\n2\tBob\tNULL\t0\t2026-08-02\tNULL\n3\tChen\t90\t1\tNULL\tok')
 eq(f'SELECT count(*) FROM read_ndjson({file("basic.jsonl")})','3')
 run(f'SELECT * FROM read_csv({file("basic.csv")},sample_size=10)',error=1235)
 run(f"SELECT * FROM read_csv({file('basic.csv')},'xml')",error=1064)
@test('DESCRIBE dynamic columns')
def _():
 rows=run('DESCRIBE SELECT * FROM '+csv).splitlines()
 assert len(rows)==8,rows
 assert rows[0]=='id\tBIGINT\tNO\tNULL\tNULL\tNULL',rows
 assert rows[3].startswith('active\tBOOLEAN\tNO'),rows
 assert rows[4].startswith('day\tDATE\tYES'),rows
@test('Parquet and projected columns')
def _():eq(f'SELECT count(*),min(c1),max(c1),sum(c1) FROM {pq}','7\t0\t6\t21')
@test('native INNER/LEFT and cross-format JOIN')
def _():
 eq(f'SELECT a.id,b.name FROM {csv} a INNER JOIN {js} b ON a.id=b.id ORDER BY a.id','1\tAlice\n2\tBob\n3\tChen')
 eq(f'SELECT a.id,b.name FROM {csv} a LEFT JOIN (SELECT * FROM {js} WHERE id=2) b ON a.id=b.id ORDER BY a.id','1\tNULL\n2\tBob\n3\tNULL')
 eq(f'SELECT count(*) FROM {pq} p INNER JOIN {csv} c ON p.c1=c.id','3')
 plan=run(f'EXPLAIN SELECT a.name,b.score FROM {csv} a JOIN {js} b ON a.id=b.id')
 assert 'JOIN' in plan.upper() and 'FUNCTION' in plan.upper(),plan
@test('glob discovery')
def _():
 names=run(f"SELECT substring_index(file,'/',-1) FROM glob({q(str(root/'*'))}) ORDER BY file").splitlines()
 assert all(x in names for x in ['basic.csv','basic.jsonl','simple.parquet']),names
@test('COPY roundtrip, target collision and rollback')
def _():
 out=root/'integration_export.csv';out.unlink(missing_ok=True)
 run(f"COPY (SELECT id,name,score FROM {csv} ORDER BY id) TO {q(out)} (FORMAT CSV, HEADER, NULLSTR 'NULL')")
 eq(f'SELECT id,name,score FROM read_csv({q(out)}) ORDER BY id','1\tAlice\t98.5\n2\tBob\t87\n3\tChen\t90.25')
 before=out.read_bytes();run(f'COPY (SELECT 9) TO {q(out)} (HEADER)',error=True);assert out.read_bytes()==before
 failed=root/'integration_failed.csv';failed.unlink(missing_ok=True)
 run(f"COPY (SELECT id,JSON_EXTRACT(name,'$') FROM {csv}) TO {q(failed)} (HEADER)",error=True)
 assert not failed.exists();assert not list(root.glob('.seekdb-copy-*'))
@test('COPY regular table and custom CSV options')
def _():
 out=root/'integration_table.csv';out.unlink(missing_ok=True)
 run('CREATE DATABASE IF NOT EXISTS file_sql_regression; CREATE TABLE IF NOT EXISTS file_sql_regression.t(id INT,txt VARCHAR(50)); DELETE FROM file_sql_regression.t; INSERT INTO file_sql_regression.t VALUES(1,\'a;"b\'),(2,NULL)')
 run(f"COPY file_sql_regression.t TO {q(out)} (FORMAT CSV, HEADER, DELIMITER ';', ESCAPE '!', NULLSTR 'NIL')")
 eq(f"SELECT id,txt FROM read_csv({q(out)},delimiter=';',escape='!',nullstr='NIL') ORDER BY id",'1\ta;"b\n2\tNULL')
@test('schema refresh and all-NULL columns')
def _():
 path=root/'integration_changing.csv';path.write_text('a\n1\n',encoding='utf-8')
 sql=f'SELECT count(*) FROM read_csv({q(path)})';eq(sql,'1')
 path.write_text('b\n2\n3\n',encoding='utf-8');eq(sql,'2');eq(f'SELECT sum(b) FROM read_csv({q(path)})','5')
 path.write_text('x\nNULL\nNULL\n',encoding='utf-8');eq(f'SELECT count(x),count(*) FROM read_csv({q(path)})','0\t2')
 assert 'x\tNULL\tYES' in run(f'DESCRIBE SELECT * FROM read_csv({q(path)})')
@test('paths, malformed data and feature switch')
def _():
 run(f'SELECT * FROM read_csv({q(str(root)+"/../outside.csv")})',error=True)
 link=root/'integration_escape.csv';link.unlink(missing_ok=True);link.symlink_to('/etc/passwd')
 run(f'SELECT * FROM read_csv({q(link)})',error=True);link.unlink()
 path=root/'integration_bad.jsonl';path.write_text('{"x":[1]}\n');run(f'SELECT * FROM read_json({q(path)})',error=True)
 path=root/'integration_bad.csv';path.write_text('a,b\n1,2,3\n');run(f'SELECT * FROM read_csv({q(path)})',error=True)
 run('SET SESSION enable_file_sql=OFF; SELECT * FROM '+csv,error=1235)
@test('Parquet V1/V2, nullable columns, dates and multiple pages')
def _():
 for name in ['multi_1_none.parquet','multi_1_snappy.parquet','multi_2_none.parquet','multi_2_snappy.parquet']:
  source=f'read_parquet({file(name)})'
  eq(f'SELECT count(*),sum(id),count(empty),sum(active) FROM {source}','9\t45\t0\t4')
  eq(f'SELECT name,day,created FROM {source} WHERE id=1','alpha\t2024-02-29\t2024-02-29 12:30:01.123456')
 eq(f'SELECT count(*),sum(id) FROM read_parquet({file("paged_snappy.parquet")})','5000\t12497500')
@test('numeric boundaries, invalid dates and UTF-8 headers')
def _():
 path=root/'integration_types.csv'
 path.write_text('lo,hi,overflow,invalid_date,姓名\n-9223372036854775808,9223372036854775807,9223372036854775808,2023-02-29,中文\n',encoding='utf-8')
 eq(f'SELECT lo,hi,overflow,invalid_date,`姓名` FROM read_csv({q(path)})','-9223372036854775808\t9223372036854775807\t9223372036854775808\t2023-02-29\t中文')
 meta=run(f'DESCRIBE SELECT * FROM read_csv({q(path)})');assert 'overflow\tVARCHAR' in meta and 'invalid_date\tVARCHAR' in meta,meta
@test('glob directory wildcards')
def _():
 nested=root/'integration_glob';nested.mkdir(exist_ok=True);(nested/'a.csv').write_text('id\n1\n')
 eq(f"SELECT substring_index(file,'/',-1) FROM glob({q(str(root/'integration_*'/'*.csv'))}) ORDER BY file",'a.csv')
@test('forced nested-loop rescan and file changes during streaming')
def _():
 eq(f'SELECT /*+ USE_NL(b) */ count(*) FROM {csv} a JOIN {js} b ON a.id<=b.id','6')
 path=root/'integration_mutation.csv';path.write_text('id\n'+''.join(str(i)+'\n' for i in range(5000)),encoding='utf-8')
 sql=f'SET SESSION enable_file_sql=ON; SELECT SUM(SLEEP(0.002)+id) FROM read_csv({q(path)})'
 proc=subprocess.Popen([a.mysql,'-h127.0.0.1','-P'+a.port,'-uroot','--batch','-e',sql],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
 try:
  time.sleep(0.5)
  with path.open('a',encoding='utf-8') as stream:stream.write('5001\n')
  out,err=proc.communicate(timeout=20)
  assert proc.returncode!=0 and 'changed' in err.lower(),(out,err)
 finally:
  if proc.poll() is None:proc.kill();proc.communicate()
# Refuse to run feature assertions against an unavailable or different server.
try:run('SELECT @@session.enable_file_sql',enabled=False)
except Exception as exc:print('TEST INSTANCE NOT READY:',repr(exc));sys.exit(2)
failures=0
for name,fn in cases:
 try:fn();print('PASS',name,flush=True)
 except Exception as exc:failures+=1;print('FAIL',name,repr(exc),flush=True)
print(f'{len(cases)-failures}/{len(cases)} integration groups passed',flush=True)
sys.exit(bool(failures))

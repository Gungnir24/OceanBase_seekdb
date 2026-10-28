"""Generate independent Parquet test inputs; pyarrow is a TEST-ONLY dependency."""
import datetime,pathlib
import pyarrow as pa
import pyarrow.parquet as pq
out=pathlib.Path(__file__).parent/'data'
out.mkdir(exist_ok=True)
table=pa.table({
 'id':pa.array(range(1,10),type=pa.int64()),
 'nullable':pa.array([1,None,3,4,None,6,7,8,9],type=pa.int32()),
 'name':pa.array(['alpha','中文',None,'delta','alpha','x,y','say "hi"','line\nbreak','end']),
 'active':pa.array([True,False,None,True,False,True,None,False,True]),
 'score':pa.array([1.25,None,3.5,4.,5.5,6.,7.25,8.,9.],type=pa.float64()),
 'day':pa.array([datetime.date(2024,2,29),None]+[datetime.date(2024,3,i) for i in range(1,8)],type=pa.date32()),
 'created':pa.array([datetime.datetime(2024,2,29,12,30,1,123456)]*9,type=pa.timestamp('us')),
 'empty':pa.array([None]*9,type=pa.int64())})
for version in ['1.0','2.0']:
 for codec,dictionary in [('NONE',False),('snappy',True)]:
  name='multi_'+version[0]+'_'+codec.lower()+'.parquet'
  pq.write_table(table,out/name,row_group_size=3,compression=codec,use_dictionary=dictionary,data_page_version=version,write_statistics=True)
large=pa.table({'id':pa.array(range(5000),type=pa.int64()),'text':pa.array([None if i%11==0 else 'row-'+str(i%17) for i in range(5000)])})
pq.write_table(large,out/'paged_snappy.parquet',row_group_size=2700,data_page_size=1024,write_batch_size=128,compression='snappy',data_page_version='2.0')
print('Generated',len(list(out.glob('*.parquet'))),'Parquet fixtures')

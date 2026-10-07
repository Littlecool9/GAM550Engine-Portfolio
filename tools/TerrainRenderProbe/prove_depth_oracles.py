"""Independently check every recorded pixel and reject injected defects."""
import argparse,json,struct,hashlib
from pathlib import Path
import numpy as np

def cross(a,b):return a[...,0]*b[...,1]-a[...,1]*b[...,0]
def plane(p,i):return [p[3]-p[2],p[2],p[3]+p[0],p[3]-p[0],p[3]+p[1],p[3]-p[1]][i]
def clip(poly,i):
 if not poly:return []
 out=[];prev=poly[-1];dp=plane(prev,i)
 for cur in poly:
  dc=plane(cur,i)
  if (dc>=0)!=(dp>=0):out.append(prev+(cur-prev)*(dp/(dp-dc)))
  if dc>=0:out.append(cur)
  prev=cur;dp=dc
 return out

def triangles(root,case,w,h):
 vp=np.array(case['viewProjColumnMajor'],np.float32).reshape((4,4),order='F')
 offset=np.array(case['camera'])-np.array(case['placement'])
 selected={tuple(row['tile']):row for row in case['selected']}
 recipe='four-source' if case['name'].startswith('four-source') else case['name'].split('-sourceLOD')[0].split('-mixedLOD')[0]
 for file in (root/'releases'/recipe).glob('*.tpack'):
  b=file.read_bytes();at=0
  while True:
   at=b.find(b'LOD1',at)
   if at<0:break
   ver,tx,tz=struct.unpack_from('<Iii',b,at+4);src,sp=struct.unpack_from('<dd',b,at+16);n,ni=struct.unpack_from('<II',b,at+56)
   record=selected.get((tx,tz))
   if ver==1 and record and src==record['sourceSpacing'] and sp==record['renderSpacing']:
    vertices=np.frombuffer(b,'<f4',n*3,at+80).reshape(n,3).copy();ids=np.frombuffer(b,'<u4',ni,at+80+n*12).reshape(-1,3)
    vertices+=np.array([tx*64,0,tz*64],np.float32)-offset.astype(np.float32)
    projected=np.c_[vertices,np.ones(n,np.float32)]@vp.T
    for primitive,index in enumerate(ids):
     poly=[np.array(projected[j],np.float64) for j in index]
     for i in range(6):poly=clip(poly,i)
     for fan in range(1,len(poly)-1):
      q=np.array([poly[0],poly[fan],poly[fan+1]])
      if np.any(q[:,3]<=0) or not np.all(np.isfinite(q)):continue
      nd=q[:,:3]/q[:,3:]
      screen=np.c_[(nd[:,:2]+1)*[w/2,h/2],nd[:,2]]
      if cross(screen[1,:2]-screen[0,:2],screen[2,:2]-screen[0,:2])<0:
       yield screen,(tx,tz,primitive)
   at+=4

def render(root,case,w,h,bits):
 buffers=[np.zeros((h,w),np.float64),np.zeros((h,w),np.float64)]
 ids=[np.zeros((h,w),np.int64)-1,np.zeros((h,w),np.int64)-1]
 nodes=[]
 for original,key in triangles(root,case,w,h):
  ident=len(nodes);nodes.append({'tile':key[:2],'primitive':key[2],'screenUnquantized':original.tolist()})
  for variant in [0,1]:
   triangle=original.copy()
   if variant:
    ndc=(triangle[:,:2]/np.array([w/2,h/2])-1).astype(np.float32)
    viewport=(ndc+np.float32(1))*np.array([w,h],np.float32)*np.float32(.5)
    triangle[:,:2]=np.rint(viewport.astype(np.float64)*2**bits)/2**bits
    triangle[:,2]=triangle[:,2].astype(np.float32)
   a,b,c=triangle;area=cross(b[:2]-a[:2],c[:2]-a[:2])
   if area>=0:continue
   left=max(0,int(np.floor(min(a[0],b[0],c[0]))));right=min(w-1,int(np.ceil(max(a[0],b[0],c[0]))))
   top=max(0,int(np.floor(min(a[1],b[1],c[1]))));bottom=min(h-1,int(np.ceil(max(a[1],b[1],c[1]))))
   if right<left or bottom<top:continue
   ys,xs=np.mgrid[top:bottom+1,left:right+1];p=np.stack([xs+.5,ys+.5],axis=-1)
   u=cross(p-a[:2],c[:2]-a[:2])/area;v=cross(b[:2]-a[:2],p-a[:2])/area;ww=1-u-v
   inside=(u>=0)&(v>=0)&(ww>=0);depth=ww*a[2]+u*b[2]+v*c[2]
   destination=buffers[variant][top:bottom+1,left:right+1];idbuffer=ids[variant][top:bottom+1,left:right+1]
   mask=inside&(depth>destination);destination[mask]=depth[mask];idbuffer[mask]=ident
 return buffers,ids,nodes

def validation_errors(actual,oracle,normals=None):
 errors=[]
 if not np.all(np.isfinite(actual)):errors.append('nonfinite')
 if np.any((actual>0)!=(oracle>0)):errors.append('hit-mask')
 if np.any(np.abs(actual-oracle)>=2e-6):errors.append('depth-2e-6')
 if normals is not None:
  visible=actual>0
  if not np.all(np.isfinite(normals[visible])):errors.append('nonfinite-normal')
  if np.any(np.abs(np.linalg.norm(normals[visible],axis=-1)-1)>=2e-5):errors.append('normal-length')
 return errors

def main():
 parser=argparse.ArgumentParser();parser.add_argument('directory',type=Path);parser.add_argument('--out',type=Path,required=True);args=parser.parse_args()
 root=args.directory;matrix=json.loads((root/'matrix.json').read_text());w,h=matrix['width'],matrix['height'];results=[]
 for case in matrix['cases']:
  meta=json.loads((root/(case['rasterCapture']+'.depth.json')).read_text());bits=meta['rasterSubPixelPrecisionBits']
  expected,ids,nodes=render(root,case,w,h,bits)
  depth=np.fromfile(root/(case['rasterCapture']+'.depth.f32'),'<f4').reshape(h,w)
  traced=np.fromfile(root/(case['rtCapture']+'.depth.f32'),'<f4').reshape(h,w,4)
  vp=np.array(case['viewProjColumnMajor']).reshape((4,4),order='F');inverse=np.linalg.inv(vp.astype(np.float32)).astype(np.float32)
  yy,xx=np.mgrid[:h,:w];near=np.stack([(xx+.5)/w*2-1,(yy+.5)/h*2-1,np.ones((h,w)),np.ones((h,w))],axis=-1)@inverse.astype(np.float64).T
  direction=near[:,:,:3]/near[:,:,3:];direction/=np.linalg.norm(direction,axis=-1,keepdims=True)
  points=direction*traced[:,:,0:1];projected=np.concatenate([points,np.ones((h,w,1))],axis=-1)@vp.T
  rtdepth=np.divide(projected[:,:,2],projected[:,:,3],out=np.zeros((h,w)),where=traced[:,:,0]>0)
  rtdepth[traced[:,:,0]==0]=0
  inputs=[root/'matrix.json',root/(case['rasterCapture']+'.depth.f32'),root/(case['rtCapture']+'.depth.f32')]
  record={'name':case['name'],'rasterSubPixelPrecisionBits':bits,'pixels':w*h,
    'inputSha256':{f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}}
  for name,actual,want in [('raster',depth,expected[1]),('rt',rtdepth,expected[0])]:
   both=(actual>0)&(want>0);difference=np.abs(actual-want)
   record[name]={'actualHitPixels':int(np.count_nonzero(actual>0)),'oracleHitPixels':int(np.count_nonzero(want>0)),
     'maskDifferences':int(np.count_nonzero((actual>0)!=(want>0))),'maxDepthErrorOnBothHits':float(np.max(difference[both])),
     'depthFailures2e6':int(np.count_nonzero(both&(difference>=2e-6)))}
  changed=(ids[0]!=ids[1])&(ids[0]>=0)&(ids[1]>=0)
  record['primitiveCoverageChangedPixels']=int(np.count_nonzero(changed))
  indices=np.flatnonzero(changed)
  if len(indices):
   i=int(indices[0]);y,x=divmod(i,w);record['representativeCoverageChange']={'pixel':[x,y],'rt':nodes[ids[0][y,x]],'raster':nodes[ids[1][y,x]],'rtOracleDepth':float(expected[0][y,x]),'rasterOracleDepth':float(expected[1][y,x]),'actualRtDepth':float(rtdepth[y,x]),'actualRasterDepth':float(depth[y,x])}
  # Each path retains its own assertion on every hit; mutations are never used as evidence.
  visible=np.flatnonzero((expected[0]>0)&(expected[1]>0));i=int(visible[len(visible)//2]);y,x=divmod(i,w)
  record['allPixelValidationErrors']={'raster':validation_errors(depth,expected[1]),'rt':validation_errors(rtdepth,expected[0],traced[:,:,1:])}
  mutations={}
  for name,actual,oracle,normals in [('raster',depth,expected[1],None),('rt',rtdepth,expected[0],traced[:,:,1:])]:
   for fault in ['wrong-depth','missing-hit','nonfinite']:
    modified=actual.copy()
    modified[y,x]=modified[y,x]+1e-4 if fault=='wrong-depth' else 0 if fault=='missing-hit' else np.nan
    errors=validation_errors(modified,oracle,normals)
    mutations[name+'-'+fault]={'rejected':bool(errors),'failureCodes':errors}
  modifiedNormals=traced[:,:,1:].copy();modifiedNormals[y,x]*=2
  errors=validation_errors(rtdepth,expected[0],modifiedNormals)
  mutations['rt-nonunit-normal']={'rejected':bool(errors),'failureCodes':errors}
  record['faultInjection']=mutations

  results.append(record);args.out.write_text(json.dumps({'origin':'independent CPU projection against unchanged real GPU images','threshold2e6':True,'cases':results},indent=2)+'\n',encoding='utf-8')
  print(record['name'],record['raster'],record['rt'],flush=True)
if __name__=='__main__':main()

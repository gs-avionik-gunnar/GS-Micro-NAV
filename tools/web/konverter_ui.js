/* ===== Konverter-Oberflaeche ===== */
const KSRC=[],KL=[];let KOUT=null,KNAMEUSER=false;
const kEl=id=>document.getElementById(id);
function kLog(t){const p=kEl('klog');p.style.display='block';p.textContent=t;}
async function kAdd(files){
 const msgs=[];
 for(const f of files){
  try{
   const n=f.name.toLowerCase();let list=[];
   if(n.endsWith('.zip')){
    const es=await zipEntries(await f.arrayBuffer());
    const cand=es.filter(e=>/\.(txt|csv|json|geojson)$/i.test(e.name));
    const seeyou=cand.some(e=>/seeyou/i.test(e.name));
    for(const e of cand){
     if(/\.txt$/i.test(e.name)&&seeyou&&!/seeyou/i.test(e.name))continue;
     list.push({name:f.name+'/'+e.name,text:await e.text(),off:/embedded/i.test(e.name)});
    }
   }else list.push({name:f.name,text:await f.text()});
   for(const it of list){
    const s=vmSource(it.name,it.text);
    if(!s){msgs.push(it.name+': Format nicht erkannt');continue;}
    KSRC.push(s);
    for(const p of vmPresets(s))KL.push(Object.assign({src:s,on:!it.off,flt:'',nameField:'',tol:'',skip:''},p));
    msgs.push(it.name+': '+(s.kind==='oa'?'OpenAir':s.feats.length+' Einträge')+(it.off?' (Embedded-Variante, standardmäßig aus)':''));
   }
  }catch(e){msgs.push(f.name+': '+e.message);}
 }
 kLog(msgs.join('\n'));kRender();
}
function kAuto(){
 if(KNAMEUSER)return;
 const on=KL.filter(l=>l.on);
 const oa=on.length&&on.every(l=>l.typ==='oa'),ap=on.length&&on.every(l=>l.src.kind==='csv');
 kEl('kname').value=oa?'ovl_luftraum.vmap':ap?'ovl_flugplaetze.vmap':'karte.vmap';
}
function kRender(){
 const box=kEl('klayers');box.innerHTML='';
 KL.forEach((L,i)=>{
  const d=document.createElement('div');d.className='kl';
  const r1=document.createElement('div');r1.className='kr';
  const cb=document.createElement('input');cb.type='checkbox';cb.checked=L.on;cb.onchange=()=>{L.on=cb.checked;kAuto();};
  const nm=document.createElement('span');nm.className='kn';nm.textContent=L.src.name.split('/').pop();nm.title=L.src.name;
  const sel=document.createElement('select');
  VM_TYPES.forEach(([v,t])=>{const o=document.createElement('option');o.value=v;o.textContent=t;if(String(v)===String(L.typ))o.selected=true;sel.append(o);});
  sel.onchange=()=>{L.typ=sel.value==='oa'?'oa':Number(sel.value);kAuto();kRender();};
  const x=document.createElement('button');x.textContent='✕';x.className='d';x.style.flex='0 0 auto';x.onclick=()=>{KL.splice(i,1);kAuto();kRender();};
  r1.append(cb,nm,sel,x);d.append(r1);
  const r2=document.createElement('div');r2.className='kr';
  const mk=(ph,key,type,cls)=>{const e=document.createElement('input');e.placeholder=ph;e.title=ph;e.value=L[key]===undefined?'':L[key];if(type)e.type=type;
   e.oninput=()=>{L[key]=e.value;};return e;};
  if(L.typ==='oa'){r2.append(mk('Klassen weglassen, z.B. W|G','skip'),mk('Toleranz m','tol','number'));}
  else{const f=mk('Filter: key=wert|wert:key>=zahl','flt');f.style.flex='3 1 220px';r2.append(f);
   if(Number(L.typ)>=10)r2.append(mk('Namensfeld (z.B. name, ident)','nameField'));
   else r2.append(mk('Toleranz m','tol','number'));}
  d.append(r2);box.append(d);
 });
 kAuto();
}
function kBB(){const t=kEl('kbb').value.trim();if(!t)return null;
 const b=t.split(/[\s,;]+/).map(Number);if(b.length!==4||b.some(isNaN))throw new Error('BBox: vier Zahlen W S O N');return b;}
async function kConvert(){
 try{
  const L=KL.filter(l=>l.on);if(!L.length)throw new Error('Keine Ebene aktiv - bitte zuerst Quelldateien wählen.');
  kLog('Konvertiere ...');
  const r=await vmConvert(L,{bbox:kBB(),tol:Number(kEl('ktol').value)||0});
  let nm=kEl('kname').value.trim()||'karte.vmap';if(!/\.vmap$/i.test(nm))nm+='.vmap';
  KOUT={blob:new Blob([r.buf],{type:'application/octet-stream'}),name:nm,buf:r.buf};
  kLog(r.log.join('\n')+'\n\nDatei: '+nm+' ('+fmt(r.size)+')'+(r.size>1.5*1048576?'\nHinweis: große Datei - Toleranz erhöhen oder BBox verkleinern.':''));
  kEl('kdl').style.display=kEl('kup').style.display='inline-block';
  kEl('kdl').onclick=()=>{const a=document.createElement('a');a.href=URL.createObjectURL(KOUT.blob);a.download=KOUT.name;a.click();};
  kEl('kup').onclick=kUpload;kPreview(r.buf);
 }catch(e){kLog('Fehler: '+e.message);}
}
function kUpload(){
 if(!KOUT)return;const fd=new FormData();fd.append('file',KOUT.blob,KOUT.name);
 const p=kEl('kprog');p.style.display='block';p.value=0;
 const x=new XMLHttpRequest();x.open('POST','/upload');
 x.upload.onprogress=e=>{if(e.lengthComputable)p.value=e.loaded/e.total*100;};
 x.onload=()=>{kLog(KOUT.name+': '+x.responseText);if(typeof load==='function')load();};
 x.onerror=()=>kLog('Verbindungsfehler - Seite muss vom M5Dial (AP-Modus) geladen sein.');
 x.send(fd);
}
function kPreview(buf){
 const c=kEl('kprev'),g=c.getContext('2d'),m=vmRead(buf);c.style.display='block';
 g.fillStyle='#080c14';g.fillRect(0,0,c.width,c.height);
 const[la0,lo0,la1,lo1]=m.bb,mid=(la0+la1)/2,k=Math.cos(mid/1e5*Math.PI/180);
 const w=(lo1-lo0)*k||1,h=(la1-la0)||1,s=Math.min((c.width-10)/w,(c.height-10)/h);
 const X=p=>5+(p[1]-lo0)*k*s,Y=p=>c.height-5-(p[0]-la0)*s;
 for(const[t,pts]of m.recs){
  g.strokeStyle=g.fillStyle=VM_COL[t]||'#fff';
  if(t>=10&&t<=14){const r=t>=13?2.5:1.5;g.beginPath();g.arc(X(pts[0]),Y(pts[0]),r,0,6.3);g.fill();continue;}
  g.globalAlpha=t>=20?0.8:1;g.beginPath();pts.forEach((p,i)=>i?g.lineTo(X(p),Y(p)):g.moveTo(X(p),Y(p)));g.stroke();g.globalAlpha=1;
 }
}
document.addEventListener('DOMContentLoaded',()=>{const f=kEl('kfiles');if(f)f.onchange=()=>{kAdd([...f.files]);f.value='';};
 kEl('kname').oninput=()=>{KNAMEUSER=true;};});

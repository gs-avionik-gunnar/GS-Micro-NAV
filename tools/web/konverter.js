/* ===== vmap-Konverter (laeuft komplett im Browser; Format siehe README.md) ===== */
const VM_ORDER=[2,7,1,3,6,5,4,27,26,21,22,20,24,23,25,0,12,11,10,14,13];
const VM_COL={0:'#ebebeb',1:'#5f6978',2:'#3e4858',3:'#2d73e1',4:'#f08c1e',5:'#aa913c',6:'#6e6e6e',7:'#4682b4',10:'#ffffff',11:'#e6e6c8',12:'#bebea0',13:'#e65ae6',14:'#e65ae6',20:'#3c8cff',21:'#46aac8',22:'#50be6e',23:'#eb4646',24:'#fac83c',25:'#ff2828',26:'#789678',27:'#a078ff'};
const VM_TYPES=[[0,'0 Landesgrenze'],[1,'1 Regionen/Bundesländer'],[2,'2 Nachbarländer'],[3,'3 Flüsse/Gewässer'],[4,'4 Autobahn'],[5,'5 Hauptstraße'],[6,'6 Bahn'],[7,'7 Küste'],[10,'10 Großstadt'],[11,'11 Stadt'],[12,'12 Ort'],[13,'13 Verkehrsflughafen'],[14,'14 Flugplatz'],[20,'20 CTR'],[21,'21 TMA/CTA'],[22,'22 RMZ/TMZ'],[23,'23 Beschränkung R'],[24,'24 Gefahrengebiet Q/D'],[25,'25 Verbotszone P'],[26,'26 TRA/Segelflug'],[27,'27 Luftraum E'],['oa','OpenAir (Lufträume)']];
const VM_EXO={"Munich":"Muenchen","Cologne":"Koeln","Nuremberg":"Nuernberg","Hanover":"Hannover","Brunswick":"Braunschweig","Prague":"Prag","Vienna":"Wien","Copenhagen":"Kopenhagen","Warsaw":"Warschau","Zurich":"Zuerich","Geneva":"Genf","Luxembourg":"Luxemburg","Brussels":"Bruessel","Strasbourg":"Strassburg","Prague ":"Prag"};
const VM_REPL=[["ä","ae"],["ö","oe"],["ü","ue"],["Ä","Ae"],["Ö","Oe"],["Ü","Ue"],["ß","ss"],["ø","oe"],["Ø","Oe"],["å","aa"],["Å","Aa"],["æ","ae"],["ł","l"],["Ł","L"],["đ","d"]];

/* Python-round (Banker's rounding) */
function vmRnd(x){const r=Math.round(x);return(Math.abs(x%1)===0.5&&r%2!==0)?r-1:r;}
function vmToday(){const d=new Date();return d.getFullYear()*10000+(d.getMonth()+1)*100+d.getDate();}
function vmFmtDate(d){return d?String(d%100).padStart(2,'0')+'.'+String(Math.floor(d/100)%100).padStart(2,'0')+'.'+Math.floor(d/10000):'unbekannt';}
function asciiDe(s){
 if(VM_EXO[s])s=VM_EXO[s];
 for(const[a,b]of VM_REPL)s=s.split(a).join(b);
 s=s.normalize('NFKD').replace(/[^\x00-\x7f]/g,'');
 return s.trim().slice(0,31);
}
/* ---- Geometrie ---- */
function vmLines(g){const t=g.type,c=g.coordinates;
 if(t==='LineString')return[c];if(t==='MultiLineString')return c;if(t==='Polygon')return c;
 if(t==='MultiPolygon'){const o=[];for(const p of c)for(const r of p)o.push(r);return o;}return[];}
function vmPoints(g){const t=g.type,c=g.coordinates;if(t==='Point')return[c];if(t==='MultiPoint')return c;return[];}
function clipRuns(pts,bb){
 if(!bb)return[pts];
 const[x0,y0,x1,y1]=bb,n=pts.length;
 const ins=pts.map(p=>x0<=p[0]&&p[0]<=x1&&y0<=p[1]&&p[1]<=y1);
 const runs=[];let cur=[];
 for(let i=0;i<n;i++){
  const keep=ins[i]||(i>0&&ins[i-1])||(i<n-1&&ins[i+1]);
  if(keep)cur.push(pts[i]);else if(cur.length){runs.push(cur);cur=[];}
 }
 if(cur.length)runs.push(cur);
 return runs.filter(r=>r.length>=2);
}
function rdp(pts,tol){
 const n=pts.length;if(n<3)return pts.map((_,i)=>i);
 const keep=new Array(n).fill(false);keep[0]=keep[n-1]=true;
 const st=[[0,n-1]];
 while(st.length){
  const[a,b]=st.pop();const ax=pts[a][0],ay=pts[a][1];
  const dx=pts[b][0]-ax,dy=pts[b][1]-ay,l2=dx*dx+dy*dy;
  let dmax=-1,idx=-1;
  for(let i=a+1;i<b;i++){
   const px=pts[i][0],py=pts[i][1];let d;
   if(l2===0)d=Math.hypot(px-ax,py-ay);
   else{let t=((px-ax)*dx+(py-ay)*dy)/l2;t=t<0?0:t>1?1:t;d=Math.hypot(px-(ax+t*dx),py-(ay+t*dy));}
   if(d>dmax){dmax=d;idx=i;}
  }
  if(dmax>tol){keep[idx]=true;st.push([a,idx]);st.push([idx,b]);}
 }
 const o=[];keep.forEach((k,i)=>{if(k)o.push(i);});return o;
}
function simplify(pts,tol,latRef){
 if(tol<=0||pts.length<3)return pts;
 const cx=Math.cos(latRef*Math.PI/180)*111320.0;
 return rdp(pts.map(p=>[p[0]*cx,p[1]*110574.0]),tol).map(i=>pts[i]);
}
function ringSimplify(pts,tol,latRef){
 const f=pts[0],l=pts[pts.length-1];
 if(f[0]===l[0]&&f[1]===l[1])pts=pts.slice(0,-1);
 if(tol<=0||pts.length<6)return pts.concat([pts[0]]);
 const cx=Math.cos(latRef*Math.PI/180)*111320.0,x0=pts[0][0],y0=pts[0][1];
 let far=0,best=-1;
 for(let i=0;i<pts.length;i++){const d=((pts[i][0]-x0)*cx)**2+((pts[i][1]-y0)*110574.0)**2;if(d>best){best=d;far=i;}}
 const a=simplify(pts.slice(0,far+1),tol,latRef);
 const b=simplify(pts.slice(far).concat([pts[0]]),tol,latRef);
 return a.slice(0,-1).concat(b);
}
/* ---- OpenAir ---- */
function oaCoord(tok,h){const f=tok.split(':').map(Number);
 const v=f[0]+(f.length>1?f[1]/60:0)+(f.length>2?f[2]/3600:0);
 return('SW'.includes(h.toUpperCase())?-v:v);}
function oaLatLon(txt){const t=txt.replace(/,/g,' ').trim().split(/\s+/);return[oaCoord(t[2],t[3]),oaCoord(t[0],t[1])];}
function oaLimit(txt){
 const u=txt.trim().toUpperCase().replace(/ /g,'');let m;
 if(['GND','SFC','GROUND','SURFACE'].includes(u))return'GND';
 if(['UNL','UNLTD','UNLIMITED'].includes(u))return'UNL';
 if((m=/^FL(\d+)$/.exec(u)))return'FL'+m[1];
 if((m=/^(\d+)(?:FT)?(MSL|AMSL)$/.exec(u)))return String(parseInt(m[1],10));
 if((m=/^(\d+)(?:FT)?(AGL|GND|AGND)$/.exec(u))){const n=parseInt(m[1],10);return n===0?'GND':n+'AGL';}
 return u.replace(/[^A-Z0-9]/g,'').slice(0,8)||'?';
}
function oaType(ac,ay){ay=(ay||'').toUpperCase();ac=ac.toUpperCase();
 if(ay==='CTR')return 20;if(ay==='RMZ'||ay==='TMZ'||ac==='TMZ')return 22;
 const m={R:23,Q:24,P:25,W:26,G:26,E:27};return m[ac]!==undefined?m[ac]:21;}
function loadOpenAir(text,skip){
 const out=[];let cur=null,center=null,warn=0;
 const flush=()=>{
  if(cur&&cur.pts.length>=3&&!skip.has(cur.ac)){
   let lab=(cur.an||'').replace(/\b\d{3}\.\d{2,3}\b/g,'');
   lab=asciiDe(lab.replace(/\|/g,' ').split(/\s+/).filter(Boolean).join(' ')).slice(0,24).trim();
   out.push([oaType(cur.ac,cur.ay),(lab||'?')+'|'+oaLimit(cur.al||'GND')+'|'+oaLimit(cur.ah||'UNL'),cur.pts]);
  }
  cur=null;
 };
 for(const raw of text.split(/\r?\n/)){
  const l=raw.trim();if(!l||l[0]==='*')continue;
  const sp=l.indexOf(' ');const k=(sp<0?l:l.slice(0,sp)).toUpperCase(),v=sp<0?'':l.slice(sp+1);
  if(k==='AC'){flush();cur={ac:v.trim(),pts:[]};center=null;}
  else if(!cur)continue;
  else if(k==='AN'||k==='AY'||k==='AH'||k==='AL')cur[k.toLowerCase()]=v.trim();
  else if(k==='DP')cur.pts.push(oaLatLon(v));
  else if(k==='V'&&v.trim().toUpperCase().startsWith('X='))center=oaLatLon(v.split('=').slice(1).join('='));
  else if(k==='DC'&&center){const r=parseFloat(v);
   for(let a=0;a<360;a+=5){const g=a*Math.PI/180;
    cur.pts.push([center[0]+r*1852*Math.sin(g)/(111320.0*Math.cos(center[1]*Math.PI/180)),center[1]+r*1852*Math.cos(g)/110574.0]);}}
  else if(k==='DA'||k==='DB')warn++;
 }
 flush();
 return{items:out,warn};
}
/* ---- Filter ---- */
function parseFilter(s){
 for(const op of['>=','<=','!=','=','>','<']){const i=s.indexOf(op);if(i>0)return[s.slice(0,i),op,s.slice(i+op.length)];}
 throw new Error('Ungültiger Filter: '+s);
}
function matchProps(pr,flt){
 for(const[key,op,val]of flt){
  const v=pr[key];
  if(op==='='||op==='!='){const hit=val.split('|').includes(String(v));if(hit!==(op==='='))return false;}
  else{const x=(v===null||v===undefined||v==='')?NaN:Number(v),y=Number(val);
   if(isNaN(x)||isNaN(y))return false;
   const r={'>=':x>=y,'<=':x<=y,'>':x>y,'<':x<y}[op];if(!r)return false;}
 }
 return true;
}
/* ---- Quellen laden ---- */
function parseCSV(t){
 if(t.charCodeAt(0)===0xFEFF)t=t.slice(1);
 const rows=[];let r=[],f='',q=false;
 for(let i=0;i<t.length;i++){const c=t[i];
  if(q){if(c==='"'){if(t[i+1]==='"'){f+='"';i++;}else q=false;}else f+=c;}
  else if(c==='"')q=true;else if(c===','){r.push(f);f='';}
  else if(c==='\n'){r.push(f);rows.push(r);r=[];f='';}
  else if(c!=='\r')f+=c;}
 if(f!==''||r.length){r.push(f);rows.push(r);}
 return rows;
}
function csvFeatures(text){
 const rows=parseCSV(text),h=rows[0]||[],ia=h.indexOf('latitude_deg'),io=h.indexOf('longitude_deg');
 if(ia<0||io<0)throw new Error('CSV ohne Spalten latitude_deg/longitude_deg (OurAirports airports.csv erwartet)');
 const feats=[];
 for(let i=1;i<rows.length;i++){const r=rows[i],lat=parseFloat(r[ia]),lon=parseFloat(r[io]);
  if(isNaN(lat)||isNaN(lon))continue;
  const pr={};for(let k=0;k<h.length;k++)pr[h[k]]=r[k];
  feats.push({properties:pr,geometry:{type:'Point',coordinates:[lon,lat]}});}
 return feats;
}
function geoFeatures(text){
 const j=JSON.parse(text.charCodeAt(0)===0xFEFF?text.slice(1):text);
 if(j.features)return j.features;if(j.geometry)return[j];if(j.type&&j.coordinates)return[{properties:{},geometry:j}];
 throw new Error('Kein GeoJSON');
}
/* Quelle aus Text erzeugen: {name,kind:'geo'|'csv'|'oa',feats|text} */
function vmSource(name,text){
 const n=name.toLowerCase();
 if(n.endsWith('.csv'))return{name,kind:'csv',feats:csvFeatures(text)};
 if(n.endsWith('.json')||n.endsWith('.geojson'))return{name,kind:'geo',feats:geoFeatures(text)};
 if(/^AC\s/m.test(text))return{name,kind:'oa',text};
 return null;
}
/* Voreingestellte Ebenen je nach Quelldatei (Natural Earth, OurAirports, OpenAir) */
function vmPresets(src){
 const b=src.name.toLowerCase().split('/').pop();
 if(src.kind==='oa')return[{typ:'oa',flt:'',tol:150}];
 if(src.kind==='csv'){const nf='ident';
  return[{typ:13,flt:'iso_country=DE:type=large_airport|medium_airport',nameField:nf},{typ:14,flt:'iso_country=DE:type=small_airport',nameField:nf}];}
 if(b.includes('admin_0_countries'))return[{typ:2,flt:'ADM0_A3!=DEU',tol:1500},{typ:0,flt:'ADM0_A3=DEU',tol:600}];
 if(b.includes('coastline'))return[{typ:7,tol:1500}];
 if(b.includes('admin_1_states'))return[{typ:1,flt:'ADM0_A3=DEU',tol:800}];
 if(b.includes('rivers_lake'))return[{typ:3,tol:1000}];
 if(b.includes('_roads'))return[{typ:5,flt:'type=Secondary Highway',tol:2000},{typ:4,flt:'type=Major Highway',tol:1200}];
 if(b.includes('populated_places'))return[{typ:12,flt:'adm0name=Germany:pop_max>=60000:pop_max<150000'},{typ:11,flt:'adm0name=Germany:pop_max>=150000:pop_max<500000'},{typ:11,flt:'adm0name!=Germany:featurecla=Admin-0 capital'},{typ:10,flt:'adm0name=Germany:pop_max>=500000'}];
 const g=src.feats.find(f=>f.geometry);
 return[{typ:g&&/Point/.test(g.geometry.type)?12:5}];
}
/* ---- Konvertierung ---- */
const tick=()=>new Promise(r=>setTimeout(r,0));
async function vmConvert(layers,o){
 const bb=o.bbox||null,latRef=bb?(bb[1]+bb[3])/2:51.0,log=[],recs=[];
 const pt=(x,y)=>[vmRnd(y*1e5),vmRnd(x*1e5)];
 for(const L of layers){
  await tick();
  const tol=(L.tol===''||L.tol===undefined||L.tol===null||isNaN(Number(L.tol)))?o.tol:Number(L.tol);
  const nm0=L.src.name.split('/').pop();
  if(L.typ==='oa'){
   const skip=new Set((L.skip||'').split('|').map(s=>s.trim()).filter(Boolean));
   const oa=loadOpenAir(L.src.text,skip);
   let nIn=0,nOut=0,nRec=0;
   for(const[t,nm,pts]of oa.items){
    const xs=pts.map(p=>p[0]),ys=pts.map(p=>p[1]);
    if(bb&&(Math.max(...xs)<bb[0]||Math.min(...xs)>bb[2]||Math.max(...ys)<bb[1]||Math.min(...ys)>bb[3]))continue;
    nIn+=pts.length;const q=[];
    for(const[x,y]of ringSimplify(pts,tol,latRef)){const p=pt(x,y);const l=q[q.length-1];if(!l||l[0]!==p[0]||l[1]!==p[1])q.push(p);}
    if(q[0][0]!==q[q.length-1][0]||q[0][1]!==q[q.length-1][1])q.push(q[0]);
    if(q.length<4)continue;
    recs.push([t,nm,q]);nOut+=q.length;nRec++;
   }
   log.push('OpenAir '+nm0+': '+nRec+' Lufträume, Punkte '+nIn+' -> '+nOut);
   if(oa.warn)log.push('  Warnung: '+oa.warn+' Bogen-Befehle (DA/DB) werden nicht unterstützt');
   continue;
  }
  const flt=(L.flt||'').split(':').map(s=>s.trim()).filter(Boolean).map(parseFilter);
  const typ=Number(L.typ);let nf=0,nIn=0,nOut=0,nRec=0;
  for(const ft of L.src.feats){
   const g=ft.geometry;if(!g||!matchProps(ft.properties||{},flt))continue;nf++;
   if(typ>=10){
    for(const[x,y]of vmPoints(g)){
     if(bb&&!(bb[0]<=x&&x<=bb[2]&&bb[1]<=y&&y<=bb[3]))continue;
     const v=(ft.properties||{})[L.nameField||'name'];
     recs.push([typ,asciiDe(String(v||'')),[pt(x,y)]]);nRec++;
    }
    continue;
   }
   for(const line0 of vmLines(g)){
    const line=line0.map(p=>[p[0],p[1]]);nIn+=line.length;
    for(let run of clipRuns(line,bb)){
     run=simplify(run,tol,latRef);const q=[];
     for(const[x,y]of run){const p=pt(x,y);const l=q[q.length-1];if(!l||l[0]!==p[0]||l[1]!==p[1])q.push(p);}
     for(let k=0;k<q.length;k+=60000){
      const ch=q.slice(Math.max(0,k-1),k+60000);
      if(ch.length>=2){recs.push([typ,'',ch]);nOut+=ch.length;nRec++;}
     }
    }
   }
  }
  log.push('Typ '+typ+' '+nm0+': '+nf+' Features -> '+nRec+' Records'+(typ<10?', Punkte '+nIn+' -> '+nOut:''));
 }
 const ord={};VM_ORDER.forEach((t,i)=>ord[t]=i);
 recs.sort((a,b)=>(ord[a[0]]!==undefined?ord[a[0]]:99)-(ord[b[0]]!==undefined?ord[b[0]]:99));
 if(!recs.length)throw new Error('Keine Daten - Filter/BBox prüfen.');
 let la0=1e9,lo0=1e9,la1=-1e9,lo1=-1e9,np=0,size=32;
 const enc=new TextEncoder(),names=recs.map(r=>enc.encode(r[1]));
 recs.forEach((r,i)=>{size+=20+names[i].length+8*r[2].length;np+=r[2].length;
  for(const p of r[2]){if(p[0]<la0)la0=p[0];if(p[0]>la1)la1=p[0];if(p[1]<lo0)lo0=p[1];if(p[1]>lo1)lo1=p[1];}});
 const buf=new ArrayBuffer(size),dv=new DataView(buf),u8=new Uint8Array(buf);
 u8.set([0x56,0x4d,0x41,0x50,1,0,0,0]);
 dv.setUint32(8,recs.length,true);dv.setInt32(12,la0,true);dv.setInt32(16,lo0,true);dv.setInt32(20,la1,true);dv.setInt32(24,lo1,true);
 const date=o.date||vmToday();dv.setUint32(28,date,true);
 let off=32;
 recs.forEach((r,i)=>{
  let a=1e9,b=1e9,c=-1e9,d=-1e9;
  for(const p of r[2]){if(p[0]<a)a=p[0];if(p[0]>c)c=p[0];if(p[1]<b)b=p[1];if(p[1]>d)d=p[1];}
  dv.setUint8(off,r[0]);dv.setUint8(off+1,names[i].length);dv.setUint16(off+2,r[2].length,true);
  dv.setInt32(off+4,a,true);dv.setInt32(off+8,b,true);dv.setInt32(off+12,c,true);dv.setInt32(off+16,d,true);
  off+=20;u8.set(names[i],off);off+=names[i].length;
  for(const p of r[2]){dv.setInt32(off,p[0],true);dv.setInt32(off+4,p[1],true);off+=8;}
 });
 log.push('=> '+recs.length+' Records, '+np+' Punkte, '+(size/1024).toFixed(1)+' KB, Stand '+vmFmtDate(date));
 return{buf,log,recs:recs.length,pts:np,size,date};
}
/* ---- .vmap lesen (Vorschau) ---- */
function vmRead(buf){
 const dv=new DataView(buf),n=dv.getUint32(8,true);let off=32;const recs=[];
 for(let i=0;i<n;i++){const t=dv.getUint8(off),nl=dv.getUint8(off+1),np=dv.getUint16(off+2,true);off+=20+nl;
  const pts=[];for(let k=0;k<np;k++){pts.push([dv.getInt32(off,true),dv.getInt32(off+4,true)]);off+=8;}recs.push([t,pts]);}
 return{bb:[dv.getInt32(12,true),dv.getInt32(16,true),dv.getInt32(20,true),dv.getInt32(24,true)],date:dv.getUint32(28,true),recs};
}
/* ---- ZIP lesen (Deflate ueber DecompressionStream) ---- */
async function zipEntries(buf){
 const u8=new Uint8Array(buf),dv=new DataView(buf);let e=u8.length-22;
 while(e>=0&&dv.getUint32(e,true)!==0x06054b50)e--;
 if(e<0)throw new Error('Keine gültige ZIP-Datei');
 const n=dv.getUint16(e+10,true);let p=dv.getUint32(e+16,true);const out=[],dec=new TextDecoder();
 for(let i=0;i<n;i++){
  if(dv.getUint32(p,true)!==0x02014b50)break;
  const method=dv.getUint16(p+10,true),cs=dv.getUint32(p+20,true),nl=dv.getUint16(p+28,true),el=dv.getUint16(p+30,true),cl=dv.getUint16(p+32,true),lh=dv.getUint32(p+42,true);
  const name=dec.decode(u8.subarray(p+46,p+46+nl));p+=46+nl+el+cl;
  if(name.endsWith('/'))continue;
  out.push({name,text:async()=>{
   const st=lh+30+dv.getUint16(lh+26,true)+dv.getUint16(lh+28,true),data=u8.subarray(st,st+cs);
   if(method===0)return dec.decode(data);
   if(method!==8)throw new Error('ZIP-Methode '+method+' nicht unterstützt');
   if(typeof DecompressionStream==='undefined')throw new Error('Browser kann ZIP nicht entpacken - bitte Datei einzeln wählen');
   const s=new Blob([data]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
   return dec.decode(await new Response(s).arrayBuffer());}});
 }
 return out;
}
if(typeof module!=='undefined')module.exports={vmFmtDate,vmConvert,vmSource,vmPresets,vmRead,zipEntries,parseCSV,asciiDe,loadOpenAir};

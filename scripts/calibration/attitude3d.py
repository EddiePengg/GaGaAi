#!/usr/bin/env python3
"""gaga 3D 姿态台：实时复现设备的姿态/动势，摇动判据可视化（2026-09-26）。

用法：python3 scripts/calibration/attitude3d.py → 浏览器开 http://127.0.0.1:8767
数据源 = 串口 'A' 流（Wake.cpp 每 10ms 一行：t,模长,偏离,ax,ay,az,gx,gy,gz）。
⚠️ 陀螺仪只在亮屏（Active 档）才有值——息屏后只有加速度（页面会提醒去点亮屏）。

页面四块：
  ① 3D 姿态：Mahony 六轴融合（陀螺积分为主 + 重力校正），圆盘模型实时转，
     屏幕法线拖尾 = 动势轨迹，黄箭头 = 去重力后的动态加速度，蓝箭头 = 重力；
  ② 数值：三轴加速度/陀螺仪/水平分量 hDev/角速率/倾角；
  ③ v5 摇动判据镜像：固件 Wake.cpp 的检测逻辑原样在浏览器跑——
     实时显示摆数/峰值/衰减比/待确认状态，卡在哪道闸一眼可见；
  ④ 滚动曲线：hDev（0.6g 判据线）+ |gyro|，成摆▲/触发🔥打标。

与 orient_calib.py（8766，转向定标）互不影响；本页不落盘、不做阶段采集。
"""
import glob
import json
import sys
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import serial

state_lock = threading.Condition()
samples: deque = deque(maxlen=4000)   # (seq, json_str)，SSE 只发新增
seq_counter = 0
serial_err = ""
ser = None


def find_port():
    ports = glob.glob("/dev/cu.usb*")
    return ports[0] if ports else None


def serial_thread():
    """读 [A] 流。5s 没数据补发一次 'A'（开关型命令：只在确认没流时才发，
    避免把已开的流关掉——orient_calib 同款防御）。"""
    global serial_err
    last_try = 0.0
    got_any = False
    while True:
        try:
            line = ser.readline()
        except Exception as e:
            serial_err = f"串口读取失败: {e}"
            return
        if not line:
            if not got_any and time.time() - last_try > 5:
                try:
                    ser.write(b"A")
                except Exception as e:
                    serial_err = f"串口写入失败: {e}"
                    return
                last_try = time.time()
            continue
        text = line.decode("utf-8", "replace").strip()
        if "[A]," not in text:
            continue
        try:
            f = text.split("[A],")[1].split(",")
            # f = [t, mag, dev, ax, ay, az, gx, gy, gz]（字段序同 orient_calib，
            # 2026-09-25 有过 f[4..6] 错位前科，别再错）
            vals = [float(x) for x in f[:9]]
        except (IndexError, ValueError):
            continue
        got_any = True
        with state_lock:
            global seq_counter
            seq_counter += 1
            samples.append((seq_counter, json.dumps(vals)))
            state_lock.notify_all()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/":
            self._send(200, "text/html; charset=utf-8", HTML.encode())
        elif self.path == "/api/stream":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            last_seq = 0
            # 先把最新一帧发过去（浏览器好立刻初始化姿态）
            with state_lock:
                if samples:
                    last_seq, j = samples[-1]
                    try:
                        self.wfile.write(f"data: {j}\n\n".encode())
                        self.wfile.flush()
                    except Exception:
                        return
            while True:
                with state_lock:
                    while len(samples) == 0 or samples[-1][0] <= last_seq:
                        state_lock.wait(timeout=5)
                        if len(samples) == 0 or samples[-1][0] <= last_seq:
                            try:  # 心跳：探测断连 + 防代理超时
                                self.wfile.write(b": hb\n\n")
                                self.wfile.flush()
                            except Exception:
                                return
                    fresh = [(s, j) for s, j in samples if s > last_seq]
                    last_seq = fresh[-1][0]
                try:
                    for _, j in fresh:
                        self.wfile.write(f"data: {j}\n\n".encode())
                    self.wfile.flush()
                except Exception:
                    return
        else:
            self._send(404, "text/plain", b"404")


HTML = r"""<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>gaga 3D 姿态台</title>
<script type="importmap">
{"imports":{"three":"https://unpkg.com/three@0.160.0/build/three.module.js"}}
</script>
<style>
body{background:#0a0a0f;color:#eee;font-family:-apple-system,sans-serif;margin:0}
header{display:flex;align-items:center;gap:14px;padding:10px 14px;flex-wrap:wrap}
h2{color:#FFD60A;margin:0;font-size:18px}
button{font-size:14px;padding:6px 14px;border-radius:8px;border:0;
background:#FFD60A;font-weight:700;cursor:pointer}
#warn{color:#ff7a7a;font-size:14px;min-height:18px;padding:0 14px}
main{display:flex;gap:10px;padding:0 10px;align-items:stretch;flex-wrap:wrap}
#stage{flex:1 1 560px;min-width:320px;height:64vh;border-radius:12px;
background:#101018;position:relative}
#stage canvas{width:100%;height:100%;display:block;border-radius:12px}
aside{flex:0 0 300px;display:flex;flex-direction:column;gap:8px}
.panel{background:#15151f;border-radius:10px;padding:10px}
.grid{display:grid;grid-template-columns:1fr 1fr 3fr;gap:6px}
.cell{background:#0e0e16;border-radius:8px;padding:6px;text-align:center}
.cell .v{font-size:19px;font-weight:700;font-variant-numeric:tabular-nums}
.cell .k{color:#888;font-size:11px}
#det{font-size:13px;line-height:1.7}
#det b{color:#FFD60A}
#verdict{font-size:15px;font-weight:700;color:#7ad07a;min-height:20px}
#fire{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;
font-size:64px;font-weight:900;color:#FFD60A;opacity:0;pointer-events:none;
text-shadow:0 0 30px #FFD60A}
#charts{display:flex;gap:10px;padding:8px 10px;flex-wrap:wrap}
.chartbox{flex:1 1 420px;background:#101018;border-radius:12px;padding:6px}
.chartbox .k{color:#888;font-size:12px;padding:2px 8px}
canvas.chart{width:100%;height:120px;display:block}
#stat{color:#666;font-size:12px;padding:2px 14px 10px}
</style></head><body>
<header><h2>🦆 gaga 3D 姿态台</h2>
<button id=flip>镜像姿态</button><button id=reset>判据清零</button>
<span id=rate style="color:#888;font-size:13px"></span></header>
<div id=warn></div>
<main><div id=stage>
<div id=fire>🔥 触发!</div></div>
<aside>
<div class="panel"><div class="grid">
<div class="cell"><div class=v id=vax>-</div><div class=k>ax g</div></div>
<div class="cell"><div class=v id=vay>-</div><div class=k>ay g</div></div>
<div class="cell"><div class=v id=vaz>-</div><div class=k>az g</div></div>
<div class="cell"><div class=v id=vgx>-</div><div class=k>gx °/s</div></div>
<div class="cell"><div class=v id=vgy>-</div><div class=k>gy °/s</div></div>
<div class="cell"><div class=v id=vgz>-</div><div class=k>gz °/s</div></div>
<div class="cell"><div class=v id=vhdev>-</div><div class=k>水平分量 g</div></div>
<div class="cell"><div class=v id=vrot>-</div><div class=k>角速率 °/s</div></div>
<div class="cell"><div class=v id=vtilt>-</div><div class=k>倾角 °</div></div>
</div></div>
<div class="panel" id="det">
<div id=verdict>等待数据…（亮屏才有陀螺仪）</div>
摆数 <b id=dsw>0</b>/4｜本摆样本 <b id=dst>0</b><br>
窗口峰值 <b id=dpeak>0.00</b>g（判据 ≥2.5）<br>
本摆/首摆 <b id=dratio>--</b>（判据 ≥0.45）<br>
状态：<b id=dstate>静</b>｜触发累计 <b id=dfire>0</b>
</div>
<div class="panel" style="color:#888;font-size:12px;line-height:1.6">
拖动旋转视角，滚轮缩放。<b style=color:#FFD60A>黄箭头</b>=动态加速度（去重力），
<b style=color:#FFD60A>拖尾</b>=屏幕朝向轨迹（动势），
蓝箭头=重力。判据=固件 v5 原样镜像（0.6g 水平分量/4 摆/峰值 2.5g/衰减 0.45）。
</div>
</aside></main>
<div id=charts>
<div class="chartbox"><div class=k>水平分量 hDev（黄）与 0.6g 判据线（红）；▲=成摆，🔥=触发</div>
<canvas class=chart id=ch1></canvas></div>
<div class="chartbox"><div class=k>角速率 |gyro| °/s（绿）</div>
<canvas class=chart id=ch2></canvas></div>
</div>
<div id=stat></div>
<script type="module">
import * as THREE from 'three';

const $=id=>document.getElementById(id);

// ---------- 场景 ----------
const stage=$('stage');
const renderer=new THREE.WebGLRenderer({antialias:true});
renderer.setPixelRatio(devicePixelRatio);
stage.appendChild(renderer.domElement);
const scene=new THREE.Scene();
const camera=new THREE.PerspectiveCamera(45,1,0.1,100);
let camYaw=0.7,camPitch=0.5,camDist=4.4;
function placeCam(){
  camera.position.set(camDist*Math.cos(camPitch)*Math.sin(camYaw),
                      camDist*Math.sin(camPitch),
                      camDist*Math.cos(camPitch)*Math.cos(camYaw));
  camera.lookAt(0,0,0);
}
placeCam();
let drag=null;
renderer.domElement.addEventListener('pointerdown',e=>{drag={x:e.clientX,y:e.clientY};});
addEventListener('pointermove',e=>{if(!drag)return;
  camYaw-=(e.clientX-drag.x)*0.008;camPitch+=(e.clientY-drag.y)*0.008;
  camPitch=Math.max(-1.4,Math.min(1.4,camPitch));drag={x:e.clientX,y:e.clientY};placeCam();});
addEventListener('pointerup',()=>drag=null);
renderer.domElement.addEventListener('wheel',e=>{e.preventDefault();
  camDist=Math.max(2,Math.min(10,camDist*(1+e.deltaY*0.001)));placeCam();},{passive:false});
scene.add(new THREE.AmbientLight(0xffffff,0.55));
const dl=new THREE.DirectionalLight(0xffffff,1.2);dl.position.set(3,5,4);scene.add(dl);
scene.add(new THREE.GridHelper(6,12,0x333344,0x1c1c2a));
scene.add(new THREE.Mesh(new THREE.SphereGeometry(1.7,24,16),
  new THREE.MeshBasicMaterial({color:0x30304a,wireframe:true,transparent:true,opacity:0.25})));

// 设备模型：扁圆柱机身（轴向对齐 body Z=屏幕法线），顶面贴 gaga 字样
function screenTexture(){
  const c=document.createElement('canvas');c.width=c.height=256;
  const g=c.getContext('2d');g.fillStyle='#FFD60A';g.fillRect(0,0,256,256);
  g.fillStyle='#111';g.font='bold 72px sans-serif';g.textAlign='center';
  g.fillText('gaga',128,150);g.font='48px sans-serif';g.fillText('🦆',128,86);
  g.fillStyle='#e33';g.beginPath();g.arc(128,26,12,0,7);g.fill(); // 内容"上边"记号
  return new THREE.CanvasTexture(c);
}
const device=new THREE.Group();
const body=new THREE.Mesh(new THREE.CylinderGeometry(1,1,0.26,48),
  new THREE.MeshStandardMaterial({color:0x2a2a35,roughness:0.5}));
body.rotation.x=Math.PI/2;device.add(body);
const face=new THREE.Mesh(new THREE.CircleGeometry(0.94,48),
  new THREE.MeshBasicMaterial({map:screenTexture()}));
face.position.z=0.135;device.add(face);
const rim=new THREE.Mesh(new THREE.TorusGeometry(0.97,0.035,10,48),
  new THREE.MeshStandardMaterial({color:0x555566}));
rim.position.z=0.13;device.add(rim);
scene.add(device);
const gArrow=new THREE.ArrowHelper(new THREE.Vector3(0,-1,0),new THREE.Vector3(0,1.75,0),1.2,0x4d9fff,0.18,0.1);
const dArrow=new THREE.ArrowHelper(new THREE.Vector3(1,0,0),new THREE.Vector3(1.75,0,0),1,0xFFD60A,0.16,0.09);
scene.add(gArrow,dArrow);

// 屏幕法线拖尾（动势）：参考球壳上的渐隐点列
const TRAIL_N=56,trail=[];
for(let i=0;i<TRAIL_N;i++){
  const m=new THREE.Mesh(new THREE.SphereGeometry(0.028,8,6),
    new THREE.MeshBasicMaterial({color:0xFFD60A,transparent:true}));
  m.material.opacity=0;m.visible=false;scene.add(m);trail.push(m);
}
let trailHead=0,lastTrailMs=0;

// ---------- Mahony 六轴姿态融合（陀螺积分为主，重力矢量校正漂移） ----------
const q={w:1,x:0,y:0,z:0};
const eInt={x:0,y:0,z:0};
let flipConj=false,inited=false;
function mahony(ax,ay,az,gx,gy,gz,dt){
  const Kp=1.6,Ki=0.02;
  const n=Math.hypot(ax,ay,az);
  let ex=0,ey=0,ez=0;
  if(n>0.2){
    ax/=n;ay/=n;az/=n;
    const hvx=q.x*q.z-q.w*q.y,hvy=q.w*q.x+q.y*q.z,hvz=q.w*q.w-0.5+q.z*q.z;
    ex=ay*hvz-az*hvy;ey=az*hvx-ax*hvz;ez=ax*hvy-ay*hvx;
  }
  eInt.x+=Ki*ex*dt;eInt.y+=Ki*ey*dt;eInt.z+=Ki*ez*dt;
  const p=Math.PI/180;
  const wx=(gx+Kp*ex+eInt.x)*p,wy=(gy+Kp*ey+eInt.y)*p,wz=(gz+Kp*ez+eInt.z)*p;
  const dw=0.5*(-q.x*wx-q.y*wy-q.z*wz),
        dx=0.5*( q.w*wx+q.y*wz-q.z*wy),
        dy=0.5*( q.w*wy-q.x*wz+q.z*wx),
        dz=0.5*( q.w*wz+q.x*wy-q.y*wx);
  q.w+=dw*dt;q.x+=dx*dt;q.y+=dy*dt;q.z+=dz*dt;
  const l=Math.hypot(q.w,q.x,q.y,q.z);q.w/=l;q.x/=l;q.y/=l;q.z/=l;
}

// ---------- v5 摇动判据镜像（Wake.cpp 原样：逐样本同参数） ----------
const det={streak:0,minMag:99,peak:0,swingPeak:0,firstPeak:0,swings:0,
  lastSwingMs:0,pending:false,pendingSince:0,lastFireMs:0,fires:0};
const gLp={x:0,y:0,z:1};   // 重力低通（α=0.06/样本，同固件 10ms tick）
const chartMarks1=[],chartMarksFire=[];
let lastT=null;
function fireFlash(){fireUntil=performance.now()+900;chartMarksFire.push(lastT);}
function detector(t,mag,ax,ay,az){
  const dev=Math.abs(mag-1);
  gLp.x+=0.06*(ax-gLp.x);gLp.y+=0.06*(ay-gLp.y);gLp.z+=0.06*(az-gLp.z);
  const gm=Math.hypot(gLp.x,gLp.y,gLp.z);
  let hDev=0;
  if(gm>0.2){
    const dx=ax-gLp.x,dy=ay-gLp.y,dz=az-gLp.z;
    const dm=Math.hypot(dx,dy,dz);
    const par=(dx*gLp.x+dy*gLp.y+dz*gLp.z)/gm;
    hDev=Math.sqrt(Math.max(0,dm*dm-par*par));
  }
  if(hDev>0.6){det.streak++;det.minMag=Math.min(det.minMag,mag);det.peak=Math.max(det.peak,mag);}
  else{
    if(det.streak>=3&&det.minMag>0.35&&!det.pending){
      if(det.swings>0&&t-det.lastSwingMs>500){det.swings=0;det.firstPeak=0;det.swingPeak=0;}
      det.swings++;
      if(det.swings===1)det.firstPeak=det.peak;
      det.swingPeak=Math.max(det.swingPeak,det.peak);
      det.lastSwingMs=t;chartMarks1.push(t);
    }
    det.streak=0;det.minMag=99;det.peak=0;
  }
  if(det.pending&&dev<0.2&&t-det.lastSwingMs>=500&&t-det.lastFireMs>=2000){
    det.pending=false;det.swings=0;det.firstPeak=0;det.swingPeak=0;
    det.lastFireMs=t;det.fires++;fireFlash();
  }
  if(det.pending&&t-det.pendingSince>2500){
    det.pending=false;det.swings=0;det.firstPeak=0;det.swingPeak=0;}
  return hDev;
}

// ---------- 图表 ----------
const chart1=$('ch1'),chart2=$('ch2');
const hist1=[],hist2=[];
const WIN=10000;
function pushHist(a,t,v){a.push({t,v});while(a.length&&t-a[0].t>WIN)a.shift();}
function drawChart(cv,hist,color,thresh,marks,fires){
  const g=cv.getContext('2d'),W=cv.width=cv.clientWidth*devicePixelRatio,
        H=cv.height=cv.clientHeight*devicePixelRatio;
  g.fillStyle='#101018';g.fillRect(0,0,W,H);
  if(hist.length<2)return;
  const t1=hist[hist.length-1].t,t0=t1-WIN;
  const vmax=Math.max(1,...hist.map(p=>p.v))*1.15;
  const X=t=>(t-t0)/(t1-t0)*W,Y=v=>H-(v/vmax)*(H-8)-4;
  if(thresh!=null){g.strokeStyle='#e05555';g.setLineDash([6,5]);
    g.beginPath();g.moveTo(0,Y(thresh));g.lineTo(W,Y(thresh));g.stroke();g.setLineDash([]);}
  g.strokeStyle=color;g.lineWidth=2*devicePixelRatio;g.beginPath();
  for(const p of hist){const x=X(p.t),y=Y(p.v);p===hist[0]?g.moveTo(x,y):g.lineTo(x,y);}
  g.stroke();
  if(marks){g.fillStyle='#FFD60A';
    for(const t of marks){if(t<t0)continue;const x=X(t);
      g.beginPath();g.moveTo(x,H-4);g.lineTo(x-5,H-14);g.lineTo(x+5,H-14);g.fill();}}
  if(fires){g.fillStyle='#ff7a5a';g.font=`${11*devicePixelRatio}px sans-serif`;
    for(const t of fires){if(t<t0)continue;g.fillText('🔥',X(t)-6,14);}}
}

// ---------- 样本入口（SSE） ----------
let prevSampleT=null,gyroZeroSince=null,rateCnt=0,rateT0=0,fireUntil=0;
const curA={x:0,y:0,z:1};
function onSample(s){
  const[t,mag,dev,ax,ay,az,gx,gy,gz]=s;
  lastT=t;curA.x=ax;curA.y=ay;curA.z=az;
  rateCnt++;
  if(!rateT0)rateT0=performance.now();
  else if(performance.now()-rateT0>1000){
    $('rate').textContent=`${rateCnt} 样本/s`;rateCnt=0;rateT0=performance.now();}
  let sdt=0.01;
  if(prevSampleT!=null)sdt=Math.min(0.05,Math.max(0.002,(t-prevSampleT)/1000));
  prevSampleT=t;
  if(!inited){inited=true;gLp.x=ax;gLp.y=ay;gLp.z=az;}
  mahony(ax,ay,az,gx,gy,gz,sdt);
  const hDev=detector(t,mag,ax,ay,az);
  const rot=Math.hypot(gx,gy,gz);
  pushHist(hist1,t,hDev);pushHist(hist2,t,rot);
  $('vax').textContent=ax.toFixed(2);$('vay').textContent=ay.toFixed(2);$('vaz').textContent=az.toFixed(2);
  $('vgx').textContent=gx.toFixed(0);$('vgy').textContent=gy.toFixed(0);$('vgz').textContent=gz.toFixed(0);
  $('vhdev').textContent=hDev.toFixed(2);$('vrot').textContent=rot.toFixed(0);
  $('vtilt').textContent=(Math.atan2(Math.hypot(ax,ay),Math.abs(az))*180/Math.PI).toFixed(0);
  $('dsw').textContent=det.swings;$('dst').textContent=det.streak;
  $('dpeak').textContent=det.swingPeak.toFixed(2);
  $('dratio').textContent=det.firstPeak>0?(det.peak/det.firstPeak).toFixed(2):'--';
  $('dstate').textContent=det.pending?'待停手确认':(det.streak>0?'摆中':'静');
  $('dfire').textContent=det.fires;
  $('verdict').textContent=
    det.fires&&t-det.lastFireMs<4000?'✅ 这一摇会触发':
    det.swings>0&&det.swings<4?`还差 ${4-det.swings} 摆`:
    det.swingPeak>0&&det.swingPeak<2.5?`峰值不够：${det.swingPeak.toFixed(2)}/2.5g`:
    '力度/摆数未达判据';
  const g0=(gx===0&&gy===0&&gz===0);
  if(g0){if(gyroZeroSince==null)gyroZeroSince=t;
    if(t-gyroZeroSince>1000)$('warn').textContent='⚠️ 陀螺仪无数据（息屏=Idle 档）——点一下嘎嘎亮屏再摇';}
  else{gyroZeroSince=null;$('warn').textContent='';}
  $('stat').textContent=`样本时刻 ${t}ms｜${(sdt*1000).toFixed(1)}ms/样本`;
}
const es=new EventSource('/api/stream');
es.onmessage=e=>onSample(JSON.parse(e.data));

// ---------- 渲染循环 ----------
const tmpQ=new THREE.Quaternion(),tmpV=new THREE.Vector3(),WORLD_UP=new THREE.Vector3(0,1,0);
function animate(){
  requestAnimationFrame(animate);
  const W=stage.clientWidth,H=stage.clientHeight;
  if(W&&H&&(renderer.domElement.width!==Math.round(W*devicePixelRatio))){
    renderer.setSize(W,H,false);camera.aspect=W/H;camera.updateProjectionMatrix();}
  tmpQ.set(q.x,q.y,q.z,q.w);
  if(flipConj)tmpQ.conjugate();
  device.quaternion.copy(tmpQ);
  if(lastT!=null){
    // 动态加速度：body 系测量旋到世界系，减去静态重力（≈+Y）。
    // 箭头放在球壳外侧沿自身方向向外指（原点在原点会被设备模型挡住）
    tmpV.set(curA.x,curA.y,curA.z).applyQuaternion(tmpQ).sub(WORLD_UP);
    if(tmpV.length()>0.05){
      dArrow.visible=true;
      const L=tmpV.length(),dir=tmpV.clone().normalize();
      dArrow.position.copy(dir).multiplyScalar(1.75);
      dArrow.setDirection(dir);
      dArrow.setLength(Math.min(1.2,L*0.9)+0.3,0.16,0.09);
    }else dArrow.visible=false;
    // 拖尾：屏幕法线（body +Z）打到参考球壳
    const now=performance.now();
    if(now-lastTrailMs>28){
      lastTrailMs=now;
      const m=trail[trailHead];
      m.position.set(0,0,1).applyQuaternion(tmpQ).multiplyScalar(1.7);
      m.material.opacity=0.9;m.visible=true;
      trailHead=(trailHead+1)%TRAIL_N;
    }
    for(const m of trail)m.material.opacity*=0.965;
  }
  $('fire').style.opacity=performance.now()<fireUntil?(fireUntil-performance.now())/900:0;
  drawChart(chart1,hist1,'#FFD60A',0.6,chartMarks1,chartMarksFire);
  drawChart(chart2,hist2,'#4de08a',null,null,null);
  for(const a of [chartMarks1,chartMarksFire])while(a.length&&lastT!=null&&lastT-a[0]>WIN)a.shift();
  renderer.render(scene,camera);
}
$('flip').onclick=()=>flipConj=!flipConj;
$('reset').onclick=()=>{Object.assign(det,{streak:0,minMag:99,peak:0,swingPeak:0,
  firstPeak:0,swings:0,pending:false,fires:0});chartMarks1.length=0;chartMarksFire.length=0;};
animate();
</script></body></html>"""


def main():
    global ser
    port = find_port()
    if not port:
        sys.exit("找不到串口（/dev/cu.usb*）——设备插了吗？")
    try:
        ser = serial.Serial(port, 115200, timeout=1)
    except Exception as e:
        sys.exit(f"串口打开失败（被占用？）：{e}")
    time.sleep(1.0)
    threading.Thread(target=serial_thread, daemon=True).start()
    print(f"[attitude3d] 串口 {port}")
    print("[attitude3d] 打开 http://127.0.0.1:8767 （保持嘎嘎亮屏：陀螺仪只在 Active 档输出）")
    ThreadingHTTPServer(("127.0.0.1", 8767), Handler).serve_forever()


if __name__ == "__main__":
    main()

"use client";

import { useEffect, useMemo, useRef, useState } from "react";
import { Canvas, useFrame, useThree } from "@react-three/fiber";
import { OrbitControls } from "@react-three/drei";
import * as THREE from "three";
import { Pause, Play } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Slider } from "@/components/ui/slider";
import { SHAPE, heightAt, type Snapshot } from "@/lib/snapshot";

// Particles stay 16-bit quantised in GPU memory and are decoded in the vertex shader; each is
// drawn as a lit sphere impostor coloured by speed. Solver space is y-up, like three.js.
const vertex = /* glsl */ `
  attribute vec3 qpos;
  attribute float speed;
  uniform vec3 qmin;
  uniform vec3 qext;
  uniform float radius;
  uniform float scale;
  varying float vSpeed;
  void main() {
    vec3 p = qmin + qext * (qpos / 65535.0);
    vec4 mv = modelViewMatrix * vec4(p, 1.0);
    gl_Position = projectionMatrix * mv;
    gl_PointSize = radius * scale / -mv.z;
    vSpeed = speed / 255.0;
  }
`;
const fragment = /* glsl */ `
  varying float vSpeed;
  void main() {
    vec2 c = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot(c, c);
    if (r2 > 1.0) discard;
    vec3 n = vec3(c.x, -c.y, sqrt(1.0 - r2));
    vec3 deep = vec3(0.05, 0.28, 0.42);
    vec3 fast = vec3(0.78, 0.95, 1.0);
    vec3 base = mix(deep, fast, smoothstep(0.0, 1.0, vSpeed));
    float diff = max(dot(n, normalize(vec3(-0.4, 0.7, 0.6))), 0.0);
    float spec = pow(max(dot(n, normalize(vec3(-0.2, 0.4, 1.0))), 0.0), 40.0);
    gl_FragColor = vec4(base * (0.35 + 0.75 * diff) + spec * 0.6, 1.0);
  }
`;

function Particles({ snap, frame }: { snap: Snapshot; frame: number }) {
  const { size, camera } = useThree();
  // three.js objects are mutable GPU handles; refs keep them out of React's immutability rules.
  const geoRef = useRef<THREE.BufferGeometry>(null);
  const matRef = useRef<THREE.ShaderMaterial>(null);
  const mat = useMemo(
    () =>
      new THREE.ShaderMaterial({
        vertexShader: vertex,
        fragmentShader: fragment,
        uniforms: {
          qmin: { value: new THREE.Vector3(...snap.quantMin) },
          qext: { value: new THREE.Vector3(...snap.quantMax.map((v, i) => v - snap.quantMin[i])) },
          radius: { value: snap.particleRadius * 2.2 },
          scale: { value: 1 },
        },
      }),
    [snap],
  );
  useEffect(() => {
    const f = snap.frames[frame];
    const geo = geoRef.current;
    if (!f || !geo) return;
    geo.setAttribute("qpos", new THREE.BufferAttribute(new Float32Array(f.positions), 3));
    geo.setAttribute("speed", new THREE.BufferAttribute(new Float32Array(f.speed), 1));
    geo.setAttribute("position", new THREE.BufferAttribute(new Float32Array(f.count * 3), 3));
    geo.setDrawRange(0, f.count);
    geo.boundingSphere = new THREE.Sphere(new THREE.Vector3(), 1e6);
  }, [snap, frame]);
  useFrame(() => {
    const cam = camera as THREE.PerspectiveCamera;
    if (matRef.current) matRef.current.uniforms.scale.value = size.height / (2 * Math.tan((cam.fov * Math.PI) / 360));
  });
  return (
    <points>
      <bufferGeometry ref={geoRef} />
      <primitive object={mat} ref={matRef} attach="material" />
    </points>
  );
}

function Obstacles({ snap, frame }: { snap: Snapshot; frame: number }) {
  const off = snap.frames[frame]?.obstacleOffsets;
  return (
    <>
      {snap.obstacles.map((o, i) => {
        const d: [number, number, number] = off ? [off[3 * i], off[3 * i + 1], off[3 * i + 2]] : [0, 0, 0];
        const mat = <meshStandardMaterial color="#5f6670" roughness={0.8} />;
        if (o.type === SHAPE.box) {
          const s = o.max.map((v, k) => v - o.min[k]) as [number, number, number];
          const c = o.min.map((v, k) => (v + o.max[k]) / 2 + d[k]) as [number, number, number];
          return <mesh key={i} position={c}><boxGeometry args={s} />{mat}</mesh>;
        }
        if (o.type === SHAPE.sphere) {
          return <mesh key={i} position={o.center.map((v, k) => v + d[k]) as [number, number, number]}><sphereGeometry args={[o.radius, 48, 24]} />{mat}</mesh>;
        }
        if (o.type === SHAPE.cylinder) {
          const rot: [number, number, number] = o.axis === 0 ? [0, 0, Math.PI / 2] : o.axis === 2 ? [Math.PI / 2, 0, 0] : [0, 0, 0];
          return <mesh key={i} rotation={rot} position={o.center.map((v, k) => v + d[k]) as [number, number, number]}><cylinderGeometry args={[o.radius, o.radius, 2 * o.halfLength, 48]} />{mat}</mesh>;
        }
        return <Terrain key={i} o={o} />;
      })}
    </>
  );
}

function Terrain({ o }: { o: Snapshot["obstacles"][number] }) {
  const geo = useMemo(() => {
    const nx = 96, nz = 48;
    const g = new THREE.PlaneGeometry(1, 1, nx, nz);
    const p = g.attributes.position as THREE.BufferAttribute;
    for (let i = 0; i < p.count; i++) {
      const x = o.min[0] + (p.getX(i) + 0.5) * (o.max[0] - o.min[0]);
      const z = o.min[2] + (0.5 - p.getY(i)) * (o.max[2] - o.min[2]);
      p.setXYZ(i, x, Math.min(heightAt(o, x, z), o.max[1]), z);
    }
    g.computeVertexNormals();
    return g;
  }, [o]);
  return <mesh geometry={geo}><meshStandardMaterial color="#4a4238" roughness={0.9} side={THREE.DoubleSide} /></mesh>;
}

function Tank({ snap }: { snap: Snapshot }) {
  const s = snap.domainMax.map((v, i) => v - snap.domainMin[i]) as [number, number, number];
  const c = snap.domainMin.map((v, i) => (v + snap.domainMax[i]) / 2) as [number, number, number];
  const edges = useMemo(() => new THREE.EdgesGeometry(new THREE.BoxGeometry(...s)), [s[0], s[1], s[2]]); // eslint-disable-line react-hooks/exhaustive-deps
  const big = Math.max(...s);
  return (
    <>
      <lineSegments geometry={edges} position={c}><lineBasicMaterial color="#343842" /></lineSegments>
      <gridHelper args={[big * 4, 40, "#23262d", "#171a1f"]} position={[c[0], snap.domainMin[1] - 1e-3, c[2]]} />
    </>
  );
}

export function ParticleViewer({ snap, camera }: { snap: Snapshot; camera?: { target?: number[]; distance?: number; yaw_deg?: number; pitch_deg?: number } }) {
  const [frame, setFrame] = useState(snap.frames.length - 1);
  const [playing, setPlaying] = useState(false);
  const last = snap.frames.length - 1;
  const timer = useRef<number | null>(null);

  useEffect(() => {
    if (!playing) return;
    timer.current = window.setInterval(() => setFrame((f) => (f >= last ? 0 : f + 1)), 50);
    return () => { if (timer.current) window.clearInterval(timer.current); };
  }, [playing, last]);

  const center = snap.domainMin.map((v, i) => (v + snap.domainMax[i]) / 2);
  const target = camera?.target ?? center;
  const dist = camera?.distance ?? Math.max(...snap.domainMax.map((v, i) => v - snap.domainMin[i])) * 1.6;
  const yaw = ((camera?.yaw_deg ?? 38) * Math.PI) / 180;
  const pitch = ((camera?.pitch_deg ?? 20) * Math.PI) / 180;
  const eye: [number, number, number] = [
    target[0] + dist * Math.cos(pitch) * Math.sin(yaw),
    target[1] + dist * Math.sin(pitch),
    target[2] + dist * Math.cos(pitch) * Math.cos(yaw),
  ];

  return (
    <div className="flex flex-col gap-3">
      <div className="relative aspect-video overflow-hidden rounded-lg border border-border bg-background">
        <Canvas camera={{ position: eye, fov: 40, near: dist * 0.01, far: dist * 50 }} dpr={[1, 2]}>
          <color attach="background" args={["#0a0b0d"]} />
          <ambientLight intensity={0.4} />
          <directionalLight position={[eye[0], eye[1] * 3, eye[2]]} intensity={1.2} />
          <Tank snap={snap} />
          <Obstacles snap={snap} frame={frame} />
          <Particles snap={snap} frame={frame} />
          <OrbitControls target={target as [number, number, number]} makeDefault enableDamping />
        </Canvas>
        <div className="pointer-events-none absolute top-3 left-3 font-mono text-2xs text-muted-foreground">
          t = {snap.frames[frame]?.time.toFixed(3)} s · {snap.frames[frame]?.count.toLocaleString()} particles
        </div>
      </div>
      <div className="flex items-center gap-3">
        <Button size="icon" variant="secondary" aria-label={playing ? "Pause" : "Play"} onClick={() => setPlaying((p) => !p)} disabled={last < 1}>
          {playing ? <Pause size={16} strokeWidth={1.5} /> : <Play size={16} strokeWidth={1.5} />}
        </Button>
        <Slider aria-label="Frame" min={0} max={Math.max(last, 0)} step={1} value={[frame]} onValueChange={([v]) => { setPlaying(false); setFrame(v); }} className="flex-1" />
        <span className="w-20 text-right font-mono text-xs text-muted-foreground tabular">{frame + 1}/{last + 1}</span>
      </div>
    </div>
  );
}

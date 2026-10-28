// Response shapes of the AquaSPH API (backend/api/openapi.yaml), as the console uses them.

export type JobState = "queued" | "running" | "completed" | "failed" | "cancelled";
export type Quality = "low" | "medium" | "high";

export type Progress = {
  t: number;
  t_end: number;
  fraction: number;
  step: number;
  fluid: number;
  max_speed?: number;
  wall_s: number;
};

export type Job = {
  id: string;
  state: JobState;
  outcome: "stable" | "unstable" | null;
  scenario: string;
  quality: Quality;
  sim_time?: number;
  max_steps?: number;
  max_particles: number;
  timeout_seconds: number;
  priority: number;
  overrides: Record<string, unknown>;
  labels: Record<string, string>;
  sweep_id?: string;
  sweep_params?: Record<string, unknown>;
  progress: Progress | null;
  attempt: number;
  max_attempts: number;
  cancel_requested: boolean;
  cache_hit: boolean;
  source_job_id?: string;
  spec_hash: string;
  solver_id: string | null;
  error: { code: string; message?: string } | null;
  spec?: ScenarioSpec;
  created_at: string;
  started_at: string | null;
  finished_at: string | null;
};

export type Page<T> = { data: T[]; next_cursor: string | null };

export type ScenarioSummary = {
  name: string;
  tier: number;
  description: string;
  approximation?: string;
};

export type ScenarioSpec = {
  name: string;
  tier?: number;
  description?: string;
  approximation?: string;
  domain?: { min: number[]; max: number[] };
  duration?: { simulated_time?: number; output_interval?: number };
  camera?: { target?: number[]; distance?: number; yaw_deg?: number; pitch_deg?: number; fov_deg?: number };
  materials?: { name: string; viscosity?: number; rest_density?: number; surface_tension?: number }[];
  numerics?: Record<string, number>;
  [key: string]: unknown;
};

export type ScenarioDetail = ScenarioSummary & { spec: ScenarioSpec };

export type JobEvent = {
  at: string;
  from_state: JobState | null;
  to_state: JobState;
  attempt: number;
  worker_id?: string;
  detail?: string;
};

export type Artifact = { name: string; size: number; sha256: string; content_type: string; url: string };

export type Stats = {
  tenant: {
    name: string;
    max_concurrent_jobs: number;
    max_queued_jobs: number;
    max_particles: number;
    max_wall_seconds: number;
    rate_per_minute: number;
    rate_burst: number;
  };
  window_hours: number;
  states: Record<JobState, number>;
  window: {
    submitted: number;
    completed: number;
    failed: number;
    cancelled: number;
    cache_hits: number;
    cache_hit_rate: number | null;
    runtime_p50_s: number | null;
    runtime_p95_s: number | null;
    queue_wait_p95_s: number | null;
    stable_fraction: number | null;
  };
  hourly: { hour: string; completed: number; cache_hits: number; failed: number; cancelled: number }[];
  scenarios: { scenario: string; runs: number; stable: number; runtime_p50_s: number | null }[];
  workers: { live: number; slots: number; busy: number };
};

export type Sweep = {
  id: string;
  state: "running" | "completed";
  scenario: string;
  quality: Quality;
  grid: Record<string, unknown[]>;
  overrides: Record<string, unknown>;
  labels: Record<string, string>;
  total: number;
  counts: Partial<Record<JobState, number>>;
  cache_hits: number;
  created_at: string;
};

export type SweepResults = {
  axes: string[];
  metric_pointers: string[];
  rows: {
    job_id: string;
    state: JobState;
    outcome: "stable" | "unstable" | null;
    cache_hit: boolean;
    error_code?: string;
    params: Record<string, unknown>;
    metrics: Record<string, unknown>;
  }[];
};

export type WebhookEndpoint = {
  id: string;
  url: string;
  description: string;
  event_types: string[];
  created_at: string;
  secret?: string;
};

export type WebhookDelivery = {
  id: number;
  event_id: string;
  event_type: string;
  job_id: string | null;
  state: "pending" | "delivering" | "delivered" | "dead";
  attempt: number;
  next_attempt_at: string | null;
  last_status: number | null;
  last_error: string | null;
  last_attempt_at: string | null;
  delivered_at: string | null;
  created_at: string;
};

// The subset of the solver's metrics.json the console reads.
export type Metrics = {
  scenario: string;
  tier: string;
  status: "STABLE" | "UNSTABLE" | "CANCELLED";
  quality: string;
  threads: number;
  particles: { fluid_final?: number; boundary?: number; fluid_peak?: number; [k: string]: unknown };
  time: { simulated_seconds: number; steps: number; wall_seconds: number; avg_step_ms: number; dt_min: number; dt_mean: number; dt_max: number };
  density: { min: number; max: number; avg_final: number; fraction_within_1pct_final: number; fraction_within_1pct_mean: number };
  dynamics: { max_speed: number; max_acceleration: number; containment_events: number; unstable_particles: number };
  volume: { initial_m3: number; final_m3: number };
  timeline?: {
    t: number[];
    dt: number[];
    max_speed: number[];
    density_avg: number[];
    density_max: number[];
    near_rest: number[];
    fluid: number[];
    volume: number[];
    kinetic_energy: number[];
    potential_energy: number[];
  };
  surge_front?: { t: number; front: number; Z: number; T: number }[];
  probes?: { name: string; series: { t: number[]; elevation: number[] } }[];
  snapshots?: { frames: number; interval: number; bytes: number; truncated: boolean };
};

export type ProblemDetails = {
  type?: string;
  title?: string;
  status: number;
  detail?: string;
  request_id?: string;
  errors?: { field: string; message: string }[];
};

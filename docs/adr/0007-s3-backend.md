# ADR 0007 — S3 backend: Hetzner Object Storage, not in-cluster MinIO

Status: Accepted — 2026-08-14

## Context

`src/storage/Storage.hpp` already ships the seam this decision plugs into:
`StorageBackend` is an interface, `LocalStorage` is the dev default, and
`S3Storage` (hand-rolled SigV4 over libcurl, path-style addressing) is the
production implementation — wired purely through env vars (`STORAGE_BACKEND`,
`S3_ENDPOINT`, `S3_REGION`, `S3_BUCKET`, `S3_ACCESS_KEY`, `S3_SECRET_KEY`). No
code changes at all when the S3 endpoint moves; this ADR only decides **where**
that endpoint lives — spec §16/§19 left it open pending a look at the actual
cluster (`docs/superpowers/specs/2026-08-14-cyber-accountant-design.md`).

**Cluster shape** (`../cluster/main.tf`, `kubectl` against
`../cluster/kubeconfig`, 2026-08-14): Talos on Hetzner Cloud, region **fsn1**
(Falkenstein), **3× cx43** (8 vCPU / 16 GiB) — control-plane-only, no separate
worker pool; `control_plane_allow_schedule = true` so all application pods
share the same three nodes as etcd/the API server. Block storage is Longhorn
on each node's local disk (no separate volume/NAS backing it).

`kubectl top nodes` returned `error: Metrics API not available` — no
metrics-server is deployed, so live CPU/mem % isn't available. Static data
instead, from `kubectl describe nodes` / `get pv,pvc,sc -A`:

- **Requests** per node: CPU 23–27% of 7950m allocatable, memory 11–18% of
  ~15 GiB allocatable. Headroom exists on paper, but the cluster is already
  running a full stack on 3 nodes: `db` (3× Postgres + 3× Redis PVCs),
  `monitoring` (kube-prometheus-stack + Loki + Tempo, 4 more PVCs), Longhorn
  itself, cert-manager, ingress-nginx, external-dns, and a `www` site.
- **Storage class posture**: the default `longhorn` SC replicates
  (multi-replica); nearly every existing PVC (`db/*`, `monitoring/*`) instead
  uses `longhorn-single-replica` (`numberOfReplicas: 1`,
  `dataLocality: strict-local`) — a deliberate trade of durability for
  RAM/disk/network headroom on a 3-node cluster. Adding another
  self-hosted stateful service competes for that same already-rationed
  budget.
- **A MinIO instance already exists in-cluster** (`minio` namespace): a
  single pod (`quay.io/minio/minio:RELEASE.2024-12-18T13-15-44Z`, no
  replicas, so a SPOF), backed by one 20 GiB PVC on the (replicated)
  `longhorn` SC. It predates this project, isn't part of this repo's Helm/IaC,
  and there's no evidence of a backup policy or lifecycle ownership for it —
  reusing it would silently couple `cyber-accountant` tenant data's
  availability and backup story to an unrelated, unmanaged deployment. Not
  considered a viable "MinIO is already free" argument.
- `hcloud` CLI (v1.67.0) is installed and authenticated-capable, but has no
  `object-storage`/`bucket` subcommand — only `storage-box` (Hetzner's
  SFTP/CIFS product, a different offering). Object Storage bucket creation
  and S3 access-key generation are Hetzner Console-only actions; nothing in
  this session's tool access (kubeconfig, hcloud CLI) can provision them
  programmatically.

None of the above "screams MinIO" — the cluster is small, control-plane-only,
already running every other stateful piece with reduced replication to fit,
and the one existing MinIO pod is an unmanaged single point of failure outside
this project's ownership. The data confirms rather than overturns the spec's
default.

## Decision

1. **Production S3 backend is Hetzner Object Storage**, not a MinIO subchart
   in the cluster. Object Storage is managed (no pod/PVC/backup to operate),
   S3-compatible (no code change vs. MinIO — same `S3Storage` class), and
   lives in the same Hetzner region (**fsn1**) as the cluster, so latency and
   egress stay local.
2. **Bucket**: `cyber-accountant-prod`, region `fsn1`. **Secret**: a
   `kubectl create secret generic s3-credentials` in the `cyber-accountant`
   namespace, holding `S3_ENDPOINT`, `S3_BUCKET`, `S3_ACCESS_KEY`,
   `S3_SECRET_KEY` (and `S3_REGION=fsn1` — `S3Storage` defaults the region to
   `us-east-1`, which is wrong for Hetzner and must be set explicitly). Real
   key material never enters git; the ADR records only the secret's *name*.
3. **Fallback, explicitly scoped**: if a future requirement needs
   in-cluster data locality (e.g. air-gapped/offline mode, or Object Storage
   egress cost/latency becomes a real problem), MinIO can be added as a
   Helm subchart under `helm/cyber-accountant` without touching
   `Storage.hpp` — only the `s3-credentials` secret's `S3_ENDPOINT` changes.
   That would need its own ADR superseding this one, not a silent chart edit.
4. **Revisit trigger**: if the cluster grows a dedicated worker pool with
   materially more free RAM/disk than measured above, and the owner wants
   in-cluster storage for cost or locality reasons, reopen this ADR — the
   data above is what would need to look different (headroom *and* an
   owned, replicated, backed-up MinIO deployment, not the existing
   unmanaged one).

## Consequences

- **+** Zero code change either way — `S3Storage` already speaks any
  S3-compatible endpoint; only the secret's `S3_ENDPOINT`/region/keys differ.
- **+** No new stateful workload on an already-constrained 3-node
  control-plane-only cluster; no bucket backup/upgrade/SPOF to operate.
- **+** Bucket and region colocated with the cluster (fsn1) — no cross-region
  latency or egress.
- **−** Bucket creation and S3 key generation are manual, Console-only steps
  (no Terraform/hcloud-CLI coverage for Hetzner Object Storage today) — see
  the MANUAL STEP in the task-10 report for the exact instructions.
- **−** Adds an external dependency (Hetzner Object Storage API/uptime)
  instead of keeping storage inside the already-provisioned cluster.

## Not adopted (deferred)

- **In-cluster MinIO (fresh, project-owned subchart)** — viable later per the
  fallback above, but not justified today: no data-locality requirement
  exists yet, and it would be the fourth-plus stateful workload competing for
  the same single-replica-rationed disk/RAM budget.
- **Reusing the existing unmanaged `minio` namespace** — rejected outright:
  unowned by this project, single-pod SPOF, no visible backup policy: wrong
  place to put tenant financial documents.

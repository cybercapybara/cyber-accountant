# ADR 0007 — S3 backend: Hetzner Object Storage, not in-cluster MinIO

Status: Superseded by owner decision — 2026-08-14

## Owner decision — 2026-08-14 (supersedes the decision below)

The owner reversed this ADR's original conclusion the same day it was
written: **production S3 backend is the existing in-cluster MinIO**, not
Hetzner Object Storage. Reason given: keep both the data and the cost inside
the already-provisioned cluster rather than adding an external paid service,
accepting the SPOF/backup gaps noted in the Context below as a known,
revisitable trade-off rather than a blocker.

- **Endpoint**: `http://minio.minio.svc.cluster.local:9000` (in-cluster
  MinIO, namespace `minio`, `quay.io/minio/minio:RELEASE.2024-12-18T13-15-44Z`
  — the same instance the original Context section described and rejected).
- **Bucket**: `cyber-accountant-prod`, created via a one-off `mc` pod against
  MinIO's root credentials (`minio-root` secret in the `minio` namespace),
  then deleted — no standing access to root credentials anywhere.
- **Application access**: a dedicated, non-root MinIO user was created
  (`mc admin user add` with a random access key/secret pair) with an
  `mc admin policy` scoped to `s3:*` on `arn:aws:s3:::cyber-accountant-prod`
  and `arn:aws:s3:::cyber-accountant-prod/*` only — verified to list/read/
  write that bucket and to get `Access Denied` on the cluster's other
  pre-existing buckets (`cybercapybara`, `loki`, `tempo`).
- **Secret contract unchanged**: `s3-credentials` in the `cyber-accountant`
  namespace still holds `S3_ENDPOINT`, `S3_BUCKET`, `S3_REGION`,
  `S3_ACCESS_KEY`, `S3_SECRET_KEY` — `S3Storage` needs no code change.
  `S3_REGION=us-east-1` (MinIO doesn't enforce a real region; this is
  `S3Storage`'s existing default, kept as-is since there's no `fsn1`-style
  region concept for an in-cluster MinIO endpoint).
- **Hetzner Object Storage remains documented below as the rejected
  alternative** — the original Context/Decision/Consequences sections are
  kept verbatim as the record of that (superseded) reasoning, including why
  it was originally preferred (managed, no SPOF/backup ownership, colocated
  region). That reasoning wasn't wrong; the owner simply weighted
  in-cluster cost/data-locality higher.
- **Revisit trigger, updated**: if the existing `minio` deployment's
  single-pod/no-backup posture becomes a real incident (data loss, extended
  outage), or the owner wants durability guarantees Hetzner Object Storage
  would provide, reopen this ADR again.

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

## Decision (original — superseded by the owner decision above)

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

## Consequences (original — superseded by the owner decision above)

- **+** Zero code change either way — `S3Storage` already speaks any
  S3-compatible endpoint; only the secret's `S3_ENDPOINT`/region/keys differ.
- **+** No new stateful workload on an already-constrained 3-node
  control-plane-only cluster; no bucket backup/upgrade/SPOF to operate.
- **+** Bucket and region colocated with the cluster (fsn1) — no cross-region
  latency or egress.
- **−** Bucket creation and S3 key generation are manual, Console-only steps
  (no Terraform/hcloud-CLI coverage for Hetzner Object Storage today) — see
  "Manual provisioning" below for the exact steps.
- **−** Adds an external dependency (Hetzner Object Storage API/uptime)
  instead of keeping storage inside the already-provisioned cluster.

## Provisioning — actual state (owner decision, done automatically)

Unlike the original ADR's assumption (Hetzner Console-only, manual), the
in-cluster MinIO path was fully scripted, no Console/manual step involved:

1. **Bucket + dedicated app user**: a one-off `mc` pod (`quay.io/minio/mc`)
   was run in the `minio` namespace with the existing root credentials
   (`minio-root` secret, `secretKeyRef`, never exported to a shell or file),
   used to:
   - `mc mb --ignore-existing` the `cyber-accountant-prod` bucket;
   - generate a random access key/secret (`openssl rand -hex 16` × 2) and
     `mc admin user add` a non-root MinIO user with them;
   - `mc admin policy create` + `attach` a policy granting `s3:*` scoped to
     only `arn:aws:s3:::cyber-accountant-prod{,/*}`.
   The pod was deleted immediately after; root credentials touched nothing
   outside that pod's exec session.
2. **Cluster secret** (namespace `cyber-accountant`, created idempotently via
   `--dry-run=client -o yaml | kubectl apply -f -`):

   ```bash
   KUBECONFIG=../cluster/kubeconfig kubectl -n cyber-accountant create secret generic s3-credentials \
     --from-literal=S3_ENDPOINT=http://minio.minio.svc.cluster.local:9000 \
     --from-literal=S3_BUCKET=cyber-accountant-prod \
     --from-literal=S3_REGION=us-east-1 \
     --from-literal=S3_ACCESS_KEY=<generated> \
     --from-literal=S3_SECRET_KEY=<generated> \
     --dry-run=client -o yaml | kubectl apply -f -
   ```

   Real key material lives only in the cluster secret (and briefly in the
   provisioning pod's exec session); nothing was ever written to this repo
   or to disk outside the operator's scratch directory during the run.
3. **Verified isolation**: with the new (non-root) credentials, `mc ls`
   listed only `cyber-accountant-prod` and could read/write into it;
   `mc ls` against the cluster's other pre-existing buckets
   (`cybercapybara`, `loki`, `tempo`) returned `Access Denied`.

## Not adopted (deferred)

- **Hetzner Object Storage** — this ADR's original recommendation (see
  Decision/Consequences above); not adopted per the owner's 2026-08-14
  reversal. Nothing about the original reasoning was wrong — managed,
  colocated in `fsn1`, no SPOF/backup ownership — the owner simply chose to
  keep data and cost inside the cluster instead. Revisit if the in-cluster
  MinIO's single-pod/no-backup posture causes a real incident.
- **A fresh, project-owned MinIO subchart** (the original ADR's fallback
  option) — superseded by simply using the existing in-cluster instance
  directly; no new stateful workload was added.

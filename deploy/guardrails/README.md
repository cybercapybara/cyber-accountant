# guardrails-llm-filter — LLM masking proxy

Owner decision, 2026-08-14: this replaces the previously-planned
`cybercapybara-llm-guard` as the LLM egress point. See
`docs/superpowers/specs/2026-08-14-cyber-accountant-design.md` (§2, §4,
§11.1, §16) for the design-level rationale.

## What it is

[`guardrails-llm-filter`](https://github.com/cloud-ru-tech/guardrails-llm-filter)
(Cloud.ru) is a transparent HTTP proxy that sits between our agent code and
`api.anthropic.com`. It is **not** an API gateway, auth layer, or budget
tracker:

- Scans outgoing requests with ~265 built-in regex rules (credentials, API
  keys, access tokens, IPs, PII including Russian-specific formats — cards,
  IBAN, SNILS/INN/OGRN with checksum validation) and replaces matches with
  placeholders (`<EMAIL_1>`, ...) before forwarding upstream.
  Un-masks the response back to originals for the caller, including
  token-by-token SSE streaming and tool-call arguments. The model never sees
  real sensitive data; the caller never sees placeholders.
- Supports Anthropic's `/v1/messages` natively — JSON and streaming, plus
  tool-call argument masking/unmasking.
- **Fail-open by design**: any internal error passes traffic through
  unmodified rather than blocking it. This is a deliberate reliability
  trade-off in the upstream project, not a bug — do not treat this proxy as
  a hard security boundary; treat it as best-effort DLP.
- **Auth headers pass through untouched.** cyber-accountant has no Anthropic
  API key stored anywhere in this deployment — the caller's own
  `x-api-key`/`Authorization` header travels through the proxy unmodified to
  `api.anthropic.com`. Only the base URL changes on the client side.
- Does **not** do audit logging or per-org budget/rate limiting on its own.
  That responsibility belongs to the `agent` module (P4 in the design spec)
  — the proxy masks, nothing else.
- Config API / web console (`:9080`) is unauthenticated and mutating (rule
  CRUD, enforce/shadow toggle). We don't operate it and want it unreachable
  — see "Security fix — 2026-08-14" below for how that's actually enforced
  (an empty `GUARDRAILS_API_ADDR` does **not** work, despite what it looks
  like it should do).

## Where it comes from

Public image: `ghcr.io/cloud-ru-tech/guardrails-llm-filter`. Manifests here
are adapted from the upstream reference deploy at
`_reference/guardrails-llm-filter/deploy/kubernetes/` (README + configs in
that repo document the full env var surface — see there for anything not
covered by our minimal `configmap.yml`).

Deployed here as plain manifests (no Helm) — this is a cluster ops artifact,
not part of the umbrella chart.

## What's deployed

- `namespace.yml` — `guardrails` namespace.
- `configmap.yml` — env for the data plane: upstream is
  `https://api.anthropic.com`, `in_memory` store (single replica, nothing to
  share), config API bound to loopback only (see security fix below).
- `deployment.yml` — single replica, image pinned by **tag + digest**
  (`0.1.2@sha256:...`), modest resources (25m/64Mi requests, 250m/256Mi
  limits), `/healthz` liveness + `/readyz` readiness on `:8080`.
- `service.yml` — ClusterIP, ports `8080` (data plane) and `9090` (metrics).
  No ingress — cluster-internal only.
- `networkpolicy.yml` — default-deny ingress for the `guardrails` namespace,
  plus a single allow rule: TCP `8080` from pods in the `cyber-accountant`
  namespace only. Port `9080` (config API) has no allow rule at all —
  nothing outside the pod's own network namespace can reach it over the
  network, full stop. Port `9090` (metrics) is also not opened; nothing
  scrapes it today.

No `secret.yml`: the upstream reference's secret is only needed for a
Redis/Postgres store backend or at-rest encryption key, neither of which we
use (`in_memory` store, single replica).

## Security fix — 2026-08-14

A push scanner + manual cluster check found the config API answering on
`:9080` despite `configmap.yml` originally setting `GUARDRAILS_API_ADDR: ""`
to disable it (per the upstream reference's own comment,
`internal/config/config.go:207`: "empty disables the API server").

**Root cause**: that comment describes `servers.go`'s check
(`if e.cfg.API.Addr == "" { return nil }`), but the field is populated by
`caarlos0/env v11.4.1`, and that library's `getOr()` (`env.go:648`) treats
an environment variable that **exists but is empty** the same as **unset**
whenever the field has an `envDefault` — it substitutes the default
(`:9080`) instead of leaving the field as `""`. So
`GUARDRAILS_API_ADDR: ""` never reaches the app as an empty string; it
silently becomes `:9080`, and `servers.go`'s "empty disables it" branch
never fires. There is no env-var value that reaches this field as a real
empty string through this library — "disable via empty string" is not
achievable here at all.

**Fix applied**:

1. `configmap.yml`: `GUARDRAILS_API_ADDR: "127.0.0.1:9080"` — binds the
   config API to the pod's loopback interface. Reachable only from
   processes sharing the pod's network namespace (i.e. `kubectl exec` into
   the pod itself); not reachable via the pod's real IP, the Service, or
   from any other pod.
2. `networkpolicy.yml` (new): default-deny ingress for the namespace, plus
   an allow rule scoped to `cyber-accountant` on port `8080` only. This is
   the actual network-level control; the loopback bind is defense in depth
   on top of it, not a replacement for it.

**Tested, factual results** (this matters — don't assume from the fix
description alone):

- `kubectl -n guardrails port-forward pod/<pod> 9080` **still succeeds** and
  a `curl` through it **still gets `HTTP 200`**, even though the app binds
  only to `127.0.0.1:9080` inside the container. This is expected, not a
  bug in the fix: `kubectl port-forward` connects to `localhost:<port>`
  from *inside* the pod's network namespace (via the kubelet/runtime, not
  over the pod's `eth0`), so a loopback-bound port is exactly as reachable
  from port-forward as from a process actually running in the pod. **A
  loopback bind does not, and cannot, block `kubectl port-forward`** — it
  only blocks access arriving over the pod's real network interface (other
  pods, Services, anything routed via the CNI). Don't rely on it as a
  barrier against anyone with `pods/portforward` RBAC.
- `/healthz` on `:8080` via port-forward: `HTTP 200` (unaffected, as
  expected — the data plane isn't touched by this fix).
- From a temporary pod in the `default` namespace:
  `curl http://guardrails-llm-filter.guardrails:8080/healthz` — connection
  **timed out** (NetworkPolicy default-deny, no allow rule for `default`).
  From a temporary pod in the `cyber-accountant` namespace, the same
  request returned `HTTP 200` (matches the allow rule). Port `9080` from
  the `cyber-accountant` pod also timed out — no allow rule covers it,
  matching intent.

**Credential-exposure false positive**: a scanner flagged `configmap.yml`
for strings like `API_KEYS`/`CREDENTIALS`. These are guardrails-llm-filter's
built-in *data-type category names* (`GUARDRAILS_DATA_TYPES` — which
categories of sensitive data to scan *for*), not actual credentials or key
material. There is no secret value anywhere in `configmap.yml`; real
credentials for this deployment don't exist yet (see "Auth headers pass
through untouched" above) and would live in a `Secret`, not here.

## Note on version pinning

The task asked for tag `v0.1.2`; the actual tag published on ghcr.io is
`0.1.2` (no `v` prefix) — confirmed via the anonymous token flow:

```sh
TOKEN=$(curl -s "https://ghcr.io/token?scope=repository:cloud-ru-tech/guardrails-llm-filter:pull" | jq -r .token)
curl -s -H "Authorization: Bearer $TOKEN" \
  https://ghcr.io/v2/cloud-ru-tech/guardrails-llm-filter/tags/list
# {"name":"...","tags":["0.1.0","0.1","latest","0.1.1","0.1.2"]}
```

`deployment.yml` pins both the tag and the resolved manifest digest for
`0.1.2` (`sha256:4171df7af4bcae60be92ec19ccbd97d5731bd3e488c1cb5279c75258d42c90c4`).

## How to update the image

1. Check available tags (command above) and pick the new version.
2. Resolve its digest:
   ```sh
   TOKEN=$(curl -s "https://ghcr.io/token?scope=repository:cloud-ru-tech/guardrails-llm-filter:pull" | jq -r .token)
   curl -sI -H "Authorization: Bearer $TOKEN" \
     -H "Accept: application/vnd.docker.distribution.manifest.v2+json,application/vnd.oci.image.index.v1+json" \
     "https://ghcr.io/v2/cloud-ru-tech/guardrails-llm-filter/manifests/<new-tag>" \
     | grep -i docker-content-digest
   ```
3. Update the `image:` line in `deployment.yml` with the new
   `<tag>@sha256:<digest>`, diff `configmap.yml` against the upstream
   reference's `deploy/kubernetes/configmap.yml` for any new required env
   vars, then `kubectl apply -f deploy/guardrails/`.
4. Never deploy `:latest` — always pin tag + digest, so a rollback is a
   one-line revert.

## Deploying / verifying

```sh
export KUBECONFIG=../cluster/kubeconfig   # or wherever it lives locally
kubectl apply -f deploy/guardrails/

kubectl -n guardrails rollout status deployment/guardrails-llm-filter

kubectl -n guardrails port-forward svc/guardrails-llm-filter 8080:8080 &
curl -sS -o /dev/null -w '%{http_code}\n' http://localhost:8080/healthz
curl -sS -o /dev/null -w '%{http_code}\n' http://localhost:8080/readyz
```

To use it from cyber-accountant, point the Anthropic base URL at
`http://guardrails-llm-filter.guardrails.svc.cluster.local:8080` instead of
`https://api.anthropic.com` — the client-supplied API key still travels in
the request headers unchanged.

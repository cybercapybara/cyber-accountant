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
  CRUD, enforce/shadow toggle). We don't operate it and have it disabled
  (`GUARDRAILS_API_ADDR: ""` in `configmap.yml`) rather than rely solely on
  network policy to keep it private.

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
  share), config API disabled.
- `deployment.yml` — single replica, image pinned by **tag + digest**
  (`0.1.2@sha256:...`), modest resources (25m/64Mi requests, 250m/256Mi
  limits), `/healthz` liveness + `/readyz` readiness on `:8080`.
- `service.yml` — ClusterIP, ports `8080` (data plane) and `9090` (metrics).
  No ingress — cluster-internal only.

No `secret.yml`: the upstream reference's secret is only needed for a
Redis/Postgres store backend or at-rest encryption key, neither of which we
use (`in_memory` store, single replica).

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

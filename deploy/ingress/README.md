# Ingress — buh.cybercapybara.kz

Public entrypoint for the cyber-accountant release, added 2026-08-14 per
owner decision.

## What's here

- `buh-cybercapybara-kz.yaml` — one `Ingress` object in the
  `cyber-accountant` namespace, host `buh.cybercapybara.kz` → Service
  `web:8080` (the SPA; its nginx already proxies `/api/` to the `api`
  Service itself — see `helm/cyber-accountant` chart's `web` ConfigMap —
  so a single backend covers both the app and the API).

## Why a standalone manifest, not helm values

`helm/cpp-env`'s umbrella already renders two Ingress objects per
environment (`templates/ingress.yaml` + the `cpp-env.ingress` /
`cpp-env.host` helpers in `templates/_helpers.tpl`), but that helper is
hardcoded to exactly `api.<env>.<baseDomain>` and `app.<env>.<baseDomain>`.
This release's `env`/`baseDomain` (`cybercapybara` / `example.com`, see
`helm/cpp-env/values-cybercapybara.yaml`) render those as placeholder hosts
nobody owns. Making that helper produce `buh.cybercapybara.kz` would mean
either templating changes (out of scope for this change) or repurposing
`env`/`baseDomain` in a way that also renames the existing api/app Ingress
objects — disruptive, for a chart that isn't shaped for a single custom
public hostname. A standalone manifest is the documented fallback for
exactly this case and matches how every other real hostname already live in
this cluster is wired: `www/frontend-cpp-frontend`, `minio/minio`,
`minio/minio-console`, `monitoring/kube-prometheus-stack-grafana` are all
standalone `Ingress` objects, not templated through a shared per-env
helper.

## Infra this relies on (already present, nothing installed)

Surveyed before writing anything:

- **ingress-nginx**: already running, 3 replicas, namespace `ingress-nginx`,
  `Service/ingress-nginx-controller` type `LoadBalancer`
  (`hcloud-cloud-controller-manager` is present and provisioned it — IPs
  `10.0.1.1` (internal), `2a01:4f8:c01e:3387::1` (IPv6), `49.12.17.46`
  (the public Hetzner LB IP)). `ingressClassName: nginx` everywhere in this
  cluster targets this controller.
- **cert-manager**: already running (namespace `cert-manager`), with a
  `ClusterIssuer/letsencrypt-prod` already `Ready: True` (production ACME,
  not staging) — the same issuer every other real hostname in this cluster
  uses (`www`, `minio`, `grafana`).
- **DNS**: `*.cybercapybara.kz` is an existing wildcard (owner-confirmed,
  not touched by this change). `dig +short test.cybercapybara.kz` and
  `dig +short cybercapybara.kz` both resolve to Cloudflare anycast IPs
  (`172.67.175.222`, `104.21.83.128`) — the zone is proxied through
  Cloudflare. The existing `www/frontend-cpp-frontend` Ingress carries
  `external-dns.alpha.kubernetes.io/target: 49.12.17.46`, confirming
  Cloudflare's origin for this zone is this cluster's ingress-nginx LB IP.
  Since the wildcard already covers `buh.cybercapybara.kz` and proxies to
  that same LB, no DNS/external-dns change was needed or made for this
  Ingress (deliberately no `external-dns.alpha.kubernetes.io/*` annotations
  here — this object relies entirely on the pre-existing wildcard).

## Verifying / re-applying

```sh
export KUBECONFIG=../cluster/kubeconfig
kubectl apply -f deploy/ingress/buh-cybercapybara-kz.yaml

kubectl -n cyber-accountant get ingress buh-cybercapybara-kz
kubectl -n cyber-accountant get certificate buh-cybercapybara-kz-tls
kubectl -n cyber-accountant get challenge   # only present while ACME HTTP-01 is in flight

curl -I https://buh.cybercapybara.kz/healthz   # expect 200 once the cert is issued
curl -I https://cybercapybara.kz               # must still be 200 — unrelated to this change,
                                                # but was checked before AND after to be sure
```

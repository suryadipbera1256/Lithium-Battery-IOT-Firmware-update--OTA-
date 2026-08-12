# Production deployment — ECS Fargate + ALB + Cognito OIDC

Target: `https://ota.internal.pointo.in` serving the dashboard from private subnets,
authenticated at the load balancer before a single byte reaches Streamlit.

```
Operator ──TLS 1.3──▶ ALB :443 ─── authenticate-cognito ──▶ target group :8501
   (office CIDR)       (public subnets)     │                  │
                                            ▼                  ▼
                                    Cognito Hosted UI     Fargate task
                                                          (private subnet,
                                                           no public IP)
                                                              │
                                       ┌──────────────────────┼───────────────┐
                                       ▼                      ▼               ▼
                                  S3 as-ota-firmware    IoT control plane  Secrets Mgr
                                  (VPC gw endpoint)     (CreateJob)        (VPC endpoint)

Browser ══ WSS + SigV4 ═══════════════════════════════▶ IoT Core (data plane)
   (direct; never traverses the task — see components/telemetry.py)
```

The telemetry socket is browser→IoT Core directly, so the **task needs no IoT
data-plane egress**. Only the control plane (`CreateJob`, `ListThings`) is called
server-side.

---

## 0 · Prerequisites

```bash
export ACCOUNT_ID=$(aws sts get-caller-identity --query Account --output text)
export REGION=ap-south-1
export DOMAIN=ota.internal.pointo.in
```

---

## 1 · The presign role (do this first — OTA is broken without it)

`${aws:iot:s3-presigned-url:...}` is expanded **only** when `CreateJob` supplies
`presignedUrlConfig.roleArn`. Omit it and devices receive the placeholder
verbatim, then fail at `AT+QHTTPGET` with HTTP 403.

```bash
# Split iam/iot-presign-role.json into the two documents it contains.
aws iam create-role --role-name AsOtaIotPresignRole \
  --assume-role-policy-document file://trust.json
aws iam put-role-policy --role-name AsOtaIotPresignRole \
  --policy-name SignFirmwareObjects --policy-document file://perms.json
```

Put the resulting ARN in `[iot].presign_role_arn`. `core/jobs.py` refuses to
create a job without it.

---

## 2 · ECR + image

```bash
aws ecr create-repository --repository-name as-ota-dashboard \
  --image-scanning-configuration scanOnPush=true \
  --image-tag-mutability IMMUTABLE \
  --encryption-configuration encryptionType=AES256

aws ecr get-login-password --region $REGION | \
  docker login --username AWS --password-stdin $ACCOUNT_ID.dkr.ecr.$REGION.amazonaws.com

cd tools/ota-dashboard
docker build -t as-ota-dashboard:$(git rev-parse --short HEAD) .
docker tag as-ota-dashboard:$(git rev-parse --short HEAD) \
  $ACCOUNT_ID.dkr.ecr.$REGION.amazonaws.com/as-ota-dashboard:$(git rev-parse --short HEAD)
docker push $ACCOUNT_ID.dkr.ecr.$REGION.amazonaws.com/as-ota-dashboard:$(git rev-parse --short HEAD)
```

`IMMUTABLE` tags matter: a mutable `latest` means the image that passed review is
not provably the image running.

---

## 3 · Secrets

The whole `secrets.toml` document becomes one secret value. Nothing is baked into
the image; `docker-entrypoint.sh` writes it to ephemeral storage at boot.

```bash
aws secretsmanager create-secret \
  --name as-ota-dashboard/secrets-toml \
  --secret-string file://.streamlit/secrets.toml
```

Set `access_key_id`/`secret_access_key`/`profile` to **empty strings** — boto3
resolves the task role from the ECS metadata endpoint. Static keys in a container
are the loophole this architecture exists to remove.

---

## 4 · IAM roles (three, not two)

| Role | Assumed by | Policy |
|---|---|---|
| `AsOtaDashboardTaskRole` | the application | `iam/ecs-task-role-policy.json` |
| `AsOtaDashboardExecRole` | the ECS agent | `iam/ecs-task-execution-role-policy.json` |
| `AsOtaIotPresignRole` | `iot.amazonaws.com` | `iam/iot-presign-role.json` |

Separation is the control: the app can create jobs but cannot read the raw
secret; the agent can read the secret but cannot touch IoT.

Trust policy for both ECS roles:

```json
{"Version":"2012-10-17","Statement":[{"Effect":"Allow",
 "Principal":{"Service":"ecs-tasks.amazonaws.com"},"Action":"sts:AssumeRole",
 "Condition":{"ArnLike":{"aws:SourceArn":"arn:aws:ecs:REGION:ACCOUNT_ID:*"},
              "StringEquals":{"aws:SourceAccount":"ACCOUNT_ID"}}}]}
```

---

## 5 · Task definition

```json
{
  "family": "as-ota-dashboard",
  "requiresCompatibilities": ["FARGATE"],
  "networkMode": "awsvpc",
  "cpu": "512",
  "memory": "1024",
  "runtimePlatform": { "cpuArchitecture": "X86_64", "operatingSystemFamily": "LINUX" },
  "taskRoleArn": "arn:aws:iam::ACCOUNT_ID:role/AsOtaDashboardTaskRole",
  "executionRoleArn": "arn:aws:iam::ACCOUNT_ID:role/AsOtaDashboardExecRole",

  "volumes": [
    { "name": "streamlit-home" },
    { "name": "tmp" }
  ],

  "containerDefinitions": [
    {
      "name": "dashboard",
      "image": "ACCOUNT_ID.dkr.ecr.REGION.amazonaws.com/as-ota-dashboard:GIT_SHA",
      "essential": true,
      "portMappings": [{ "containerPort": 8501, "protocol": "tcp" }],

      "readonlyRootFilesystem": true,
      "user": "10001",
      "linuxParameters": { "initProcessEnabled": true },

      "mountPoints": [
        { "sourceVolume": "streamlit-home", "containerPath": "/home/app", "readOnly": false },
        { "sourceVolume": "tmp",            "containerPath": "/tmp",      "readOnly": false }
      ],

      "secrets": [
        {
          "name": "SECRETS_TOML",
          "valueFrom": "arn:aws:secretsmanager:REGION:ACCOUNT_ID:secret:as-ota-dashboard/secrets-toml"
        }
      ],

      "environment": [
        { "name": "STREAMLIT_SERVER_ENABLE_CORS", "value": "false" },
        { "name": "STREAMLIT_SERVER_ENABLE_XSRF_PROTECTION", "value": "true" },
        { "name": "STREAMLIT_SERVER_ENABLE_WEBSOCKET_COMPRESSION", "value": "false" },
        { "name": "STREAMLIT_BROWSER_GATHER_USAGE_STATS", "value": "false" }
      ],

      "healthCheck": {
        "command": ["CMD-SHELL", "curl -fsS http://127.0.0.1:8501/_stcore/health || exit 1"],
        "interval": 30, "timeout": 5, "retries": 3, "startPeriod": 20
      },

      "logConfiguration": {
        "logDriver": "awslogs",
        "options": {
          "awslogs-group": "/ecs/as-ota-dashboard",
          "awslogs-region": "REGION",
          "awslogs-stream-prefix": "task"
        }
      }
    }
  ]
}
```

**`readonlyRootFilesystem: true` requires those two volumes.** Fargate does not
support `linuxParameters.tmpfs` (EC2 launch type only), so writable scratch comes
from name-only volumes backed by task ephemeral storage — which is encrypted and
destroyed with the task, so the materialised secret never persists.

---

## 6 · Networking and security groups

```bash
# ALB: public subnets, 443 only, restricted source.
aws ec2 authorize-security-group-ingress --group-id $SG_ALB \
  --protocol tcp --port 443 --cidr <OFFICE_CIDR>/32

# Task: reachable ONLY from the ALB. Source is the SG, not a CIDR.
aws ec2 authorize-security-group-ingress --group-id $SG_TASK \
  --protocol tcp --port 8501 --source-group $SG_ALB
```

Tasks run in **private** subnets with `assignPublicIp=DISABLED`. Add VPC
endpoints so no NAT path is needed for the common calls:

| Endpoint | Type |
|---|---|
| `com.amazonaws.$REGION.ecr.api`, `.ecr.dkr` | Interface |
| `com.amazonaws.$REGION.s3` | **Gateway** |
| `com.amazonaws.$REGION.secretsmanager` | Interface |
| `com.amazonaws.$REGION.logs` | Interface |
| `com.amazonaws.$REGION.sts` | Interface |

Confirm interface-endpoint availability for the IoT **control plane** in your
region before removing NAT entirely — `CreateJob`/`ListThings` are control-plane
calls and endpoint coverage varies. Keep a NAT gateway until verified.

---

## 7 · ALB + target group

```bash
aws elbv2 create-target-group --name as-ota-dash-tg \
  --protocol HTTP --port 8501 --vpc-id $VPC_ID --target-type ip \
  --health-check-path /_stcore/health --matcher HttpCode=200 \
  --health-check-interval-seconds 15 --healthy-threshold-count 2

# Streamlit holds session state per WebSocket. With >1 task the initial GET and
# the /_stcore/stream upgrade must land on the same target.
aws elbv2 modify-target-group-attributes --target-group-arn $TG \
  --attributes Key=stickiness.enabled,Value=true \
               Key=stickiness.type,Value=lb_cookie \
               Key=stickiness.lb_cookie.duration_seconds,Value=86400 \
               Key=deregistration_delay.timeout_seconds,Value=30

# Default 60 s idle timeout will cut the telemetry WebSocket.
aws elbv2 modify-load-balancer-attributes --load-balancer-arn $ALB \
  --attributes Key=idle_timeout.timeout_seconds,Value=300 \
               Key=routing.http.drop_invalid_header_fields.enabled,Value=true \
               Key=deletion_protection.enabled,Value=true \
               Key=access_logs.s3.enabled,Value=true \
               Key=access_logs.s3.bucket,Value=<log-bucket>
```

---

## 8 · Cognito OIDC at the listener

Auth happens **at the ALB**. Unauthenticated traffic never reaches Streamlit.

```bash
POOL_ID=$(aws cognito-idp create-user-pool --pool-name as-ota-operators \
  --mfa-configuration ON \
  --admin-create-user-config AllowAdminCreateUserOnly=true \
  --policies 'PasswordPolicy={MinimumLength=14,RequireUppercase=true,RequireLowercase=true,RequireNumbers=true,RequireSymbols=true}' \
  --query 'UserPool.Id' --output text)

aws cognito-idp create-user-pool-domain \
  --user-pool-id $POOL_ID --domain as-ota-operators

# The callback path is fixed by ALB and must be exact.
aws cognito-idp create-user-pool-client --user-pool-id $POOL_ID \
  --client-name alb --generate-secret \
  --allowed-o-auth-flows code --allowed-o-auth-scopes openid email \
  --allowed-o-auth-flows-user-pool-client \
  --supported-identity-providers COGNITO \
  --callback-urls "https://$DOMAIN/oauth2/idpresponse" \
  --logout-urls "https://$DOMAIN/"
```

`AllowAdminCreateUserOnly=true` disables self-signup — nobody grants themselves
access. `MFA ON` is not optional for a console that controls MOSFETs.

```bash
# 443 listener: authenticate FIRST, forward SECOND. Order is significant.
aws elbv2 create-listener --load-balancer-arn $ALB \
  --protocol HTTPS --port 443 --certificates CertificateArn=$ACM_ARN \
  --ssl-policy ELBSecurityPolicy-TLS13-1-2-2021-06 \
  --default-actions '[
    {"Type":"authenticate-cognito","Order":1,
     "AuthenticateCognitoConfig":{
       "UserPoolArn":"arn:aws:cognito-idp:REGION:ACCOUNT_ID:userpool/POOL_ID",
       "UserPoolClientId":"CLIENT_ID",
       "UserPoolDomain":"as-ota-operators",
       "Scope":"openid email",
       "SessionCookieName":"AWSELBAuthSessionCookie",
       "SessionTimeout":28800,
       "OnUnauthenticatedRequest":"authenticate"}},
    {"Type":"forward","Order":2,"TargetGroupArn":"'$TG'"}]'

# 80 -> 443 only.
aws elbv2 create-listener --load-balancer-arn $ALB --protocol HTTP --port 80 \
  --default-actions '[{"Type":"redirect","RedirectConfig":{"Protocol":"HTTPS","Port":"443","StatusCode":"HTTP_301"}}]'
```

Cognito auth requires an **HTTPS** listener — it cannot be attached to HTTP.

Attach WAF with the managed common rule set and a rate limit:

```bash
aws wafv2 associate-web-acl --web-acl-arn $WAF_ARN --resource-arn $ALB
```

---

## 9 · Close the authorisation loophole this creates

With ALB auth in front, the app's own password gate becomes the **weaker** of two
mechanisms, and it has a real flaw: `core/auth.py` derives the operator/viewer
role from *which password was typed*. Any authenticated Cognito user who learns
the operator password escalates to CreateJob.

Fix: derive the role from Cognito group membership. ALB injects a signed JWT in
`x-amzn-oidc-data`, and Streamlit ≥1.37 exposes request headers.

```python
# core/auth.py — sketch; validate the signature before trusting any claim.
import base64, json
import streamlit as st

def alb_identity() -> dict | None:
    raw = st.context.headers.get("x-amzn-oidc-data")
    if not raw:
        return None                      # local run, or ALB auth not in path
    # MUST verify: fetch the signing key from
    # https://public-keys.auth.elb.<region>.amazonaws.com/<kid>
    # (kid comes from the JWT header) and verify ES256 before use.
    payload = json.loads(base64.urlsafe_b64decode(raw.split(".")[1] + "=="))
    return payload                        # contains email + cognito:groups
```

Create `as-ota-operators` and `as-ota-viewers` groups, map them to roles, and
keep the password gate only as a local-development fallback. **Do not trust the
header without verifying its ES256 signature** — treat it as attacker-controlled
until verified, since a task reachable from anywhere but the ALB could be fed a
forged header. The security-group rule in §6 is what makes that hard; signature
verification is what makes it impossible.

---

## 10 · Service

```bash
aws ecs create-service --cluster as-ota --service-name dashboard \
  --task-definition as-ota-dashboard --desired-count 2 \
  --launch-type FARGATE \
  --network-configuration "awsvpcConfiguration={subnets=[$PRIV_A,$PRIV_B],securityGroups=[$SG_TASK],assignPublicIp=DISABLED}" \
  --load-balancers "targetGroupArn=$TG,containerName=dashboard,containerPort=8501" \
  --health-check-grace-period-seconds 45 \
  --deployment-configuration "minimumHealthyPercent=100,maximumPercent=200,deploymentCircuitBreaker={enable=true,rollback=true}" \
  --enable-execute-command
```

`desired-count 2` spans two AZs; stickiness (§7) keeps sessions coherent. The
circuit breaker rolls back a task that never passes its health check.

---

## 11 · CI/CD (GitHub Actions, OIDC — no stored keys)

```yaml
name: deploy-dashboard
on:
  push:
    branches: [main]
    paths: ['tools/ota-dashboard/**']

permissions:
  id-token: write        # OIDC federation; no AWS keys in GitHub secrets
  contents: read

jobs:
  deploy:
    runs-on: ubuntu-latest
    defaults: { run: { working-directory: tools/ota-dashboard } }
    steps:
      - uses: actions/checkout@v4

      - name: Contract tests (firmware <-> dashboard)
        run: |
          python -m pip install -r requirements.txt
          python tests/test_phase_h.py

      - uses: aws-actions/configure-aws-credentials@v4
        with:
          role-to-assume: arn:aws:iam::ACCOUNT_ID:role/GhaDeployDashboard
          aws-region: ap-south-1

      - uses: aws-actions/amazon-ecr-login@v2
        id: ecr

      - name: Build and push
        env:
          IMAGE: ${{ steps.ecr.outputs.registry }}/as-ota-dashboard:${{ github.sha }}
        run: |
          docker build -t "$IMAGE" .
          docker push "$IMAGE"

      - name: Fail on CRITICAL findings
        run: |
          aws ecr wait image-scan-complete --repository-name as-ota-dashboard \
            --image-id imageTag=${{ github.sha }}
          CRIT=$(aws ecr describe-image-scan-findings \
            --repository-name as-ota-dashboard --image-id imageTag=${{ github.sha }} \
            --query 'imageScanFindings.findingSeverityCounts.CRITICAL' --output text)
          [ "$CRIT" = "None" ] || [ -z "$CRIT" ] || { echo "CRITICAL: $CRIT"; exit 1; }

      - name: Roll out
        run: |
          aws ecs deploy --cluster as-ota --service dashboard \
            --task-definition as-ota-dashboard \
            --codedeploy-appspec /dev/null 2>/dev/null || \
          aws ecs update-service --cluster as-ota --service dashboard \
            --task-definition as-ota-dashboard --force-new-deployment
```

The GHA role needs only `ecr:*` on the one repository, `ecs:UpdateService` +
`ecs:DescribeServices` on the one service, and `iam:PassRole` on the two ECS
roles — never `ecs:RegisterTaskDefinition` with a wildcard `PassRole`, which is
privilege escalation to any role in the account.

---

## 12 · Post-deploy verification

| Check | Expected |
|---|---|
| `curl -I https://$DOMAIN` | `302` to the Cognito Hosted UI |
| `curl -I http://$DOMAIN` | `301` to HTTPS |
| `curl -I http://<task-private-ip>:8501` from outside the VPC | timeout |
| Sign in, open Live Telemetry | cards populate; `LIVE <n>s` badge |
| Disconnect a CAN harness | badge flips to `STALE`/`NO CAN`, block greys out |
| Upload a `.bin`, deploy to **one** thing | job reaches `SUCCEEDED` |
| CloudWatch `/ecs/as-ota-dashboard` | no `AccessDenied` |

If OTA fails with `get_len` on the device console, the presign role (§1) is
missing or lacks `s3:GetObject` on `firmwares/*`.

---

## 13 · Device-side prerequisite (not dashboard)

`OTA_REPORT_JOB_STATUS = 1` in `include/EC200U_AWS_OTA.h` requires the **device
certificate policy** to allow:

```json
{"Effect":"Allow",
 "Action":["iot:Publish","iot:Subscribe","iot:Receive"],
 "Resource":[
   "arn:aws:iot:REGION:ACCOUNT_ID:topic/$aws/things/${iot:Connection.Thing.ThingName}/jobs/*",
   "arn:aws:iot:REGION:ACCOUNT_ID:topicfilter/$aws/things/${iot:Connection.Thing.ThingName}/jobs/*"]}
```

Without it AWS closes the connection on an unauthorised publish. The firmware
falls back to notify-next automatically, but job executions never reach a
terminal state — and a non-terminal execution stays `$next` forever, **blocking
every later job** for that device.

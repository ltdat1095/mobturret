# aws — CloudFormation templates

**Status: planned.** This folder is a placeholder; nothing is built
yet.

All AWS infrastructure for MobTurret lives here as raw CloudFormation
templates (YAML). No CDK / SAM / Terraform — explicit choice so the
deploy surface is the same syntax AWS consumes.

## Planned layout

```
server/aws/
├── templates/                   # CloudFormation YAML
│   ├── 00-base.yaml             # S3 deploy bucket, KMS key, log group
│   ├── 10-storage.yaml          # DynamoDB tables + GSIs, S3 media bucket
│   ├── 20-iam.yaml              # Lambda execution roles, IoT thing policy
│   ├── 30-iot.yaml              # IoT thing, cert, topic rules
│   ├── 40-api.yaml              # API Gateway + authorizer + routes
│   ├── 50-lambdas.yaml          # All Lambda functions + event sources
│   └── 60-notifications.yaml    # SNS topic + platform applications (FCM/APNs)
├── params/
│   ├── dev.json
│   └── prod.json
├── scripts/
│   ├── deploy.sh                # cfn-lint, cfn-nag, then deploy stack-by-stack
│   └── package-lambdas.sh       # zip + upload Go binaries to S3
└── README.md
```

Stack numbering reflects deploy order; each stack exports values
the next one consumes via `!ImportValue` / `!Sub`.

## Conventions

- **YAML, not JSON.** Easier diffs, comments allowed.
- **No transforms.** No `AWS::LanguageExtensions` yet — keep
  templates compatible with vanilla CFN.
- **Parameters over hard-coding.** Region, account ID, environment
  name, and domain live in `params/<env>.json`.
- **All IAM scoped tight.** No `*` actions; no `*` resources unless
  the resource is genuinely account-wide (e.g. CloudWatch Logs).

## Validation

Before any deploy, run:

```bash
cfn-lint templates/                       # syntax + best-practice lint
cfn-nag templates/                        # security scan
```

Add both to CI when the project grows.

## Deploy

```bash
# From server/aws/
./scripts/package-lambdas.sh <env>        # builds + uploads Go zips to S3
./scripts/deploy.sh <env>                 # cfn-lint → cfn-nag → deploy stacks
```

`deploy.sh` iterates `00-` through `60-` in numeric order, waits
for each stack to reach `CREATE_COMPLETE` / `UPDATE_COMPLETE` before
moving on.

## Open questions still to settle

- Single-account multi-stack vs multi-account (orgs / OUs)?
- StackSet for cross-region replication?
- Use `AWS::LanguageExtensions` (`!Ref` short forms, `!If` chain
  improvements) once we're sure the deploy tooling supports it?
- DNS: own domain via Route53, or CloudFront default?

## Related docs

- [`../CLAUDE.md`](../CLAUDE.md) — server subproject overview
- [`../gun_bot_server/CLAUDE.md`](../gun_bot_server/CLAUDE.md) —
  Go Lambda handlers referenced by these templates
- [`../../design.md`](../../design.md) §4 (cloud infra) and §5
  (DynamoDB) — authoritative spec

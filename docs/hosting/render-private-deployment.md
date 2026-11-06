# Private Render deployment

Status: **deployment configuration only; not deployed or security-tested yet**.

This configuration is intended to host the existing terminal behind HTTP Basic Authentication. It preserves the Windows launcher and the default local-only behavior. It does not add trading, prediction models, or any claim of model performance.

## Before deploying

1. Review and merge the open market-history/CSV pull request (#5) into `main`, or deliberately keep that feature out of the first deployment. This branch was based on `main` and does not silently include the open PR.
2. Review this deployment branch and merge it only after the checks below pass.
3. In Render, create a Blueprint from the repository and select `render.yaml`.
4. Set both `RESOLUTE_AUTH_USER` and `RESOLUTE_AUTH_PASSWORD` in the Render service's Environment settings. Use a unique username and a long, randomly generated password. Never commit their values.
5. Keep the health check at `/api/health`. That one endpoint is intentionally unauthenticated for Render health checks; the terminal page, assets, API routes, and event stream require Basic Auth.
6. Open the HTTPS Render URL in Safari and sign in when prompted. Add it to the Home Screen only after the hosted checks pass.

## Important limitations

- Basic Auth is a minimal single-user access gate, not a full account system. Use a unique high-entropy password and do not reuse another account's password.
- The health endpoint reveals basic service/provider status and is public by design; do not add secrets or sensitive data to it.
- The application currently uses in-memory watchlists/cache and one shared market-data engine. Hosted state is not durable across restarts and multiple users/tabs can affect each other's selected watchlists.
- Yahoo Finance data through `yfinance` is currently documented in this repository as for personal use. Verify provider terms before exposing data to other people or using it commercially.
- A free Render service can sleep or restart and is not a guaranteed always-on market-data feed. Validate current plan limits before relying on it.
- No predictive model, model-training endpoint, research database, or trading execution layer is added by this configuration.

## Required validation before calling it deployed

- Python syntax/import and unit tests pass on the branch.
- Local Windows launcher still runs unchanged.
- Missing hosted credentials cause startup to fail closed.
- `/api/health` returns 2xx without credentials.
- The terminal page, static assets, API routes, and event stream return 401 without valid credentials.
- Correct credentials allow the page and data routes.
- Safari mobile layout, chart rendering, reconnect behavior, and provider rate-limit handling are checked against the deployed HTTPS service.
- No API key or password appears in Git history, logs, browser code, or responses.

# Publishing this monograph (GitHub Pages)

The static site lives in **`site/`** at the repo root. GitHub Actions workflow:

`.github/workflows/pages.yml`

## One-time GitHub settings

1. Repo → **Settings** → **Pages**
2. **Build and deployment** → Source: **GitHub Actions**
3. Push to `master`/`main` (or run the workflow manually under **Actions**)

The workflow:

- Stages `site/` into an artifact
- **Always** copies showcase files from `docs/media` and `docs/images` into the
  artifact (local `site/media` / `site/images` are optional untracked symlinks
  for local preview only — they are **not** required in git)
- Excludes inverse research media
- Fails if cover assets are missing (`media/helmet_orbit.mp4`,
  `images/hero_outdoor_graded.jpg`) or if inverse product strings appear in HTML
- Deploys with `actions/deploy-pages`

Public URL shape (project pages):

`https://<user>.github.io/ohao_engine/`

If the site is served from a project subpath and assets 404, keep using **relative** links (`../styles.css`, `m/…`) — do not switch to root-absolute `/styles.css`.

## Local preview (same as CI root)

```bash
cd site
python3 -m http.server 8765
# open http://127.0.0.1:8765/
```

## Verify publish tree without GitHub

```bash
# same staging idea as the workflow
bash -c 'rm -rf /tmp/ohao_site_stage && mkdir -p /tmp/ohao_site_stage && cp -a site/. /tmp/ohao_site_stage/'
python3 -m http.server 8766 --directory /tmp/ohao_site_stage
```

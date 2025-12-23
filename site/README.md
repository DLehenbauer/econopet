# Site

This directory contains the static website for EconoPET, hosted via GitHub Pages at:

https://dlehenbauer.github.io/econopet

## Deployment

The site references files stored in Git LFS (images, PDFs, and firmware downloads). Because GitHub Pages' default deployment does not check out LFS objects, we use a custom GitHub Actions workflow instead.

The workflow is defined in [`.github/workflows/gh-pages.yml`](../.github/workflows/gh-pages.yml) and must be triggered manually:

1. Go to the [Actions](https://github.com/DLehenbauer/econopet/actions) tab in the GitHub repository
2. Select the **Deploy static content to Pages** workflow from the left sidebar
3. Click **Run workflow** and confirm

The workflow checks out the repository with `lfs: true` to fetch LFS objects, then deploys the contents of `site/dist` to GitHub Pages.

## Links

QR codes linking to the site are printed on the PCB silkscreen. Because space is limited, we use [is.gd](https://is.gd) to shorten URLs. Shorter URLs encode fewer characters, resulting in lower-density QR codes that remain scannable at small sizes with a typical phone camera.

Page  | Short URL
------|----------
[https://dlehenbauer.github.io/econopet](https://dlehenbauer.github.io/econopet) | [https://is.gd/FjBm6R](https://is.gd/FjBm6R)
[https://dlehenbauer.github.io/econopet/40-8096-A.html](https://dlehenbauer.github.io/econopet/40-8096-A.html) | [https://is.gd/6hpvh6](https://is.gd/6hpvh6)

# TSDuck Packages

Place TSDuck `.deb` packages here for offline installation.

## Required Files

Download from: https://github.com/tsduck/tsduck/releases

For **amd64** (Intel/AMD 64-bit):
- `tsduck_3.42-4421.ubuntu24_amd64.deb`

For **arm64** (ARM 64-bit):
- `tsduck_3.42-4421.ubuntu24_arm64.deb`

## How it works

The install script will:
1. First check this `packages/` directory for local `.deb` files
2. If not found, attempt to download from GitHub
3. If download fails, display instructions for manual download

## Note

These packages are not included in the git repository due to their size (~30MB each).
Users should download the appropriate package for their architecture.

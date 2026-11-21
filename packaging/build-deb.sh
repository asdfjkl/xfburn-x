#!/usr/bin/env bash
# Copyright (c) 2026 Dominik Klein
# Build and verify an x86_64 (amd64) Debian/Ubuntu .deb package for Xfburn-X.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${ROOT_DIR}/dist"
BUILDDIR="${ROOT_DIR}/build-deb"
USE_DH="auto"
VERSION_OVERRIDE=""
MAX_GLIBC="2.39"

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --output-dir DIR   Directory for output .deb and SHA256SUMS (default: dist)
  --version VER      Override Debian version string (default: from debian/changelog)
  --dh               Require dpkg-buildpackage/debhelper
  --no-dh            Force direct Meson + dpkg-deb build (no debhelper required)
  -h, --help         Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --output-dir)
      OUT_DIR="$2"
      shift 2
      ;;
    --version)
      VERSION_OVERRIDE="$2"
      shift 2
      ;;
    --dh)
      USE_DH="yes"
      shift
      ;;
    --no-dh)
      USE_DH="no"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

ARCH="$(dpkg --print-architecture)"
if [[ "${ARCH}" != "amd64" ]]; then
  echo "Error: Xfburn-X binary releases are limited to x86_64 (amd64); host is '${ARCH}'." >&2
  exit 1
fi

if [[ -n "${VERSION_OVERRIDE}" ]]; then
  DEB_VERSION="${VERSION_OVERRIDE}"
else
  DEB_VERSION="$(dpkg-parsechangelog -l "${ROOT_DIR}/debian/changelog" -S Version)"
fi

mkdir -p "${OUT_DIR}"
OUT_DEB="${OUT_DIR}/xfburn-x_${DEB_VERSION}_amd64.deb"

should_use_dh=0
if [[ "${USE_DH}" == "yes" ]]; then
  if ! command -v dh >/dev/null 2>&1; then
    echo "Error: --dh requested, but debhelper (dh) is not installed." >&2
    exit 1
  fi
  should_use_dh=1
elif [[ "${USE_DH}" == "auto" ]]; then
  if command -v dh >/dev/null 2>&1 && command -v dpkg-buildpackage >/dev/null 2>&1 && [[ -z "${VERSION_OVERRIDE}" ]]; then
    should_use_dh=1
  fi
fi

if [[ "${should_use_dh}" -eq 1 ]]; then
  echo "==> Building xfburn-x ${DEB_VERSION} (amd64) via dpkg-buildpackage (debhelper)..."
  (
    cd "${ROOT_DIR}"
    dpkg-buildpackage -us -uc -b --no-sign
  )
  PARENT_DIR="$(dirname "${ROOT_DIR}")"
  BUILT_DEB="${PARENT_DIR}/xfburn-x_${DEB_VERSION}_amd64.deb"
  if [[ ! -f "${BUILT_DEB}" ]]; then
    echo "Error: Expected ${BUILT_DEB} not found after dpkg-buildpackage." >&2
    exit 1
  fi
  mv -f "${BUILT_DEB}" "${OUT_DEB}"
  # Clean auxiliary dpkg-buildpackage output artifacts in parent dir
  rm -f "${PARENT_DIR}/xfburn-x-dbgsym_${DEB_VERSION}_amd64.ddeb" \
        "${PARENT_DIR}/xfburn-x_${DEB_VERSION}_amd64.buildinfo" \
        "${PARENT_DIR}/xfburn-x_${DEB_VERSION}_amd64.changes"
else
  echo "==> Building xfburn-x ${DEB_VERSION} (amd64) via Meson + dpkg-shlibdeps + dpkg-deb..."
  STAGE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/xfburn-x-deb-stage.XXXXXX")"
  chmod 0755 "${STAGE_DIR}"
  trap 'rm -rf "${STAGE_DIR}"' EXIT

  rm -rf "${BUILDDIR}"
  meson setup "${BUILDDIR}" "${ROOT_DIR}" \
    --prefix=/usr \
    --buildtype=debugoptimized \
    --wrap-mode=nodownload \
    -Dstrip=true
  meson compile -C "${BUILDDIR}"
  meson test -C "${BUILDDIR}" --verbose
  DESTDIR="${STAGE_DIR}" meson install -C "${BUILDDIR}" --no-rebuild

  # Ensure binary is stripped
  if command -v strip >/dev/null 2>&1; then
    strip --strip-unneeded "${STAGE_DIR}/usr/bin/xfburn-x"
  fi

  # Compress manpage per Debian policy
  if [[ -f "${STAGE_DIR}/usr/share/man/man1/xfburn-x.1" ]]; then
    gzip -9n "${STAGE_DIR}/usr/share/man/man1/xfburn-x.1"
  fi

  # Install copyright and compressed changelog into /usr/share/doc/xfburn-x
  DOC_DIR="${STAGE_DIR}/usr/share/doc/xfburn-x"
  mkdir -p "${DOC_DIR}"
  cp -p "${ROOT_DIR}/debian/copyright" "${DOC_DIR}/copyright"
  gzip -9nc "${ROOT_DIR}/debian/changelog" > "${DOC_DIR}/changelog.gz"

  # Normalize permissions
  find "${STAGE_DIR}/usr" -type d -exec chmod 0755 {} +
  find "${STAGE_DIR}/usr" -type f -exec chmod 0644 {} +
  chmod 0755 "${STAGE_DIR}/usr/bin/xfburn-x"

  # Compute runtime shared-library dependencies from Debian .symbols/.shlibs
  SHLIBS_DEPS="$(
    cd "${ROOT_DIR}"
    dpkg-shlibdeps -O "${STAGE_DIR}/usr/bin/xfburn-x" | sed -n 's/^shlibs:Depends=//p'
  )"
  if [[ -z "${SHLIBS_DEPS}" ]]; then
    echo "Error: dpkg-shlibdeps returned empty shlibs:Depends." >&2
    exit 1
  fi

  INSTALLED_SIZE="$(du -sk "${STAGE_DIR}/usr" | awk '{print $1}')"

  mkdir -p "${STAGE_DIR}/DEBIAN"
  cat > "${STAGE_DIR}/DEBIAN/control" <<EOF
Package: xfburn-x
Version: ${DEB_VERSION}
Architecture: amd64
Maintainer: Dominik Klein <33625-asdfjkl@users.noreply.gitlab.xfce.org>
Installed-Size: ${INSTALLED_SIZE}
Depends: hicolor-icon-theme, ${SHLIBS_DEPS}
Recommends: gstreamer1.0-libav, gstreamer1.0-plugins-base, gstreamer1.0-plugins-good, gstreamer1.0-plugins-ugly, gvfs
Section: misc
Priority: optional
Homepage: https://github.com/asdfjkl/xfburn-x
Description: CD, DVD, and Blu-ray burning GUI with post-burn verification
 Xfburn-X is an enhanced fork of Xfce Xfburn based on the libburnia
 libraries, designed to install cleanly side-by-side with upstream xfburn.
 .
 It can blank/format rewritable discs, burn and create ISO images, burn
 audio CDs, and write data compositions to CD, DVD, and Blu-ray (including
 M-DISC), with built-in per-sector post-burn verification, asynchronous
 non-blocking drive probing and unmounting, a Disc Info inspector dialog,
 and a diagnostic flight-recorder logger.
EOF

  (
    cd "${STAGE_DIR}"
    find usr -type f -print0 | LC_ALL=C sort -z | xargs -0 md5sum > DEBIAN/md5sums
  )
  chmod 0644 "${STAGE_DIR}/DEBIAN/control" "${STAGE_DIR}/DEBIAN/md5sums"

  # Use xz compression for universal compatibility across Ubuntu 24.04/26.04 & Debian
  fakeroot dpkg-deb --build --root-owner-group -Zxz "${STAGE_DIR}" "${OUT_DEB}"
fi

echo ""
echo "==> Verifying ${OUT_DEB}..."

# 1. Check Architecture == amd64
PKG_ARCH="$(dpkg-deb -f "${OUT_DEB}" Architecture)"
if [[ "${PKG_ARCH}" != "amd64" ]]; then
  echo "Error: Package Architecture is '${PKG_ARCH}', expected 'amd64'." >&2
  exit 1
fi

# 2. Check archive root permissions and no collisions with upstream xfburn package paths
CONTENTS="$(dpkg-deb -c "${OUT_DEB}")"
ROOT_PERM="$(echo "${CONTENTS}" | awk '$6 == "./" {print $1; exit}')"
if [[ "${ROOT_PERM}" != "drwxr-xr-x" ]]; then
  echo "Error: Archive root './' has mode '${ROOT_PERM}', expected 'drwxr-xr-x'." >&2
  exit 1
fi
if echo "${CONTENTS}" | grep -qE ' \./usr/bin/xfburn$| \./usr/share/xfburn/| \./usr/share/applications/xfburn\.desktop$| \./usr/share/man/man1/xfburn\.1'; then
  echo "Error: Package contains paths colliding with upstream xfburn!" >&2
  exit 1
fi

# 3. Verify ELF GLIBC symbol ceiling <= 2.39 (Ubuntu 24.04 LTS compatible)
VERIFY_DIR="$(mktemp -d "${TMPDIR:-/tmp}/xfburn-x-deb-verify.XXXXXX")"
dpkg-deb -x "${OUT_DEB}" "${VERIFY_DIR}"
MAX_FOUND_GLIBC="$(
  readelf -V "${VERIFY_DIR}/usr/bin/xfburn-x" \
    | grep -oE 'GLIBC_[0-9]+\.[0-9]+(\.[0-9]+)?' \
    | sed 's/^GLIBC_//' \
    | sort -Vu \
    | tail -n 1
)"
rm -rf "${VERIFY_DIR}"

if [[ -n "${MAX_FOUND_GLIBC}" ]]; then
  echo "    Highest GLIBC symbol required: GLIBC_${MAX_FOUND_GLIBC} (Ubuntu 24.04 ceiling: ${MAX_GLIBC})"
  if dpkg --compare-versions "${MAX_FOUND_GLIBC}" gt "${MAX_GLIBC}"; then
    echo "Error: Binary requires GLIBC_${MAX_FOUND_GLIBC} > ${MAX_GLIBC} (would not run on Ubuntu 24.04 LTS)!" >&2
    exit 1
  fi
fi

# 4. Print package info & write SHA256SUMS
echo ""
dpkg-deb -I "${OUT_DEB}"
echo ""
(
  cd "${OUT_DIR}"
  sha256sum "$(basename "${OUT_DEB}")" > SHA256SUMS
  cat SHA256SUMS
)

if command -v lintian >/dev/null 2>&1; then
  echo ""
  echo "==> Running lintian..."
  lintian --info "${OUT_DEB}" || true
fi

echo ""
echo "==> Success: ${OUT_DEB}"

#!/usr/bin/env node
/**
 * prepare-android-opencode.mjs
 *
 * Downloads the official opencode linux-arm64-musl binary + a static musl loader
 * into android/app/src/main/jniLibs/ as:
 *   arm64-v8a/libopencode.so
 *   arm64-v8a/libmusl-loader.so
 *
 * Android extracts jniLibs to nativeLibraryDir and allows execve only from there,
 * hence the .so names (they are NOT JNI libraries).
 *
 * Usage: node scripts/prepare-android-opencode.mjs [version]
 * Env:   OPENCODE_VERSION=1.18.34  MUSL_LOADER_URL=...
 *        FORCE=1  — re-download even if files already exist
 *
 * Windows-friendly: pure Node walk/extract (no Unix find/chmod/sh).
 */
import { execFileSync, execSync } from 'node:child_process';
import { createWriteStream, existsSync, mkdirSync, renameSync, rmSync, statSync, readdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { pipeline } from 'node:stream/promises';
import { Readable } from 'node:stream';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const abi = 'arm64-v8a';
const jni = join(root, 'android', 'app', 'src', 'main', 'jniLibs', abi);
mkdirSync(jni, { recursive: true });

const version = process.argv[2] || process.env.OPENCODE_VERSION || '';
const pkgName = 'opencode-linux-arm64-musl';
const FORCE = !!process.env.FORCE;

/** Recursive file list (Windows-safe replacement for `find`). */
function walk(dir, out = []) {
  let entries;
  try {
    entries = readdirSync(dir, { withFileTypes: true });
  } catch {
    return out;
  }
  for (const e of entries) {
    const p = join(dir, e.name);
    if (e.isDirectory()) walk(p, out);
    else if (e.isFile()) out.push(p);
  }
  return out;
}

function tarExtract(archive, destDir) {
  mkdirSync(destDir, { recursive: true });
  // Windows 10+ ships bsdtar as tar.exe; works for .tar.gz and Alpine .apk (= tar.gz)
  execFileSync('tar', ['-xzf', archive, '-C', destDir], { stdio: ['ignore', 'pipe', 'pipe'] });
}

async function npmTarball(name, ver) {
  const spec = ver ? `${name}@${ver}` : name;
  const out = execSync(`npm view ${spec} dist.tarball`, { encoding: 'utf8', shell: true }).trim();
  if (!out.startsWith('http')) throw new Error(`no tarball for ${spec}: ${out}`);
  return out;
}

/** Fetch with retries; falls back to curl.exe (Windows) then npm pack. */
async function download(url, dest) {
  console.log(`  ↓ ${url}`);
  let lastErr;
  for (let attempt = 1; attempt <= 4; attempt++) {
    try {
      const res = await fetch(url, {
        signal: AbortSignal.timeout(60_000),
        headers: { 'user-agent': 'nebula-emu-prepare/1.0' },
      });
      if (!res.ok || !res.body) throw new Error(`HTTP ${res.status}`);
      await pipeline(Readable.fromWeb(res.body), createWriteStream(dest));
      if (statSync(dest).size > 0) return;
      throw new Error('empty file after download');
    } catch (e) {
      lastErr = e;
      console.log(`  · attempt ${attempt} failed: ${e?.message ?? e}`);
      rmSync(dest, { force: true });
    }
  }
  // curl.exe fallback (ships with Win10+)
  try {
    console.log('  · fallback: curl.exe');
    execFileSync('curl', ['-L', '--fail', '--retry', '3', '--connect-timeout', '30', '-o', dest, url], {
      stdio: ['ignore', 'ignore', 'pipe'],
    });
    if (statSync(dest).size > 0) return;
  } catch (e) {
    lastErr = e;
    console.log(`  · curl failed: ${e?.message ?? e}`);
  }
  throw new Error(`download failed for ${url}: ${lastErr?.message ?? lastErr}`);
}

function extractBinaryFromTgz(tgz, destSo) {
  // tarball layout: package/bin/opencode (or package/opencode)
  const tmp = tgz + '.d';
  rmSync(tmp, { recursive: true, force: true });
  mkdirSync(tmp, { recursive: true });
  tarExtract(tgz, tmp);

  const files = walk(tmp);
  // prefer package/bin/opencode
  const bin =
    files.find((c) => c.endsWith(join('bin', 'opencode'))) ||
    files.find((c) => /opencode$/.test(c) && !c.endsWith('.exe')) ||
    files[0];
  if (!bin) throw new Error('binary not found in tarball: ' + files.join(', '));

  rmSync(destSo, { force: true });
  renameSync(bin, destSo);
  rmSync(tmp, { recursive: true, force: true });

  const sz = statSync(destSo).size;
  console.log(`  ✓ libopencode.so  ${(sz / 1024 / 1024).toFixed(1)} MB`);
  if (sz < 10_000_000) throw new Error('suspiciously small binary — check extract path');
}

async function main() {
  console.log('[prepare-android-opencode]');

  // 1) official opencode musl arm64 binary
  const destBin = join(jni, 'libopencode.so');
  if (existsSync(destBin) && statSync(destBin).size > 10_000_000 && !FORCE) {
    console.log(`  • libopencode.so already present (${(statSync(destBin).size / 1024 / 1024).toFixed(1)} MB)`);
  } else {
    const url = await npmTarball(pkgName, version);
    const tgz = join(root, 'node_modules', `.oc-${pkgName}.tgz`);
    mkdirSync(dirname(tgz), { recursive: true });
    await download(url, tgz);
    extractBinaryFromTgz(tgz, destBin);
    rmSync(tgz, { force: true });
  }

  // 2) musl loader (aarch64) — ld-musl from a known Alpine package
  const destLoader = join(jni, 'libmusl-loader.so');
  if (existsSync(destLoader) && statSync(destLoader).size > 50_000 && !FORCE) {
    console.log(`  • libmusl-loader.so already present (${statSync(destLoader).size} B)`);
  } else {
    const loaderUrl =
      process.env.MUSL_LOADER_URL ||
      'https://dl-cdn.alpinelinux.org/alpine/latest-stable/aarch64/musl-1.2.5-r2.apk';
    const apk = join(root, 'node_modules', '.musl.apk');
    await download(loaderUrl, apk);
    const tmpDir = apk + '.d';
    rmSync(tmpDir, { recursive: true, force: true });
    try {
      tarExtract(apk, tmpDir);
      const found = walk(tmpDir).find((f) => {
        const base = f.replace(/\\/g, '/').split('/').pop() || '';
        return base.startsWith('ld-musl');
      });
      if (!found) throw new Error('ld-musl not found in Alpine apk');
      rmSync(destLoader, { force: true });
      renameSync(found, destLoader);
      console.log(`  ✓ libmusl-loader.so  ${statSync(destLoader).size} B`);
    } finally {
      rmSync(tmpDir, { recursive: true, force: true });
      rmSync(apk, { force: true });
    }
  }

  console.log('[done] jniLibs/' + abi + ' ready — run: npm run android:sync && cd android && gradlew.bat assembleRelease');
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});

package com.nebula.emu;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.webkit.WebView;

import androidx.activity.result.ActivityResult;

import com.getcapacitor.JSArray;
import com.getcapacitor.JSObject;
import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.PluginMethod;
import com.getcapacitor.annotation.ActivityCallback;
import com.getcapacitor.annotation.CapacitorPlugin;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * On-device REAL opencode — no PC, no Termux, no bridge.
 *
 * The APK ships the official opencode linux-arm64-musl binary and a musl loader
 * as jniLibs (libopencode.so / libmusl-loader.so). Android only allows execve of
 * PIE files from nativeLibraryDir, so both live there and are launched as:
 *
 *   libmusl-loader.so libopencode.so serve --hostname 127.0.0.1 --port 4096
 *
 * The WebView then talks to http://127.0.0.1:4096 (same device, loopback only).
 * Workspace = Android/data/<pkg>/files/workspace (or a user-picked folder).
 */
@CapacitorPlugin(name = "NebulaEmu")
public class OpencodeNativePlugin extends Plugin {

    private static final int OC_PORT = 4096;
    private static final String OC_BASE = "http://127.0.0.1:" + OC_PORT;

    private final ExecutorService io = Executors.newSingleThreadExecutor();
    private java.lang.Process ocProcess;
    private Thread pumpThread;

    private File workspace() {
        File base = getContext().getExternalFilesDir(null);
        if (base == null) base = getContext().getFilesDir();
        File ws = new File(base, "workspace");
        if (!ws.exists()) {
            ws.mkdirs();
            try (FileOutputStream out = new FileOutputStream(new File(ws, "README.md"))) {
                out.write(("# Nebula Emu workspace\n\nPhone path: " + ws.getAbsolutePath()
                        + "\nReal opencode serve runs in this app and edits files here.\n")
                        .getBytes(StandardCharsets.UTF_8));
            } catch (IOException ignored) { }
        }
        return ws;
    }

    private File dataDir() {
        File d = new File(getContext().getFilesDir(), "opencode");
        if (!d.exists()) d.mkdirs();
        return d;
    }

    @PluginMethod
    public void info(PluginCall call) {
        JSObject r = new JSObject();
        r.put("platform", "android");
        r.put("workspace", workspace().getAbsolutePath());
        r.put("data", dataDir().getAbsolutePath());
        r.put("abi", Build.SUPPORTED_ABIS.length > 0 ? Build.SUPPORTED_ABIS[0] : "unknown");
        r.put("sdk", Build.VERSION.SDK_INT);
        r.put("base", OC_BASE);
        r.put("allFilesAccess", hasAllFilesAccess());
        try {
            if (Build.VERSION.SDK_INT >= 26) {
                android.content.pm.PackageInfo wv = WebView.getCurrentWebViewPackage();
                if (wv != null) r.put("webview", wv.versionName);
            }
        } catch (Exception ignored) { }
        call.resolve(r);
    }

    private boolean hasAllFilesAccess() {
        if (Build.VERSION.SDK_INT < 30) return true;
        try {
            return Environment.isExternalStorageManager();
        } catch (Throwable t) {
            return false;
        }
    }

    /** Open system screen «All files access» for this app. */
    @PluginMethod
    public void openStorageSettings(PluginCall call) {
        try {
            Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
            intent.setData(Uri.parse("package:" + getContext().getPackageName()));
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            getActivity().startActivity(intent);
            JSObject r = new JSObject();
            r.put("ok", true);
            call.resolve(r);
        } catch (Exception e) {
            try {
                Intent fallback = new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION);
                fallback.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                getActivity().startActivity(fallback);
                JSObject r = new JSObject();
                r.put("ok", true);
                call.resolve(r);
            } catch (Exception e2) {
                call.reject("storage settings: " + e2);
            }
        }
    }

    /**
     * SAF folder picker → real filesystem path (primary/secondary volume).
     * Requires All files access for opencode to read/write the path outside app dirs.
     */
    @PluginMethod
    public void pickDirectory(PluginCall call) {
        try {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            intent.addFlags(
                    Intent.FLAG_GRANT_READ_URI_PERMISSION
                            | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                            | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
                            | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
            if (Build.VERSION.SDK_INT >= 26) {
                intent.putExtra("android.content.extra.SHOW_ADVANCED", true);
            }
            // start in external storage root when possible
            if (Build.VERSION.SDK_INT >= 26) {
                // DocumentsContract.EXTRA_INITIAL_URI = "android.content.extra.INITIAL_URI"
                intent.putExtra("android.content.extra.INITIAL_URI",
                        Uri.parse("content://com.android.externalstorage.documents/document/primary%3A"));
            }
            startActivityForResult(call, intent, "onDirectoryPicked");
        } catch (Exception e) {
            call.reject("pickDirectory: " + e);
        }
    }

    @ActivityCallback
    public void onDirectoryPicked(PluginCall call, ActivityResult result) {
        JSObject r = new JSObject();
        Intent data = result.getData();
        Uri uri = data != null ? data.getData() : null;
        if (result.getResultCode() != Activity.RESULT_OK || uri == null) {
            r.put("cancelled", true);
            call.resolve(r);
            return;
        }
        try {
            int takeFlags = Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
            getContext().getContentResolver().takePersistableUriPermission(uri, takeFlags);
        } catch (Exception ignored) { /* some providers disallow persist */ }

        String path = treeUriToPath(uri);
        String name = lastSegment(uri);
        if (path == null) {
            r.put("error", "cannot resolve path for " + uri);
            r.put("uri", uri.toString());
            r.put("name", name);
            call.resolve(r);
            return;
        }
        r.put("path", path);
        r.put("name", name);
        r.put("uri", uri.toString());
        r.put("allFilesAccess", hasAllFilesAccess());
        call.resolve(r);
    }

    private static String lastSegment(Uri uri) {
        String p = uri.getLastPathSegment();
        if (p == null) return "folder";
        int colon = p.indexOf(':');
        String leaf = colon >= 0 ? p.substring(colon + 1) : p;
        int slash = leaf.lastIndexOf('/');
        if (slash >= 0 && slash < leaf.length() - 1) leaf = leaf.substring(slash + 1);
        return leaf.isEmpty() ? "folder" : leaf;
    }

    /**
     * content://…/tree/primary%3ADownload%2Fproj → /storage/emulated/0/Download/proj
     * content://…/tree/ABCD-1234%3A… → /storage/ABCD-1234/…
     */
    static String treeUriToPath(Uri uri) {
        if (uri == null) return null;
        String auth = uri.getAuthority();
        if (auth == null || !auth.contains("externalstorage")) return null;
        String tree = null;
        try {
            tree = DocumentsContract.getTreeId(uri);
        } catch (Throwable ignored) { }
        if (tree == null || tree.isEmpty()) {
            String s = uri.toString();
            int i = s.indexOf("/tree/");
            if (i < 0) return null;
            tree = Uri.decode(s.substring(i + 6));
        }
        int colon = tree.indexOf(':');
        if (colon < 0) return null;
        String vol = tree.substring(0, colon);
        String rel = tree.substring(colon + 1);
        String root;
        if ("primary".equalsIgnoreCase(vol)) {
            root = Environment.getExternalStorageDirectory().getAbsolutePath();
        } else if ("home".equalsIgnoreCase(vol)) {
            root = new File(Environment.getExternalStorageDirectory(), "Download").getAbsolutePath();
        } else {
            root = "/storage/" + vol;
        }
        if (rel == null || rel.isEmpty() || rel.equals(".")) return root;
        // rel may contain nested paths
        return root + "/" + rel;
    }

    /** List a real directory (or common roots when path empty). */
    @PluginMethod
    public void listDirectory(PluginCall call) {
        String path = call.getString("path", "");
        io.execute(() -> {
            JSObject r = new JSObject();
            try {
                File dir;
                if (path == null || path.isEmpty()) {
                    JSArray entries = new JSArray();
                    List<File> roots = new ArrayList<>();
                    File ext = getContext().getExternalFilesDir(null);
                    if (ext != null) roots.add(ext);
                    roots.add(getContext().getFilesDir());
                    roots.add(Environment.getExternalStorageDirectory());
                    File sd = new File("/storage");
                    if (sd.isDirectory()) {
                        File[] vols = sd.listFiles();
                        if (vols != null) roots.addAll(Arrays.asList(vols));
                    }
                    for (File f : roots) {
                        if (f == null || !f.isDirectory()) continue;
                        JSObject e = new JSObject();
                        e.put("path", f.getAbsolutePath());
                        e.put("name", f.getName().isEmpty() ? f.getAbsolutePath() : f.getName());
                        e.put("dir", true);
                        entries.put(e);
                    }
                    r.put("entries", entries);
                    r.put("path", "/");
                    call.resolve(r);
                    return;
                }
                dir = new File(path);
                if (!dir.exists()) {
                    r.put("error", "no such path: " + path);
                    r.put("entries", new JSArray());
                    r.put("path", path);
                    call.resolve(r);
                    return;
                }
                if (!dir.isDirectory()) {
                    r.put("error", "not a directory: " + path);
                    r.put("entries", new JSArray());
                    r.put("path", path);
                    call.resolve(r);
                    return;
                }
                File[] children = dir.listFiles();
                JSArray entries = new JSArray();
                if (children != null) {
                    Arrays.sort(children, new Comparator<File>() {
                        @Override
                        public int compare(File a, File b) {
                            if (a.isDirectory() != b.isDirectory()) return a.isDirectory() ? -1 : 1;
                            return a.getName().compareToIgnoreCase(b.getName());
                        }
                    });
                    for (File f : children) {
                        // skip huge/system noise
                        String n = f.getName();
                        if (n.startsWith(".") && !n.equals("..")) continue;
                        JSObject e = new JSObject();
                        e.put("path", f.getAbsolutePath());
                        e.put("name", n);
                        e.put("dir", f.isDirectory());
                        entries.put(e);
                    }
                }
                r.put("entries", entries);
                r.put("path", dir.getAbsolutePath());
                r.put("allFilesAccess", hasAllFilesAccess());
                call.resolve(r);
            } catch (Exception e) {
                r.put("error", String.valueOf(e));
                r.put("entries", new JSArray());
                r.put("path", path == null ? "" : path);
                call.resolve(r);
            }
        });
    }

    /** Absolute path of an extracted/installed native lib. */
    private File nativeLib(String name) {
        // applicationInfo.nativeLibraryDir contains lib*.so extracted from the APK
        String dir = getContext().getApplicationInfo().nativeLibraryDir;
        if (dir == null) return null;
        File f = new File(dir, name);
        return f.exists() ? f : null;
    }

    @PluginMethod
    public void start(PluginCall call) {
        io.execute(() -> {
            JSObject out = new JSObject();
            try {
                if (isHealthy()) {
                    out.put("ok", true);
                    out.put("already", true);
                    out.put("base", OC_BASE);
                    publishReady(out);
                    call.resolve(out);
                    return;
                }
                File loader = nativeLib("libmusl-loader.so");
                File bin = nativeLib("libopencode.so");
                if (loader == null || bin == null) {
                    out.put("ok", false);
                    out.put("error", "bundled opencode missing (libmusl-loader.so / libopencode.so). Run npm run android:prepare-opencode && npx cap sync.");
                    call.resolve(out);
                    return;
                }
                // ensure executable bits (extracted libs are usually already +x)
                loader.setExecutable(true, false);
                bin.setExecutable(true, false);

                File data = dataDir();
                File logFile = new File(data, "opencode.log");

                ProcessBuilder pb = new ProcessBuilder(
                        loader.getAbsolutePath(),
                        bin.getAbsolutePath(),
                        "serve",
                        "--hostname", "127.0.0.1",
                        "--port", String.valueOf(OC_PORT)
                );
                pb.directory(workspace());
                pb.redirectErrorStream(true);
                pb.redirectOutput(logFile);
                java.util.Map<String, String> env = pb.environment();
                env.put("HOME", data.getAbsolutePath());
                env.put("TMPDIR", new File(data, "tmp").getAbsolutePath());
                env.put("OPENCODE_HOME", data.getAbsolutePath());
                new File(data, "tmp").mkdirs();

                ocProcess = pb.start();
                // lower oom-adj-ish niceness so the phantom process killer is less eager
                try {
                    android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_BACKGROUND);
                } catch (Exception ignored) { }

                // wait for health (up to 90s — first boot may migrate db)
                long deadline = System.currentTimeMillis() + 90_000;
                while (System.currentTimeMillis() < deadline) {
                    // process died before becoming healthy → fail fast
                    if (ocProcess != null && !ocAlive(ocProcess)) break;
                    if (isHealthy()) {
                        out.put("ok", true);
                        out.put("base", OC_BASE);
                        publishReady(out);
                        call.resolve(out);
                        return;
                    }
                    try { Thread.sleep(400); } catch (InterruptedException ie) {
                        Thread.currentThread().interrupt();
                        break;
                    }
                }
                String tail = tail(logFile, 4000);
                out.put("ok", isHealthy());
                out.put("base", OC_BASE);
                if (!out.getBoolean("ok", false)) {
                    out.put("error", "opencode serve did not become healthy. Log tail:\n" + tail);
                }
                publishReady(out);
                call.resolve(out);
            } catch (Exception e) {
                out.put("ok", false);
                out.put("error", String.valueOf(e));
                call.resolve(out);
            }
        });
    }

    private void publishReady(JSObject payload) {
        // expose base URL to the page
        getActivity().runOnUiThread(() -> {
            try {
                JSObject ev = new JSObject();
                ev.put("base", OC_BASE);
                ev.put("ok", payload.getBoolean("ok", false));
                notifyListeners("opencodeReady", ev, false);
                WebView wv = getBridge() != null ? getBridge().getWebView() : null;
                if (wv != null) {
                    wv.evaluateJavascript(
                            "window.__NEBULA_OC_BASE=" + jsonStr(OC_BASE) + ";"
                                    + "window.dispatchEvent(new CustomEvent('nebula-opencode',{detail:{base:"
                                    + jsonStr(OC_BASE) + ",ok:" + payload.getBoolean("ok", false) + "}}));",
                            null);
                }
            } catch (Exception ignored) { }
        });
    }

    private static String jsonStr(String s) {
        return "\"" + s.replace("\\", "\\\\").replace("\"", "\\\"") + "\"";
    }

    private boolean isHealthy() {
        HttpURLConnection conn = null;
        try {
            URL u = new URL(OC_BASE + "/global/health");
            conn = (HttpURLConnection) u.openConnection();
            conn.setConnectTimeout(800);
            conn.setReadTimeout(800);
            int code = conn.getResponseCode();
            if (code != 200) return false;
            try (InputStream in = conn.getInputStream()) {
                byte[] buf = new byte[256];
                int n = in.read(buf);
                String body = n > 0 ? new String(buf, 0, n, StandardCharsets.UTF_8) : "";
                return !body.contains("\"healthy\":false");
            }
        } catch (Exception e) {
            return false;
        } finally {
            if (conn != null) conn.disconnect();
        }
    }

    private static String tail(File f, int max) {
        if (f == null || !f.exists()) return "";
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new java.io.FileInputStream(f), StandardCharsets.UTF_8))) {
            StringBuilder all = new StringBuilder();
            String line;
            while ((line = r.readLine()) != null) {
                all.append(line).append('\n');
            }
            int s = Math.max(0, all.length() - max);
            return all.substring(s);
        } catch (IOException e) {
            return "";
        }
    }

    @PluginMethod
    public void stop(PluginCall call) {
        io.execute(() -> {
            try {
                if (ocProcess != null) {
                    ocProcess.destroy();
                    ocProcess = null;
                }
            } catch (Exception ignored) { }
            JSObject r = new JSObject();
            r.put("ok", true);
            call.resolve(r);
        });
    }

    @PluginMethod
    public void status(PluginCall call) {
        io.execute(() -> {
            JSObject r = new JSObject();
            boolean healthy = isHealthy();
            r.put("healthy", healthy);
            r.put("base", OC_BASE);
            r.put("running", ocProcess != null);
            r.put("pid", ocProcess != null ? getPid(ocProcess) : -1);
            call.resolve(r);
        });
    }

    /** true while the process has not exited yet (exitValue throws if still running). */
    private static boolean ocAlive(java.lang.Process p) {
        try {
            p.exitValue();
            return false;
        } catch (IllegalThreadStateException stillRunning) {
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    private static int getPid(java.lang.Process p) {
        try {
            Class<?> c = Class.forName("java.lang.Process");
            java.lang.reflect.Method m = c.getMethod("pid");
            Object v = m.invoke(p);
            return v instanceof Long ? (int) (long) (Long) v : -1;
        } catch (Exception e) {
            return -1;
        }
    }

    /** Simple HTTP GET through native (no CORS) — optional helper for the page. */
    @PluginMethod
    public void httpGet(PluginCall call) {
        String path = call.getString("path", "/global/health");
        io.execute(() -> {
            HttpURLConnection conn = null;
            JSObject r = new JSObject();
            try {
                conn = (HttpURLConnection) new URL(OC_BASE + path).openConnection();
                conn.setConnectTimeout(5000);
                conn.setReadTimeout(15000);
                int code = conn.getResponseCode();
                StringBuilder sb = new StringBuilder();
                try (InputStream in = code >= 400 ? conn.getErrorStream() : conn.getInputStream()) {
                    if (in != null) {
                        byte[] buf = new byte[8192];
                        int n;
                        while ((n = in.read(buf)) > 0) sb.append(new String(buf, 0, n, StandardCharsets.UTF_8));
                    }
                }
                r.put("status", code);
                r.put("body", sb.toString());
                call.resolve(r);
            } catch (Exception e) {
                r.put("status", 0);
                r.put("body", "");
                r.put("error", String.valueOf(e));
                call.resolve(r);
            } finally {
                if (conn != null) conn.disconnect();
            }
        });
    }
}

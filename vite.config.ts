import { defineConfig } from 'vite';

// Desktop dev: Vite proxies /api/opencode → the Bun bridge (npm run opencode).
// Android APK: the bridge is replaced by the in-app opencode serve on 127.0.0.1 (see android/).
export default defineConfig({
  server: {
    host: true,
    proxy: {
      '/api/opencode': { target: 'http://127.0.0.1:8787', changeOrigin: true },
    },
  },
  preview: {
    host: true,
    proxy: {
      '/api/opencode': { target: 'http://127.0.0.1:8787', changeOrigin: true },
    },
  },
  build: {
    target: 'es2022',
    sourcemap: false,
  },
});

import type { CapacitorConfig } from '@capacitor/cli';

// Android wrapper: WebView loads dist/, the in-app opencode serve runs on 127.0.0.1:4096.
// No PC bridge, no Termux — the real opencode binary ships inside the APK (jniLibs).
const config: CapacitorConfig = {
  appId: 'com.nebula.emu',
  appName: 'Nebula Emu',
  webDir: 'dist',
  server: {
    androidScheme: 'https',
    cleartext: true, // loopback http://127.0.0.1:4096 from the https WebView origin
  },
  android: {
    allowMixedContent: true,
    backgroundColor: '#0b1220',
  },
};

export default config;

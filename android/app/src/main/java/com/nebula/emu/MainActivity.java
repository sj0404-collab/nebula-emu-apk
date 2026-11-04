package com.nebula.emu;

import android.os.Bundle;

import com.getcapacitor.BridgeActivity;

public class MainActivity extends BridgeActivity {
    @Override
    public void onCreate(Bundle savedInstanceState) {
        // real on-device opencode serve (bundled musl binary) — no PC bridge
        registerPlugin(OpencodeNativePlugin.class);
        super.onCreate(savedInstanceState);
    }
}

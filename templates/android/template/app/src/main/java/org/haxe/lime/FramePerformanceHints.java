package org.haxe.lime;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.PerformanceHintManager;
import android.util.Log;

final class FramePerformanceHints {
    private static final String TAG = "NovaFlareADPF";

    private static native void nativeSetEnabled(boolean enabled, boolean diagnostic);

    static void setEnabled(boolean enabled) {
        nativeSetEnabled(enabled, false);
    }

    static boolean start(Context context, int gameThread, int renderThread, long targetNanos) {
        if (Build.VERSION.SDK_INT < 31) {
            Log.i(TAG, "unavailable on Android API " + Build.VERSION.SDK_INT);
            return false;
        }
        return Api31.start(context, gameThread, renderThread, targetNanos);
    }

    static boolean report(long actualNanos, long targetNanos) {
        return Api31.report(actualNanos, targetNanos);
    }

    static void stop() {
        Api31.stop();
    }

    static boolean isDiagnostic(Context context) {
        return context instanceof Activity && context.getPackageName().endsWith(".renderdiagnostics");
    }

    static void configureDiagnostics(GameActivity activity) {
        if (!isDiagnostic(activity)) return;
        Intent intent = activity.getIntent();
        nativeSetEnabled(intent.getBooleanExtra("nova_adpf", false), true);
        GameActivity.nativeSetenv("NOVAFLARE_ANDROID_DIAGNOSTICS", "1");
        String song = intent.getStringExtra("nova_song");
        if (song != null && song.matches("[A-Za-z0-9_-]+")) {
            GameActivity.nativeSetenv("NOVAFLARE_DIAGNOSTIC_SONG", song);
            GameActivity.nativeSetenv("NOVAFLARE_DIAGNOSTIC_DIFFICULTY", "2");
            GameActivity.nativeSetenv("NOVAFLARE_DIAGNOSTIC_MOD", "");
            GameActivity.nativeSetenv("NOVAFLARE_DIAGNOSTIC_BOTPLAY", "1");
        }
        String resolution = intent.getStringExtra("nova_resolution");
        if (resolution != null && resolution.matches("native|[1-9][0-9]{0,3}x[1-9][0-9]{0,3}")) {
            GameActivity.nativeSetenv("NOVAFLARE_RENDER_RESOLUTION", resolution);
        }
        GameActivity.nativeSetenv("NOVAFLARE_RENDER_TARGET_TRACE", "1");
    }

    private static final class Api31 {
        private static PerformanceHintManager.Session session;
        private static long target;
        private static boolean firstReport;

        static boolean start(Context context, int gameThread, int renderThread, long targetNanos) {
            try {
                PerformanceHintManager manager = context.getSystemService(PerformanceHintManager.class);
                long preferred = manager == null ? -1 : manager.getPreferredUpdateRateNanos();
                if (preferred <= 0) {
                    Log.i(TAG, "unavailable preferred_rate_ns=" + preferred);
                    return false;
                }
                int[] tids = renderThread > 0 ? new int[] {gameThread, renderThread} : new int[] {gameThread};
                session = manager.createHintSession(tids, targetNanos);
                if (session == null) {
                    Log.i(TAG, "session creation unavailable");
                    return false;
                }
                target = targetNanos;
                firstReport = true;
                Log.i(TAG, "session started game_tid=" + gameThread + " render_tid=" + renderThread + " target_ns=" + target);
                return true;
            } catch (RuntimeException error) {
                Log.w(TAG, "session creation failed", error);
                return false;
            }
        }

        static boolean report(long actualNanos, long targetNanos) {
            try {
                if (target != targetNanos) {
                    session.updateTargetWorkDuration(targetNanos);
                    target = targetNanos;
                }
                session.reportActualWorkDuration(actualNanos);
                if (firstReport) {
                    firstReport = false;
                    Log.i(TAG, "first report actual_ns=" + actualNanos + " target_ns=" + target);
                }
                return true;
            } catch (RuntimeException error) {
                Log.w(TAG, "session reporting failed", error);
                return false;
            }
        }

        static void stop() {
            if (session == null) return;
            try {
                session.close();
            } catch (RuntimeException error) {
                Log.w(TAG, "session close failed", error);
            } finally {
                session = null;
            }
            Log.i(TAG, "session closed");
        }
    }
}

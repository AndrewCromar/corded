package org.corded.corded_app

import android.content.ComponentName
import android.content.pm.PackageManager
import io.flutter.embedding.android.FlutterFragmentActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

// A fragment activity, because the fingerprint prompt needs one.
class MainActivity : FlutterFragmentActivity() {
    // The launcher entries declared in the manifest, one per icon colour.
    private val icons = listOf("indigo", "forest", "black", "plum", "teal")

    private fun entry(name: String) =
        ComponentName(this, "org.corded.corded_app.Icon" + name.replaceFirstChar { it.uppercase() })

    private fun isOn(name: String): Boolean =
        when (packageManager.getComponentEnabledSetting(entry(name))) {
            PackageManager.COMPONENT_ENABLED_STATE_ENABLED -> true
            PackageManager.COMPONENT_ENABLED_STATE_DISABLED -> false
            else -> name == "indigo" // as the manifest says
        }

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, "org.corded.app/icon").setMethodCallHandler { call, result ->
            when (call.method) {
                "get" -> result.success(icons.firstOrNull { isOn(it) } ?: "indigo")
                "set" -> {
                    val wanted = call.arguments as? String
                    if (wanted == null || wanted !in icons) {
                        result.error("unknown", "no such icon", null)
                    } else {
                        // On first, then the others off, so there is never a moment with no entry.
                        packageManager.setComponentEnabledSetting(
                            entry(wanted), PackageManager.COMPONENT_ENABLED_STATE_ENABLED, PackageManager.DONT_KILL_APP)
                        for (other in icons) if (other != wanted) {
                            packageManager.setComponentEnabledSetting(
                                entry(other), PackageManager.COMPONENT_ENABLED_STATE_DISABLED, PackageManager.DONT_KILL_APP)
                        }
                        result.success(wanted)
                    }
                }
                else -> result.notImplemented()
            }
        }
    }
}

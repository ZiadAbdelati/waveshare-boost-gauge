package com.boostgauge.app.ui.components

import android.annotation.SuppressLint
import android.graphics.Color
import android.util.Log
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Modifier
import androidx.compose.ui.viewinterop.AndroidView
import com.boostgauge.app.data.api.ApiJson
import com.boostgauge.app.data.api.Config
import com.boostgauge.app.data.api.Status
import com.boostgauge.app.data.api.ThemeInfo
import com.boostgauge.app.data.api.ThemesPayload
import java.io.ByteArrayInputStream
import kotlinx.serialization.encodeToString

/**
 * Runs the repository's canonical web/app.js canvas renderer unchanged. The
 * Android layer only supplies the live API payloads and crops the web cockpit
 * down to its 466 x 466 gauge face.
 */
@SuppressLint("SetJavaScriptEnabled")
@Composable
fun CanonicalGaugePreview(
    status: Status,
    config: Config,
    themes: ThemesPayload?,
    theme: ThemeInfo? = null,
    forceTpmsPage: Boolean = false,
    modifier: Modifier = Modifier,
) {
    val json = ApiJson.json
    val selected = theme ?: themes?.themes?.firstOrNull { it.id == status.activeThemeId }
    val script = """
        (() => {
          if (typeof drawGauge !== 'function' || !window.__androidPreviewReady) return;
          const sample = ${json.encodeToString(status)};
          const config = ${json.encodeToString(config)};
          const themesPayload = ${themes?.let { json.encodeToString(it) } ?: "null"};
          const selectedTheme = ${selected?.let { json.encodeToString(it) } ?: "null"};
          state.config = config;
          if (themesPayload) {
            state.themes = themesPayload.themes || [];
            for (const [key, value] of Object.entries(themesPayload)) {
              if (key !== 'themes' && key !== 'activeThemeId') state[key] = value;
            }
          }
          if (selectedTheme) setTheme(selectedTheme);
          state.activePage = ${if (forceTpmsPage) 1 else 0};
          state.tpms = sample.tpms || { status: 2, wheels: [] };
          state.gaugeTarget = sample;
          state.gaugePsi = Number(sample.psi || 0);
          drawGauge(sample);
          return { width: el.canvas.width, height: el.canvas.height };
        })();
    """.trimIndent()
    val latestScript by rememberUpdatedState(script)

    AndroidView(
        modifier = modifier,
        factory = { context ->
            WebView(context).apply {
                setBackgroundColor(Color.BLACK)
                settings.javaScriptEnabled = true
                settings.allowFileAccess = true
                settings.allowContentAccess = false
                settings.blockNetworkLoads = true
                isVerticalScrollBarEnabled = false
                isHorizontalScrollBarEnabled = false
                webViewClient = object : WebViewClient() {
                    override fun shouldInterceptRequest(
                        view: WebView,
                        request: WebResourceRequest,
                    ): WebResourceResponse? {
                        val name = request.url.pathSegments.lastOrNull() ?: return null
                        if (name !in CANONICAL_ASSETS) return null
                        val mime = when {
                            name.endsWith(".css") -> "text/css"
                            name.endsWith(".js") -> "application/javascript"
                            name.endsWith(".png") -> "image/png"
                            name.endsWith(".ttf") -> "font/ttf"
                            else -> "text/html"
                        }
                        val stream = if (name == "app.js") {
                            val offline = context.assets.open(name).bufferedReader().use { it.readText() }
                                .replace(
                                    "refreshAll(ERR_LIVE).finally(connectEvents);",
                                    "/* Native offline mirror: no API or WebSocket bootstrap. */",
                                )
                                .replace(
                                    "const TPMS_POWERTRAIN_SRC = \"/tpms_powertrain.png\";",
                                    "const TPMS_POWERTRAIN_SRC = \"tpms_powertrain.png\";",
                                )
                            ByteArrayInputStream(offline.toByteArray())
                        } else {
                            context.assets.open(name)
                        }
                        return WebResourceResponse(mime, null, stream)
                    }

                    override fun onPageFinished(view: WebView, url: String) {
                        fun prepare(attempt: Int = 0) {
                            view.post {
                                if ((view.width <= 0 || view.height <= 0) && attempt < 10) {
                                    view.postDelayed({ prepare(attempt + 1) }, 50)
                                    return@post
                                }
                                val density = view.resources.displayMetrics.density.coerceAtLeast(1f)
                                val cssWidth = (view.width / density).coerceAtLeast(1f)
                                val cssHeight = (view.height / density).coerceAtLeast(1f)
                                val hostScript = PREPARE_CANONICAL_CANVAS
                                    .replace("__WIDTH__", cssWidth.toString())
                                    .replace("__HEIGHT__", cssHeight.toString())
                                view.evaluateJavascript(hostScript) { prepared ->
                                    Log.d(TAG, "canonical canvas prepared: $prepared")
                                    view.evaluateJavascript(latestScript) { rendered ->
                                        Log.d(TAG, "canonical canvas rendered: $rendered")
                                    }
                                }
                            }
                        }
                        prepare()
                        // app.js sizes the backing canvas asynchronously; the
                        // first drawGauge can land on a 0-size canvas that a
                        // later resize clears (black face). Redraw at the final
                        // backing size a couple of times to make the first paint
                        // deterministic.
                        view.postDelayed({ view.evaluateJavascript(latestScript, null) }, 400)
                        view.postDelayed({ view.evaluateJavascript(latestScript, null) }, 1200)
                    }
                }
                loadUrl("file:///android_asset/index.html")
            }
        },
        update = { it.evaluateJavascript(latestScript, null) },
    )
}

private val CANONICAL_ASSETS = setOf(
    "index.html", "app.js", "styles.css", "tpms_powertrain.png", "doto.ttf",
)

private const val TAG = "CanonicalGaugePreview"

private const val PREPARE_CANONICAL_CANVAS = """
(() => {
  const device = document.getElementById('gaugeDevice');
  const canvas = document.getElementById('gaugeCanvas');
  if (!device || !canvas) return;
  document.body.replaceChildren(device);
  document.documentElement.style.cssText = 'margin:0;background:#000;overflow:hidden;width:__WIDTH__px;height:__HEIGHT__px';
  document.body.style.cssText = 'margin:0;background:#000;overflow:hidden;width:__WIDTH__px;height:__HEIGHT__px';
  device.style.cssText = 'width:__WIDTH__px;height:__HEIGHT__px;max-width:none;aspect-ratio:auto;border:0;border-radius:0;box-shadow:none';
  canvas.style.cssText = 'display:block;width:100%;height:100%';
  /* The canonical page normally owns live HTTP/WS polling. Native code owns
     transport here, so retire those loops after their startup finally block. */
  const stopNativeOwnedPolling = () => {
    if (typeof stopPolling === 'function') stopPolling();
    if (state.reconnectTimer) { clearTimeout(state.reconnectTimer); state.reconnectTimer = null; }
    if (state.heartbeatTimer) { clearInterval(state.heartbeatTimer); state.heartbeatTimer = null; }
    if (typeof calUi !== 'undefined' && calUi.pollTimer) { clearInterval(calUi.pollTimer); calUi.pollTimer = null; }
    if (typeof ambientUi !== 'undefined' && ambientUi.pollTimer) { clearInterval(ambientUi.pollTimer); ambientUi.pollTimer = null; }
  };
  stopNativeOwnedPolling();
  setTimeout(stopNativeOwnedPolling, 1000);
  window.__androidPreviewReady = true;
  return { width: device.clientWidth, height: device.clientHeight };
})();
"""

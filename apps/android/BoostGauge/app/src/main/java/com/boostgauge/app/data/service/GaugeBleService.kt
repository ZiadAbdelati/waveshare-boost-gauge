package com.boostgauge.app.data.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat

/**
 * Foreground service that holds the BLE GATT session while the gauge is
 * connected. Exempts the app from suspension when the screen turns off so
 * the link does not drop with HCI 0x13 REMOTE USER TERMINATED (the phone
 * closing the link after background GATT traffic stalls through the
 * 5-attempt request retry window).
 *
 * Started on successful BLE connect, stopped on explicit user disconnect.
 */
class GaugeBleService : Service() {

    override fun onCreate() {
        super.onCreate()
        createChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf()
                return START_NOT_STICKY
            }
            else -> {
                val name = intent?.getStringExtra(EXTRA_NAME)?.takeIf { it.isNotBlank() } ?: "BoostGauge"
                val notification = buildNotification(name)
                startForeground(NOTIF_ID, notification)
                return START_STICKY
            }
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun createChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "BoostGauge BLE",
                NotificationManager.IMPORTANCE_LOW,
            ).apply {
                description = "Keeps the gauge BLE link alive"
                setShowBadge(false)
            }
            val nm = getSystemService(NotificationManager::class.java)
            nm.createNotificationChannel(channel)
        }
    }

    private fun buildNotification(deviceName: String): Notification {
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("BoostGauge connected")
            .setContentText("Connected to $deviceName")
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setOngoing(true)
            .setCategory(Notification.CATEGORY_SERVICE)
            .build()
    }

    companion object {
        const val CHANNEL_ID = "boostgauge_ble"
        const val NOTIF_ID = 1001
        const val EXTRA_NAME = "device_name"
        const val ACTION_START = "com.boostgauge.app.action.START_BLE"
        const val ACTION_STOP = "com.boostgauge.app.action.STOP_BLE"

        fun start(context: Context, deviceName: String) {
            val intent = Intent(context, GaugeBleService::class.java).apply {
                action = ACTION_START
                putExtra(EXTRA_NAME, deviceName)
            }
            // startForegroundService is required for foregroundServiceType="connectedDevice"
            context.startForegroundService(intent)
        }

        fun stop(context: Context) {
            val intent = Intent(context, GaugeBleService::class.java).apply {
                action = ACTION_STOP
            }
            context.startService(intent)
        }
    }
}

/** Abstraction so TransportController / Repository can be unit-tested without a Context. */
interface ForegroundServiceLauncher {
    fun startBleService(name: String)
    fun stopBleService()
}

class RealForegroundServiceLauncher(private val context: Context) : ForegroundServiceLauncher {
    override fun startBleService(name: String) = GaugeBleService.start(context.applicationContext, name)
    override fun stopBleService() = GaugeBleService.stop(context.applicationContext)
}

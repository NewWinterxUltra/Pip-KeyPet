package com.novikov.piplink

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import java.util.TimeZone

/** Keeps the BLE link, the call monitor and the music monitor alive in the background. */
class PipService : Service() {
    companion object {
        const val TAG = "PipService"
        const val CHANNEL = "pip_link"
        private const val NOTIF = 7

        @Volatile
        var instance: PipService? = null

        @Volatile
        var state = "не подключено"

        /** phone.h PHONE_PRIV_*: 2 = off (nothing is sent), 1 = hidden, 0 = open. */
        @Volatile
        var privacy = 1
    }

    private var ble: BleClient? = null
    private var calls: CallMonitor? = null
    private var media: MediaMonitor? = null
    private val handler = Handler(Looper.getMainLooper())

    private val ticker = object : Runnable {
        private var n = 0
        override fun run() {
            if (ble?.ready == true) {
                sendLine("PING")
                if (n % 2 == 0) sendTime()
            }
            updateNotification()
            n++
            handler.postDelayed(this, 30000)
        }
    }

    override fun onCreate() {
        super.onCreate()
        instance = this
        createChannel()
        startForegroundNow()
        ble = BleClient(this, ::onLine) { sendTime() }
        ble?.start()
        calls = CallMonitor(this, ::sendLine).also { it.start() }
        media = MediaMonitor(this, ::sendLine).also { it.start() }
        handler.post(ticker)
        Log.i(TAG, "started")
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int = START_STICKY

    override fun onDestroy() {
        handler.removeCallbacksAndMessages(null)
        media?.stop()
        calls?.stop()
        ble?.stop()
        instance = null
        state = "остановлено"
        Log.i(TAG, "stopped")
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun createChannel() {
        val nm = getSystemService(NotificationManager::class.java)
        if (nm.getNotificationChannel(CHANNEL) == null) {
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL, "Связь с Пипом", NotificationManager.IMPORTANCE_LOW).apply {
                    description = "Pip Link держит Bluetooth-связь с брелоком"
                    setShowBadge(false)
                }
            )
        }
    }

    private fun notification(): Notification {
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        return NotificationCompat.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentTitle("Pip Link")
            .setContentText(state)
            .setContentIntent(open)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .build()
    }

    private fun startForegroundNow() {
        val type = if (Build.VERSION.SDK_INT >= 29) ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE else 0
        ServiceCompat.startForeground(this, NOTIF, notification(), type)
    }

    private fun updateNotification() {
        try {
            getSystemService(NotificationManager::class.java).notify(NOTIF, notification())
        } catch (_: Exception) {
        }
    }

    private fun sendLine(line: String) {
        ble?.write(line)
    }

    private fun sendTime() {
        val tz = TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 60000
        sendLine("TIME ${System.currentTimeMillis() / 1000} $tz")
    }

    fun note(uid: Int, app: String, title: String, text: String, hh: Int, mm: Int) {
        if (privacy == 2) return
        sendLine("NOTE $uid|${Protocol.clean(app, 23)}|${Protocol.clean(title, 47)}|${Protocol.clean(text, 460)}|$hh|$mm|0")
    }

    fun gone(uid: Int) {
        if (privacy == 2) return
        sendLine("NOTE_GONE $uid")
    }

    private fun onLine(line: String) {
        Log.d(TAG, "<- $line")
        when {
            line.startsWith("CMD ") -> command(line.substring(4))
            line.startsWith("PRIVACY ") -> privacy = line.substring(8).trim().toIntOrNull() ?: privacy
            line.startsWith("STATE ") -> Log.i(TAG, "Pip: $line")
            line == "PONG" -> {}
            else -> {}
        }
    }

    private fun command(text: String) {
        val p = text.trim().split(Regex("\\s+"))
        if (p.isEmpty()) return
        when (p[0]) {
            "DISMISS" -> p.getOrNull(1)?.toIntOrNull()?.let { PipNotificationListener.cancel(it) }
            "ANSWER" -> calls?.answer()
            "DECLINE" -> calls?.decline()
            "PLAY_PAUSE" -> media?.command("play_pause")
            "NEXT" -> media?.command("next")
            "PREV" -> media?.command("prev")
            "VOL_UP" -> media?.command("vol_up")
            "VOL_DOWN" -> media?.command("vol_down")
            "SHUTTER" -> Log.i(TAG, "camera shutter is HID-only on Android")
        }
    }
}

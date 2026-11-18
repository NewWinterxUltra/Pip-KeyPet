package com.novikov.piplink

import android.app.Notification
import android.content.ComponentName
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import android.util.Log
import java.util.concurrent.ConcurrentHashMap

/** Reads Android's notifications and hands them to the Pip Link service. */
class PipNotificationListener : NotificationListenerService() {

    companion object {
        const val TAG = "PipNotes"
        @Volatile
        var connected = false
        private val keys = ConcurrentHashMap<Int, String>()

        fun cancel(uid: Int) {
            val key = keys[uid] ?: return
            try {
                val service = instance
                service?.cancelNotification(key)
            } catch (e: Exception) {
                Log.w(TAG, "cancel: ${e.message}")
            }
        }

        @Volatile
        var instance: PipNotificationListener? = null
    }

    override fun onListenerConnected() {
        super.onListenerConnected()
        instance = this
        connected = true
        Log.i(TAG, "notification listener connected")
    }

    override fun onListenerDisconnected() {
        connected = false
        instance = null
        Log.i(TAG, "notification listener disconnected")
    }

    override fun onNotificationPosted(sbn: StatusBarNotification?) {
        if (sbn == null) return
        if (sbn.packageName == packageName) return
        if (sbn.isOngoing) return
        if ((sbn.notification.flags and Notification.FLAG_GROUP_SUMMARY) != 0) return
        if (sbn.notification.category == Notification.CATEGORY_CALL) return // the call monitor handles those
        if (PipService.privacy == 2) return

        val app = appLabel(sbn.packageName)
        val e = sbn.notification.extras
        val title = e.getCharSequence(Notification.EXTRA_TITLE)?.toString() ?: ""
        var text = e.getCharSequence(Notification.EXTRA_TEXT)?.toString() ?: ""
        if (text.isEmpty()) text = e.getCharSequence(Notification.EXTRA_BIG_TEXT)?.toString() ?: ""
        if (text.isEmpty()) text = e.getCharSequence(Notification.EXTRA_SUB_TEXT)?.toString() ?: ""
        if (title.isEmpty() && text.isEmpty()) return

        val at = if (sbn.notification.`when` > 0) sbn.notification.`when` else System.currentTimeMillis()
        val cal = java.util.Calendar.getInstance().apply { timeInMillis = at }
        var uid = sbn.key.hashCode() and 0x7fffffff
        if (uid == 0) uid = 1
        keys[uid] = sbn.key
        PipService.instance?.note(uid, app, title, text, cal.get(java.util.Calendar.HOUR_OF_DAY), cal.get(java.util.Calendar.MINUTE))
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification?) {
        if (sbn == null) return
        val uid = sbn.key.hashCode() and 0x7fffffff
        if (keys.remove(uid) != null) PipService.instance?.gone(uid)
    }

    private fun appLabel(pkg: String): String = try {
        val pm = packageManager
        pm.getApplicationLabel(pm.getApplicationInfo(pkg, 0)).toString()
    } catch (_: Exception) {
        pkg
    }
}

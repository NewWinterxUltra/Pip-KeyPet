package com.novikov.piplink

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.telecom.TelecomManager
import android.telephony.PhoneStateListener
import android.telephony.TelephonyManager
import android.util.Log
import androidx.core.content.ContextCompat

/** Incoming calls: tells Pip, and answers/declines when Pip asks. */
class CallMonitor(private val context: Context, private val send: (String) -> Unit) {
    companion object {
        const val TAG = "PipCalls"
    }

    private val tm = context.getSystemService(Context.TELEPHONY_SERVICE) as? TelephonyManager
    private val telecom = context.getSystemService(Context.TELECOM_SERVICE) as? TelecomManager
    private var ringing = false
    private var uid = 0

    private val listener = object : PhoneStateListener() {
        override fun onCallStateChanged(state: Int, phoneNumber: String?) {
            when (state) {
                TelephonyManager.CALL_STATE_RINGING -> {
                    if (ringing) return
                    ringing = true
                    uid = System.currentTimeMillis().toInt() and 0x7fffffff
                    val name = if (has(Manifest.permission.READ_CALL_LOG) && !phoneNumber.isNullOrBlank())
                        phoneNumber else "Телефон"
                    send("CALL $uid|${Protocol.clean(name, 47)}|мобильный")
                }
                TelephonyManager.CALL_STATE_IDLE -> {
                    if (ringing) {
                        ringing = false
                        send("CALL_END")
                    }
                }
            }
        }
    }

    fun start() {
        if (!has(Manifest.permission.READ_PHONE_STATE)) return
        try {
            @Suppress("DEPRECATION")
            tm?.listen(listener, PhoneStateListener.LISTEN_CALL_STATE)
        } catch (e: Exception) {
            Log.w(TAG, "listen: ${e.message}")
        }
    }

    fun stop() {
        try {
            @Suppress("DEPRECATION")
            tm?.listen(listener, PhoneStateListener.LISTEN_NONE)
        } catch (_: Exception) {
        }
    }

    fun answer() {
        if (!has(Manifest.permission.ANSWER_PHONE_CALLS)) return
        try {
            telecom?.acceptRingingCall()
        } catch (e: Exception) {
            Log.w(TAG, "answer: ${e.message}")
        }
    }

    fun decline() {
        if (!has(Manifest.permission.ANSWER_PHONE_CALLS)) return
        try {
            if (Build.VERSION.SDK_INT >= 28) telecom?.endCall()
        } catch (e: Exception) {
            Log.w(TAG, "decline: ${e.message}")
        }
    }

    private fun has(p: String) = ContextCompat.checkSelfPermission(context, p) == PackageManager.PERMISSION_GRANTED
}

package com.novikov.piplink

import android.content.ComponentName
import android.content.Context
import android.media.AudioManager
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.util.Locale

/** Now playing: reads the active MediaSession and sends the track; executes Pip's media commands. */
class MediaMonitor(private val context: Context, private val send: (String) -> Unit) {
    companion object {
        const val TAG = "PipMedia"
    }

    private val handler = Handler(Looper.getMainLooper())
    private val msm = context.getSystemService(Context.MEDIA_SESSION_SERVICE) as? MediaSessionManager
    private val component = ComponentName(context, PipNotificationListener::class.java)
    private val audio = context.getSystemService(Context.AUDIO_SERVICE) as? AudioManager
    private var controller: MediaController? = null
    private var last = ""
    private var lastAt = 0L
    private var running = false

    private val poll = object : Runnable {
        override fun run() {
            tick()
            if (running) handler.postDelayed(this, 1000)
        }
    }

    fun start() {
        running = true
        handler.post(poll)
    }

    fun stop() {
        running = false
        handler.removeCallbacks(poll)
    }

    private fun tick() {
        if (!PipNotificationListener.connected) return
        val sessions = try {
            msm?.getActiveSessions(component)
        } catch (e: Exception) {
            Log.w(TAG, "getActiveSessions: ${e.message}")
            null
        }
        val c = sessions?.firstOrNull { it.playbackState?.state == PlaybackState.STATE_PLAYING }
            ?: sessions?.firstOrNull()
        if (c == null) {
            if (last != "none") {
                last = "none"
                send("TRACK ||0|0|0")
            }
            controller = null
            return
        }
        controller = c
        val m = c.metadata ?: return
        val title = m.getString(MediaMetadata.METADATA_KEY_TITLE) ?: ""
        val artist = m.getString(MediaMetadata.METADATA_KEY_ARTIST) ?: ""
        val dur = m.getLong(MediaMetadata.METADATA_KEY_DURATION) / 1000
        val st = c.playbackState
        val pos = (st?.position ?: 0L) / 1000.0
        val playing = st?.state == PlaybackState.STATE_PLAYING
        val line = "TRACK ${Protocol.clean(title, 63)}|${Protocol.clean(artist, 47)}|$dur|${String.format(Locale.US, "%.1f", pos)}|${if (playing) 1 else 0}"
        val now = System.currentTimeMillis()
        if (line != last || now - lastAt > 5000) {
            last = line
            lastAt = now
            send(line)
        }
    }

    fun command(what: String) {
        val c = controller
        try {
            when (what) {
                "play_pause" -> if (c?.playbackState?.state == PlaybackState.STATE_PLAYING) c.transportControls.pause()
                                else c?.transportControls?.play()
                "next" -> c?.transportControls?.skipToNext()
                "prev" -> c?.transportControls?.skipToPrevious()
                "vol_up" -> audio?.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_RAISE, AudioManager.FLAG_SHOW_UI)
                "vol_down" -> audio?.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_LOWER, AudioManager.FLAG_SHOW_UI)
            }
        } catch (e: Exception) {
            Log.w(TAG, "command $what: ${e.message}")
        }
    }
}

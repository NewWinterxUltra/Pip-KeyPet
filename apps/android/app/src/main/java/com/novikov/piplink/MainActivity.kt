package com.novikov.piplink

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat

class MainActivity : AppCompatActivity() {
    private lateinit var status: TextView
    private lateinit var notesButton: Button
    private val handler = Handler(Looper.getMainLooper())

    private val ask = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { refresh() }

    private val tick = object : Runnable {
        override fun run() {
            refresh()
            handler.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(build())
        val need = missing()
        if (need.isNotEmpty()) ask.launch(need.toTypedArray())
        handler.post(tick)
    }

    override fun onDestroy() {
        handler.removeCallbacksAndMessages(null)
        super.onDestroy()
    }

    private fun build(): ScrollView {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(28), dp(24), dp(28))
            setBackgroundColor(Color.rgb(9, 9, 10))
        }

        val title = TextView(this).apply {
            text = "Pip Link"
            textSize = 30f
            setTextColor(Color.rgb(239, 237, 232))
            typeface = Typeface.create("sans-serif", Typeface.BOLD)
        }
        root.addView(title)

        val sub = TextView(this).apply {
            text = "Уведомления, звонки и музыка с Android для брелока Пип."
            textSize = 15f
            setTextColor(Color.rgb(153, 150, 142))
            setPadding(0, dp(4), 0, dp(18))
        }
        root.addView(sub)

        status = TextView(this).apply {
            text = "…"
            textSize = 15f
            setTextColor(Color.rgb(239, 237, 232))
            typeface = Typeface.MONOSPACE
            setBackgroundColor(Color.rgb(19, 19, 21))
            setPadding(dp(16), dp(14), dp(16), dp(14))
        }
        root.addView(status, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))

        notesButton = button("Доступ к уведомлениям") {
            startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
        }
        root.addView(notesButton, params())

        root.addView(button("Разрешения") {
            val m = missing()
            if (m.isNotEmpty()) ask.launch(m.toTypedArray())
        }, params())
        root.addView(button("Подключить Пипа") { start() }, params())
        root.addView(button("Остановить") {
            stopService(Intent(this, PipService::class.java))
            refresh()
        }, params())
        root.addView(button("Настройки приложения") {
            startActivity(
                Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS).setData(Uri.parse("package:$packageName"))
            )
        }, params())

        val help = TextView(this).apply {
            text = "1. Разреши уведомления и Bluetooth.\n" +
                "2. В «Доступ к уведомлениям» включи Pip Link.\n" +
                "3. Нажми «Подключить Пипа», затем подтверди код на брелоке верхней кнопкой.\n\n" +
                "Android 13+: если доступ к уведомлениям не включается, открой «Настройки приложения» → ⋮ → «Разрешить ограниченные настройки».\n\n" +
                "Без доступа к уведомлениям будут работать только звонки и музыка."
            textSize = 13f
            setTextColor(Color.rgb(153, 150, 142))
            setLineSpacing(0f, 1.25f)
            setPadding(0, dp(18), 0, 0)
        }
        root.addView(help)

        val scroll = ScrollView(this).apply { addView(root) }
        return scroll
    }

    private fun button(text: String, onClick: () -> Unit): Button = Button(this).apply {
        this.text = text
        textSize = 16f
        isAllCaps = false
        setTextColor(Color.rgb(9, 9, 10))
        setBackgroundColor(Color.rgb(255, 90, 31))
        setOnClickListener { onClick() }
    }

    private fun params(): LinearLayout.LayoutParams =
        LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(52)).apply { topMargin = dp(10) }

    private fun dp(v: Int): Int = (resources.displayMetrics.density * v).toInt()

    private fun missing(): List<String> {
        val out = ArrayList<String>()
        fun add(p: String) {
            if (ContextCompat.checkSelfPermission(this, p) != PackageManager.PERMISSION_GRANTED) out.add(p)
        }
        if (Build.VERSION.SDK_INT >= 31) {
            add(Manifest.permission.BLUETOOTH_SCAN)
            add(Manifest.permission.BLUETOOTH_CONNECT)
        } else add(Manifest.permission.ACCESS_FINE_LOCATION)
        if (Build.VERSION.SDK_INT >= 33) add(Manifest.permission.POST_NOTIFICATIONS)
        add(Manifest.permission.READ_PHONE_STATE)
        add(Manifest.permission.ANSWER_PHONE_CALLS)
        return out
    }

    private fun start() {
        if (missing().isNotEmpty()) {
            ask.launch(missing().toTypedArray())
            return
        }
        ContextCompat.startForegroundService(this, Intent(this, PipService::class.java))
        Toast.makeText(this, "Pip Link запущен", Toast.LENGTH_SHORT).show()
        refresh()
    }

    private fun refresh() {
        if (!::status.isInitialized) return
        val notes = NotificationManagerCompat.getEnabledListenerPackages(this).contains(packageName)
        val bt = if (missing().none { it.startsWith("android.permission.BLUETOOTH") || it == Manifest.permission.ACCESS_FINE_LOCATION })
            "разрешён" else "нет разрешения"
        status.text = "Связь: ${PipService.state}\nУведомления: ${if (notes) "доступ есть" else "нет доступа"}\nBluetooth: $bt"
        notesButton.text = if (notes) "Уведомления: доступ есть" else "Открыть доступ к уведомлениям"
    }
}

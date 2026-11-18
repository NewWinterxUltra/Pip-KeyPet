package com.novikov.piplink

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.core.content.ContextCompat
import java.util.UUID

/** BLE central for Pip: scans by name, discovers the Pip Link service and turns it into lines. */
class BleClient(
    private val context: Context,
    private val onLine: (String) -> Unit,
    private val onReady: () -> Unit,
) {
    companion object {
        const val TAG = "PipBle"
        val SERVICE: UUID = UUID.fromString("50495044-4247-4b45-5950-455400000010")
        val RX: UUID = UUID.fromString("50495044-4247-4b45-5950-455400000011")
        val TX: UUID = UUID.fromString("50495044-4247-4b45-5950-455400000012")
        val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        const val NAME = "Pip"
    }

    private val handler = Handler(Looper.getMainLooper())
    private val buffer = StringBuilder()
    private var scanner: BluetoothLeScanner? = null
    private var gatt: BluetoothGatt? = null
    private var scanning = false
    private var want = false
    private var retry = 0

    @Volatile
    var ready = false
        private set

    @Volatile
    var status = "не подключено"
        private set

    private val manager: BluetoothManager?
        get() = context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
    private val adapter: BluetoothAdapter?
        get() = manager?.adapter

    private fun canScan(): Boolean =
        if (Build.VERSION.SDK_INT >= 31)
            ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED
        else ContextCompat.checkSelfPermission(context, Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED

    private fun canConnect(): Boolean =
        Build.VERSION.SDK_INT < 31 ||
            ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED

    private fun setStatus(text: String) {
        status = text
        PipService.state = text
        Log.i(TAG, text)
    }

    fun start() {
        want = true
        retry = 0
        scan()
    }

    fun stop() {
        want = false
        stopScan()
        closeGatt()
        ready = false
        setStatus("не подключено")
    }

    private fun scan() {
        if (!want) return
        if (!canScan() || !canConnect()) {
            setStatus("нет разрешения Bluetooth")
            return
        }
        val a = adapter
        if (a == null) {
            setStatus("Bluetooth не поддерживается")
            return
        }
        if (!a.isEnabled) {
            setStatus("Bluetooth выключен")
            handler.postDelayed({ scan() }, 3000)
            return
        }
        if (scanning) return
        scanner = a.bluetoothLeScanner
        if (scanner == null) {
            setStatus("Bluetooth не готов")
            handler.postDelayed({ scan() }, 3000)
            return
        }
        scanning = true
        setStatus("ищу Pip…")
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        try {
            scanner?.startScan(null, settings, scanCallback)
        } catch (e: SecurityException) {
            scanning = false
            setStatus("нет разрешения Bluetooth")
            return
        }
        handler.postDelayed({
            if (scanning) {
                stopScan()
                retry++
                setStatus(if (retry < 3) "Pip не найден, ищу ещё…" else "Pip не найден. Он рядом и проснулся?")
                handler.postDelayed({ scan() }, 3000)
            }
        }, 8000)
    }

    private fun stopScan() {
        if (!scanning) return
        scanning = false
        try {
            scanner?.stopScan(scanCallback)
        } catch (_: Exception) {
        }
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val name = result.device.name ?: result.scanRecord?.deviceName
            if (name == NAME) {
                stopScan()
                connect(result.device)
            }
        }

        override fun onScanFailed(errorCode: Int) {
            scanning = false
            setStatus("ошибка поиска ($errorCode)")
            handler.postDelayed({ scan() }, 3000)
        }
    }

    @SuppressLint("MissingPermission")
    private fun connect(device: BluetoothDevice) {
        if (!want || !canConnect()) return
        closeGatt()
        setStatus("подключаюсь…")
        try {
            if (device.bondState == BluetoothDevice.BOND_NONE) device.createBond()
        } catch (_: Exception) {
        }
        gatt = try {
            device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        } catch (e: SecurityException) {
            setStatus("нет разрешения Bluetooth")
            null
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {
                setStatus("соединён, ищу службы…")
                handler.postDelayed({ try { g.discoverServices() } catch (_: Exception) {} }, 300)
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                ready = false
                closeGatt()
                if (want) {
                    setStatus("связь потеряна, переподключаюсь…")
                    handler.postDelayed({ scan() }, 2500)
                }
            }
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                setStatus("службы не найдены ($status)")
                return
            }
            val service = g.getService(SERVICE)
            val tx = service?.getCharacteristic(TX)
            if (service == null || tx == null) {
                setStatus("у Пипа нет службы Pip Link")
                return
            }
            try {
                g.setCharacteristicNotification(tx, true)
                val d = tx.getDescriptor(CCCD)
                if (d != null) {
                    d.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    g.writeDescriptor(d)
                } else {
                    hello()
                }
            } catch (e: Exception) {
                setStatus("не удалось подписаться: ${e.message}")
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) hello()
            else {
                setStatus("подписка не удалась ($status)")
                handler.postDelayed({ try { g.discoverServices() } catch (_: Exception) {} }, 2000)
            }
        }

        @Deprecated("Deprecated in API 33")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            handle(c.value ?: return)
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) {
            handle(value)
        }
    }

    private fun handle(value: ByteArray) {
        buffer.append(String(value, Charsets.UTF_8))
        while (true) {
            val i = buffer.indexOf("\n")
            if (i < 0) break
            val line = buffer.substring(0, i).trimEnd('\r')
            buffer.delete(0, i + 1)
            if (line.isEmpty()) continue
            if (line.startsWith("OK HELLO")) {
                ready = true
                setStatus("подключён")
                onReady()
            }
            onLine(line)
        }
    }

    private fun hello() {
        if (!write("HELLO android 1")) setStatus("не удалось поздороваться")
        handler.postDelayed(::helloIfNeeded, 2500)
    }

    private fun helloIfNeeded() {
        if (want && !ready && gatt != null) hello()
    }

    fun write(line: String): Boolean {
        val g = gatt ?: return false
        val ch = g.getService(SERVICE)?.getCharacteristic(RX) ?: return false
        return synchronized(this) {
            try {
                ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                ch.value = (line + "\n").toByteArray(Charsets.UTF_8)
                g.writeCharacteristic(ch)
            } catch (e: Exception) {
                Log.w(TAG, "write: ${e.message}")
                false
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun closeGatt() {
        val g = gatt ?: return
        gatt = null
        try {
            g.disconnect()
            g.close()
        } catch (_: Exception) {
        }
    }
}

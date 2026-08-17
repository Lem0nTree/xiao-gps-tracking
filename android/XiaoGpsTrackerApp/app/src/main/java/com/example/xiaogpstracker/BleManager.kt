package com.example.xiaogpstracker

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.util.Log
import androidx.core.content.ContextCompat
import java.util.UUID

class BleManager(
    private val context: Context,
    private val listener: Listener
) {
    interface Listener {
        fun onState(message: String)
        fun onReady()
        fun onNotification(data: ByteArray)
        fun onDisconnected()
    }

    companion object {
        private const val TAG = "XiaoGpsBle"
        val NUS_SERVICE: UUID = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e")
        val NUS_RX: UUID = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e") // phone -> XIAO
        val NUS_TX: UUID = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e") // XIAO -> phone
        val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    }

    private val main = Handler(Looper.getMainLooper())
    private val bluetoothManager =
        context.getSystemService(BluetoothManager::class.java)
    val adapter: BluetoothAdapter? get() = bluetoothManager.adapter

    private var scanner: BluetoothLeScanner? = null
    private var gatt: BluetoothGatt? = null
    private var rxCharacteristic: BluetoothGattCharacteristic? = null
    private var txCharacteristic: BluetoothGattCharacteristic? = null
    private var currentDevice: BluetoothDevice? = null
    private var notificationsReady = false
    private var notificationWriteInFlight = false
    private var notificationRetryCount = 0
    private var connectionInProgress = false
    private var receiverRegistered = false

    val isReady: Boolean get() = gatt != null && notificationsReady

    private fun dispatchState(message: String) {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            listener.onState(message)
        } else {
            main.post { listener.onState(message) }
        }
    }

    private fun dispatchReady() {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            listener.onReady()
        } else {
            main.post { listener.onReady() }
        }
    }

    private fun dispatchDisconnected() {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            listener.onDisconnected()
        } else {
            main.post { listener.onDisconnected() }
        }
    }

    private fun dispatchNotification(data: ByteArray) {
        val copy = data.copyOf()
        if (Looper.myLooper() == Looper.getMainLooper()) {
            listener.onNotification(copy)
        } else {
            main.post { listener.onNotification(copy) }
        }
    }

    init {
        registerBondReceiver()
    }

    @SuppressLint("MissingPermission")
    fun close() {
        stopScan()
        resetGatt()
        if (receiverRegistered) {
            runCatching { context.unregisterReceiver(bondReceiver) }
            receiverRegistered = false
        }
    }

    @SuppressLint("MissingPermission")
    private fun resetGatt() {
        val old = gatt
        gatt = null
        currentDevice = null
        rxCharacteristic = null
        txCharacteristic = null
        notificationsReady = false
        notificationWriteInFlight = false
        notificationRetryCount = 0
        connectionInProgress = false

        if (old != null && hasConnectPermission()) {
            runCatching { old.disconnect() }
            runCatching { old.close() }
        }
    }

    @SuppressLint("MissingPermission")
    fun disconnect() {
        stopScan()

        val current = gatt
        if (current == null) {
            resetGatt()
            dispatchState("Disconnected")
            dispatchDisconnected()
            return
        }

        if (!hasConnectPermission()) {
            resetGatt()
            dispatchState("Disconnected")
            dispatchDisconnected()
            return
        }

        val disconnectStarted = runCatching {
            current.disconnect()
            true
        }.getOrElse {
            resetGatt()
            dispatchState("Disconnected")
            dispatchDisconnected()
            false
        }

        if (!disconnectStarted) return

        // Defensive fallback for OEM stacks that fail to deliver the normal
        // STATE_DISCONNECTED callback.
        main.postDelayed({
            if (gatt === current) {
                resetGatt()
                dispatchState("Disconnected")
                dispatchDisconnected()
            }
        }, 2_000)
    }

    @SuppressLint("MissingPermission")
    fun startScan() {
        if (!hasRequiredPermissions()) {
            dispatchState("Bluetooth permission missing")
            return
        }

        val a = adapter
        if (a == null) {
            dispatchState("Bluetooth is not supported")
            return
        }
        if (!a.isEnabled) {
            dispatchState("Turn Bluetooth on first")
            return
        }

        stopScan()

        // Connect is also a recovery action: dispose any half-open GATT session
        // left by an interrupted pairing/subscription attempt before scanning again.
        if (gatt != null || connectionInProgress) {
            resetGatt()
        }

        dispatchState("Scanning for XIAO-GPS…")

        scanner = a.bluetoothLeScanner

        // The firmware advertises the Nordic UART Service UUID.
        val filter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid(NUS_SERVICE))
            .build()

        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        scanner?.startScan(listOf(filter), settings, scanCallback)

        main.postDelayed({
            if (gatt == null) {
                stopScan()
                dispatchState("XIAO-GPS not found")
            }
        }, 12_000)
    }

    @SuppressLint("MissingPermission")
    fun stopScan() {
        if (!hasScanPermission()) return
        runCatching { scanner?.stopScan(scanCallback) }
    }

    @SuppressLint("MissingPermission")
    fun send(data: ByteArray): Boolean {
        if (!hasConnectPermission()) {
            Log.w(TAG, "send blocked: BLUETOOTH_CONNECT permission missing")
            return false
        }

        val g = gatt
        val rx = rxCharacteristic
        if (g == null || rx == null || !notificationsReady) {
            Log.w(
                TAG,
                "send blocked: gatt=${g != null} rx=${rx != null} notificationsReady=$notificationsReady"
            )
            return false
        }

        val type = if (data.size >= 3) data[2].toInt() and 0xFF else -1

        val started = if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(
                rx,
                data,
                BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            ) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            run {
                rx.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                rx.value = data
                g.writeCharacteristic(rx)
            }
        }

        Log.d(
            TAG,
            "NUS RX write start type=0x${type.toString(16)} bytes=${data.size} started=$started bondState=${g.device.bondState}"
        )
        return started
    }

    fun hasRequiredPermissions(): Boolean = hasScanPermission() && hasConnectPermission()

    private fun hasScanPermission(): Boolean {
        return if (Build.VERSION.SDK_INT >= 31) {
            ContextCompat.checkSelfPermission(
                context,
                Manifest.permission.BLUETOOTH_SCAN
            ) == PackageManager.PERMISSION_GRANTED
        } else {
            ContextCompat.checkSelfPermission(
                context,
                Manifest.permission.ACCESS_FINE_LOCATION
            ) == PackageManager.PERMISSION_GRANTED
        }
    }

    private fun hasConnectPermission(): Boolean {
        return if (Build.VERSION.SDK_INT >= 31) {
            ContextCompat.checkSelfPermission(
                context,
                Manifest.permission.BLUETOOTH_CONNECT
            ) == PackageManager.PERMISSION_GRANTED
        } else {
            true
        }
    }

    @SuppressLint("MissingPermission")
    private fun enableNotifications() {
        if (!hasConnectPermission()) return
        if (notificationsReady || notificationWriteInFlight) return

        val g = gatt ?: return
        val tx = txCharacteristic ?: return
        val device = currentDevice ?: return

        if (!g.setCharacteristicNotification(tx, true)) {
            dispatchState("Could not enable local BLE notifications")
            return
        }

        val cccd = tx.getDescriptor(CCCD)
        if (cccd == null) {
            dispatchState("CCCD descriptor missing")
            return
        }

        dispatchState(
            if (device.bondState == BluetoothDevice.BOND_BONDED)
                "Opening encrypted BLE data channel…"
            else
                "Waiting for secure pairing…"
        )

        val started = if (Build.VERSION.SDK_INT >= 33) {
            g.writeDescriptor(
                cccd,
                BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            ) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            run {
                cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                g.writeDescriptor(cccd)
            }
        }

        if (!started) {
            dispatchState("Could not start secure notification subscription")
            return
        }

        notificationWriteInFlight = true
        Log.d(TAG, "CCCD write started; bondState=${device.bondState}")

        // OEM Bluetooth stacks occasionally lose a GATT callback during the
        // transition from bonding -> encrypted ATT. Recover instead of hanging.
        val expectedGatt = g
        main.postDelayed({
            if (gatt === expectedGatt &&
                notificationWriteInFlight &&
                !notificationsReady
            ) {
                notificationWriteInFlight = false
                if (notificationRetryCount < 3) {
                    notificationRetryCount++
                    Log.w(TAG, "CCCD callback timeout; retry $notificationRetryCount")
                    dispatchState("Retrying secure BLE data channel…")
                    enableNotifications()
                } else {
                    dispatchState("Secure BLE setup timed out. Tap Connect to retry.")
                }
            }
        }, 8_000)
    }

    @SuppressLint("MissingPermission")
    private fun connect(device: BluetoothDevice) {
        if (!hasConnectPermission()) return
        if (connectionInProgress || gatt != null) return

        connectionInProgress = true
        stopScan()
        currentDevice = device
        notificationsReady = false
        notificationWriteInFlight = false
        notificationRetryCount = 0

        dispatchState("Connecting…")
        Log.d(TAG, "Connecting to ${device.address}; bondState=${device.bondState}")

        // minSdk is 26. Passing the main Handler makes every BluetoothGattCallback
        // run on the main thread instead of Android's unspecified Binder thread.
        gatt = device.connectGatt(
            context,
            false,
            gattCallback,
            BluetoothDevice.TRANSPORT_LE,
            BluetoothDevice.PHY_LE_1M_MASK or BluetoothDevice.PHY_LE_2M_MASK,
            main
        )

        if (gatt == null) {
            connectionInProgress = false
            currentDevice = null
            dispatchState("Could not create GATT connection")
        }
    }

    private val scanCallback = object : ScanCallback() {
        @SuppressLint("MissingPermission")
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            if (!connectionInProgress && gatt == null) {
                connect(result.device)
            }
        }

        override fun onScanFailed(errorCode: Int) {
            dispatchState("BLE scan failed: $errorCode")
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(
            g: BluetoothGatt,
            status: Int,
            newState: Int
        ) {
            // Ignore callbacks from a GATT object that has already been replaced.
            if (gatt !== g) {
                runCatching { g.close() }
                return
            }

            Log.d(TAG, "connectionState status=$status newState=$newState")

            if (status != BluetoothGatt.GATT_SUCCESS ||
                newState == BluetoothProfile.STATE_DISCONNECTED
            ) {
                notificationsReady = false
                notificationWriteInFlight = false
                rxCharacteristic = null
                txCharacteristic = null
                connectionInProgress = false
                gatt = null
                runCatching { g.close() }
                dispatchState("Disconnected (GATT status $status)")
                dispatchDisconnected()
                return
            }

            if (newState == BluetoothProfile.STATE_CONNECTED) {
                connectionInProgress = false
                dispatchState(
                    if (g.device.bondState == BluetoothDevice.BOND_BONDED)
                        "Connected to paired XIAO…"
                    else
                        "Connected. Enter the XIAO pairing PIN when Android asks."
                )

                // Negotiate a useful MTU; if the request isn't accepted, discovery still proceeds.
                if (!g.requestMtu(247)) {
                    g.discoverServices()
                }
            }
        }

        @SuppressLint("MissingPermission")
        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            Log.d(TAG, "MTU changed mtu=$mtu status=$status")
            if (gatt === g) g.discoverServices()
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                dispatchState("GATT service discovery failed (status $status)")
                return
            }

            val service = g.getService(NUS_SERVICE)
            val rx = service?.getCharacteristic(NUS_RX)
            val tx = service?.getCharacteristic(NUS_TX)

            if (rx == null || tx == null) {
                dispatchState("Nordic UART service not found")
                return
            }

            rxCharacteristic = rx
            txCharacteristic = tx

            val device = currentDevice
            if (device != null && device.bondState != BluetoothDevice.BOND_BONDED) {
                dispatchState("Pairing… enter the 6-digit XIAO PIN when prompted")

                // The XIAO also requests pairing. Calling createBond() here makes
                // first-run behavior robust on phones that do not immediately act
                // on the peripheral security request.
                if (device.bondState == BluetoothDevice.BOND_NONE) {
                    val started = device.createBond()
                    Log.d(TAG, "createBond() started=$started")
                }
                return
            }

            enableNotifications()
        }

        override fun onDescriptorWrite(
            g: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int
        ) {
            if (descriptor.uuid != CCCD || gatt !== g) return

            notificationWriteInFlight = false
            Log.d(
                TAG,
                "CCCD write completed status=$status bondState=${g.device.bondState}"
            )

            if (status == BluetoothGatt.GATT_SUCCESS) {
                notificationsReady = true
                notificationRetryCount = 0
                dispatchState("Secure BLE link ready")
                dispatchReady()
                return
            }

            notificationsReady = false
            val securityRelated =
                status == BluetoothGatt.GATT_INSUFFICIENT_AUTHENTICATION ||
                status == BluetoothGatt.GATT_INSUFFICIENT_ENCRYPTION ||
                status == BluetoothGatt.GATT_INSUFFICIENT_AUTHORIZATION

            if ((securityRelated || status == BluetoothGatt.GATT_FAILURE) &&
                g.device.bondState == BluetoothDevice.BOND_BONDED &&
                notificationRetryCount < 3
            ) {
                notificationRetryCount++
                dispatchState("BLE bonded; retrying encrypted channel…")
                main.postDelayed({ enableNotifications() }, 500L * notificationRetryCount)
            } else if (g.device.bondState == BluetoothDevice.BOND_BONDING) {
                dispatchState("Waiting for pairing to complete…")
            } else {
                dispatchState("Secure notification setup failed (GATT $status). Tap Connect to retry.")
            }
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int
        ) {
            if (gatt !== g || characteristic.uuid != NUS_RX) return
            Log.d(
                TAG,
                "NUS RX write completed status=$status bondState=${g.device.bondState}"
            )
            if (status != BluetoothGatt.GATT_SUCCESS) {
                dispatchState("BLE command write failed (GATT $status)")
            }
        }

        @Deprecated("Deprecated in API 33, kept for older Android versions")
        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic
        ) {
            if (characteristic.uuid == NUS_TX) {
                @Suppress("DEPRECATION")
                val value = characteristic.value?.copyOf() ?: return
                dispatchNotification(value)
            }
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray
        ) {
            if (characteristic.uuid == NUS_TX) {
                dispatchNotification(value)
            }
        }
    }

    private val bondReceiver = object : BroadcastReceiver() {
        @SuppressLint("MissingPermission")
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action != BluetoothDevice.ACTION_BOND_STATE_CHANGED) return

            val device: BluetoothDevice? =
                if (Build.VERSION.SDK_INT >= 33) {
                    intent.getParcelableExtra(
                        BluetoothDevice.EXTRA_DEVICE,
                        BluetoothDevice::class.java
                    )
                } else {
                    @Suppress("DEPRECATION")
                    intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
                }

            if (device?.address != currentDevice?.address) return

            when (intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.BOND_NONE)) {
                BluetoothDevice.BOND_BONDING -> {
                    dispatchState("Pairing… enter the XIAO PIN")
                }

                BluetoothDevice.BOND_BONDED -> {
                    dispatchState("Paired and bonded; opening secure data channel…")
                    // Retry the protected CCCD subscription after Android has
                    // committed the bond. The in-flight guard prevents duplicates.
                    main.postDelayed({ enableNotifications() }, 300)
                }

                BluetoothDevice.BOND_NONE -> {
                    notificationsReady = false
                    notificationWriteInFlight = false
                    dispatchState("Not paired")
                }
            }
        }
    }

    private fun registerBondReceiver() {
        val filter = IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED)
        if (Build.VERSION.SDK_INT >= 33) {
            context.registerReceiver(bondReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("DEPRECATION")
            context.registerReceiver(bondReceiver, filter)
        }
        receiverRegistered = true
    }
}

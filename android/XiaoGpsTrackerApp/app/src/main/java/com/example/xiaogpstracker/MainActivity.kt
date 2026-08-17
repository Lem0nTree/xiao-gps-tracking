package com.example.xiaogpstracker

import android.Manifest
import android.app.AlertDialog
import android.bluetooth.BluetoothAdapter
import android.content.Intent
import android.content.res.ColorStateList
import android.content.res.Configuration
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.View
import android.widget.*
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.ViewCompat
import org.maplibre.android.MapLibre
import org.maplibre.android.annotations.Marker
import org.maplibre.android.annotations.MarkerOptions
import org.maplibre.android.annotations.Polyline
import org.maplibre.android.annotations.PolylineOptions
import org.maplibre.android.camera.CameraUpdateFactory
import org.maplibre.android.geometry.LatLng
import org.maplibre.android.geometry.LatLngBounds
import org.maplibre.android.maps.MapLibreMap
import org.maplibre.android.maps.MapView
import java.nio.charset.StandardCharsets
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale

class MainActivity : AppCompatActivity(), BleManager.Listener {

    private lateinit var connectionText: TextView
    private lateinit var gpsText: TextView
    private lateinit var storageText: TextView
    private lateinit var activityText: TextView
    private lateinit var versionText: TextView
    private lateinit var pointsBadge: TextView
    private lateinit var themeButton: Button
    private lateinit var timelineSeekBar: SeekBar
    private lateinit var timelineTimeText: TextView
    private lateinit var timelineStartText: TextView
    private lateinit var timelineEndText: TextView
    private lateinit var timelineDetailText: TextView
    private lateinit var connectButton: Button
    private lateinit var downloadButton: Button
    private lateinit var intervalButton: Button
    private lateinit var intervalSummaryText: TextView
    private lateinit var moreButton: Button
    private lateinit var progressBar: ProgressBar
    private lateinit var mapView: MapView

    private lateinit var ble: BleManager
    private lateinit var store: TrackStore
    private val parser = PacketParser()

    private var map: MapLibreMap? = null
    private var mapStyleReady = false
    private var trackPolyline: Polyline? = null
    private var futureTrackPolyline: Polyline? = null
    private var timelineMarker: Marker? = null
    private var selectedTimelineIndex = -1

    private var deviceInfo: DeviceInfo? = null
    private val pendingDownload = mutableListOf<GpsRecord>()
    private var downloadStartLocalCount = 0
    private var pendingExport: ExportFormat? = null
    private var pendingIntervalSeconds: Int? = null

    private val intervalCommandTimeout = Runnable {
        val requested = pendingIntervalSeconds ?: return@Runnable
        setActivity(
            "No confirmation for ${formatInterval(requested)}. Reading tracker setting again…"
        )
        // INFO lets us distinguish a lost ACK from a rejected/failed update.
        ble.send(Protocol.infoRequest())
        activityText.postDelayed({
            val actual = deviceInfo?.logIntervalSeconds
            if (pendingIntervalSeconds != null) {
                if (actual == requested) {
                    finishIntervalUpdate(requested)
                } else {
                    pendingIntervalSeconds = null
                    intervalButton.text = actual?.let(::formatInterval) ?: "Interval"
                    intervalSummaryText.text =
                        actual?.let(::intervalSummary) ?: "Tracker did not confirm the setting"
                    toast("Tracker did not confirm the new GPS interval")
                    armBleIdleDisconnect()
                }
            }
        }, 1200)
    }
    private var infoRetryCount = 0

    private val bleIdleDisconnect = Runnable {
        if (ble.isReady && pendingDownload.isEmpty()) {
            setActivity("BLE idle • disconnected to save battery")
            ble.disconnect()
        }
    }

    private val timelineTimeFormatter: DateTimeFormatter =
        DateTimeFormatter.ofPattern("EEE, dd MMM yyyy • HH:mm:ss", Locale.getDefault())
            .withZone(ZoneId.systemDefault())

    private val timelineEndpointFormatter: DateTimeFormatter =
        DateTimeFormatter.ofPattern("dd MMM yyyy HH:mm", Locale.getDefault())
            .withZone(ZoneId.systemDefault())

    enum class ExportFormat { CSV, GPX }

    private val permissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
            if (ble.hasRequiredPermissions()) {
                ensureBluetoothAndScan()
            } else {
                toast("Bluetooth permission is required")
            }
        }

    private val enableBluetoothLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
            if (ble.adapter?.isEnabled == true) ble.startScan()
        }

    private val createFileLauncher =
        registerForActivityResult(ActivityResultContracts.CreateDocument("*/*")) { uri ->
            if (uri == null) return@registerForActivityResult
            val format = pendingExport ?: return@registerForActivityResult

            val text = when (format) {
                ExportFormat.CSV -> store.csv()
                ExportFormat.GPX -> store.gpx()
            }

            contentResolver.openOutputStream(uri)?.bufferedWriter(StandardCharsets.UTF_8)?.use {
                it.write(text)
            }

            pendingExport = null
            toast("Exported")
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        applySavedTheme()
        super.onCreate(savedInstanceState)

        MapLibre.getInstance(this)
        setContentView(R.layout.activity_main)
        applySystemBarInsets()

        connectionText = findViewById(R.id.connectionText)
        gpsText = findViewById(R.id.gpsText)
        storageText = findViewById(R.id.storageText)
        activityText = findViewById(R.id.activityText)
        versionText = findViewById(R.id.versionText)
        pointsBadge = findViewById(R.id.pointsBadge)
        themeButton = findViewById(R.id.themeButton)
        timelineSeekBar = findViewById(R.id.timelineSeekBar)
        timelineTimeText = findViewById(R.id.timelineTimeText)
        timelineStartText = findViewById(R.id.timelineStartText)
        timelineEndText = findViewById(R.id.timelineEndText)
        timelineDetailText = findViewById(R.id.timelineDetailText)
        connectButton = findViewById(R.id.connectButton)
        downloadButton = findViewById(R.id.downloadButton)
        intervalButton = findViewById(R.id.intervalButton)
        intervalSummaryText = findViewById(R.id.intervalSummaryText)
        moreButton = findViewById(R.id.moreButton)
        progressBar = findViewById(R.id.progressBar)
        mapView = findViewById(R.id.mapView)

        store = TrackStore(this)
        ble = BleManager(this, this)

        Log.i("XiaoGpsApp", "APP VERSION $APP_VERSION")
        versionText.text = "App v$APP_VERSION"
        updateThemeButton()
        setConnectionState("Offline", ConnectionTone.NEUTRAL)
        gpsText.text = "Waiting for tracker"
        activityText.text = "Connect to view live GPS status"

        setupMap(savedInstanceState)
        setupTimeline()
        setupButtons()
        updateLocalStatus()
    }


    private fun applySavedTheme() {
        val mode = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
            .getInt(PREF_THEME_MODE, AppCompatDelegate.MODE_NIGHT_FOLLOW_SYSTEM)
        AppCompatDelegate.setDefaultNightMode(mode)
    }

    private fun isNightMode(): Boolean {
        return (resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
            Configuration.UI_MODE_NIGHT_YES
    }

    private fun updateThemeButton() {
        themeButton.text = if (isNightMode()) "☀" else "☾"
        themeButton.contentDescription =
            if (isNightMode()) "Switch to light mode" else "Switch to dark mode"
    }

    private fun toggleTheme() {
        val target = if (isNightMode()) {
            AppCompatDelegate.MODE_NIGHT_NO
        } else {
            AppCompatDelegate.MODE_NIGHT_YES
        }

        getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
            .edit()
            .putInt(PREF_THEME_MODE, target)
            .apply()

        AppCompatDelegate.setDefaultNightMode(target)
    }

    /**
     * API 35+ apps are edge-to-edge by default, and API 36 removes the opt-out.
     * Keep the modern edge-to-edge window, but move interactive content inside
     * the status bar, display cutout, and navigation bar safe areas.
     *
     * We preserve the XML design padding and add the live system insets on top.
     */
    private fun applySystemBarInsets() {
        val root = findViewById<View>(R.id.rootContainer)

        val baseLeft = root.paddingLeft
        val baseTop = root.paddingTop
        val baseRight = root.paddingRight
        val baseBottom = root.paddingBottom

        ViewCompat.setOnApplyWindowInsetsListener(root) { view, windowInsets ->
            val safeInsets = windowInsets.getInsets(
                WindowInsetsCompat.Type.systemBars() or
                    WindowInsetsCompat.Type.displayCutout()
            )

            view.setPadding(
                baseLeft + safeInsets.left,
                baseTop + safeInsets.top,
                baseRight + safeInsets.right,
                baseBottom + safeInsets.bottom
            )

            // Return the original insets so children such as MapLibre can still
            // receive them if they need to handle their own system UI geometry.
            windowInsets
        }

        ViewCompat.requestApplyInsets(root)
    }

    private enum class ConnectionTone {
        NEUTRAL, BUSY, CONNECTED, ERROR
    }

    private fun setConnectionState(label: String, tone: ConnectionTone) {
        connectionText.text = label

        val color = when (tone) {
            ConnectionTone.NEUTRAL -> getColor(R.color.text_secondary)
            ConnectionTone.BUSY -> getColor(R.color.warning)
            ConnectionTone.CONNECTED -> getColor(R.color.success)
            ConnectionTone.ERROR -> getColor(R.color.danger)
        }

        connectionText.setTextColor(color)
        connectionText.backgroundTintList = ColorStateList.valueOf(
            when (tone) {
                ConnectionTone.NEUTRAL -> getColor(R.color.status_neutral_bg)
                ConnectionTone.BUSY -> getColor(R.color.status_busy_bg)
                ConnectionTone.CONNECTED -> getColor(R.color.status_connected_bg)
                ConnectionTone.ERROR -> getColor(R.color.status_error_bg)
            }
        )
    }

    private fun setActivity(message: String) {
        activityText.text = message
    }

    private fun formatInterval(seconds: Int): String = when (seconds) {
        60 -> "1 min"
        900 -> "15 min"
        1800 -> "30 min"
        3600 -> "1 hour"
        7200 -> "2 hours"
        10800 -> "3 hours"
        else -> "${seconds}s"
    }

    private fun intervalSummary(seconds: Int): String = when (seconds) {
        60 -> "GPS stays powered between fixes • highest battery use"
        900 -> "GPS power-gated • 15-minute tracking"
        1800 -> "GPS power-gated • recommended for 300 mAh"
        3600 -> "GPS power-gated • low-power tracking"
        7200 -> "GPS power-gated • extended battery"
        10800 -> "GPS power-gated • maximum battery profile"
        else -> "GPS power-gated"
    }

    private fun armBleIdleDisconnect(delayMs: Long = 60_000L) {
        activityText.removeCallbacks(bleIdleDisconnect)
        if (ble.isReady) {
            activityText.postDelayed(bleIdleDisconnect, delayMs)
        }
    }

    private fun cancelBleIdleDisconnect() {
        activityText.removeCallbacks(bleIdleDisconnect)
    }

    private fun supportsIntervalConfiguration(info: DeviceInfo?): Boolean {
        info ?: return false
        if (info.firmwareMajor > 1) return true
        return info.firmwareMajor == 1 && info.firmwareMinor >= 5
    }

    private fun finishIntervalUpdate(seconds: Int) {
        activityText.removeCallbacks(intervalCommandTimeout)
        pendingIntervalSeconds = null
        intervalButton.text = formatInterval(seconds)
        intervalSummaryText.text = intervalSummary(seconds)
        setActivity("GPS interval set to ${formatInterval(seconds)}")

        // Read back the persisted value instead of trusting only the ACK.
        activityText.postDelayed({
            if (ble.isReady) ble.send(Protocol.infoRequest())
        }, 250)

        armBleIdleDisconnect(20_000L)
    }

    private fun showIntervalDialog() {
        if (!ble.isReady) {
            toast("Connect to the tracker first")
            return
        }

        val info = deviceInfo
        if (!supportsIntervalConfiguration(info)) {
            val fw = info?.firmwareVersion ?: "legacy"
            toast("GPS interval control requires XIAO firmware 1.5+ (found $fw)")
            setActivity("Update the XIAO firmware to 1.5.1 to change the GPS interval")
            return
        }

        val current = info?.logIntervalSeconds ?: 1800
        val checked = TRACK_INTERVAL_SECONDS.indexOf(current).coerceAtLeast(2)

        AlertDialog.Builder(this)
            .setTitle("GPS wake interval")
            .setSingleChoiceItems(TRACK_INTERVAL_LABELS, checked) { dialog, which ->
                val seconds = TRACK_INTERVAL_SECONDS[which]
                pendingIntervalSeconds = seconds
                cancelBleIdleDisconnect()

                if (ble.send(Protocol.setIntervalRequest(seconds))) {
                    intervalButton.text = formatInterval(seconds)
                    intervalSummaryText.text = "Saving ${formatInterval(seconds)} interval…"
                    setActivity("Updating GPS wake interval")
                    activityText.removeCallbacks(intervalCommandTimeout)
                    activityText.postDelayed(intervalCommandTimeout, 3500)
                } else {
                    pendingIntervalSeconds = null
                    toast("Could not send interval setting")
                    armBleIdleDisconnect()
                }

                dialog.dismiss()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun showClearHistoryDialog() {
        if (!ble.isReady) {
            toast("Connect to clear the tracker log")
            return
        }

        AlertDialog.Builder(this)
            .setTitle("Clear GPS history?")
            .setMessage(
                "This erases the XIAO GPS log and the local copy in this app. " +
                    "It does NOT remove the owner-phone pairing or GPS interval. " +
                    "Export GPX/CSV first if you want to keep the track."
            )
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Clear") { _, _ ->
                cancelBleIdleDisconnect()
                if (!ble.send(Protocol.clearRequest())) {
                    toast("Could not send clear command")
                    armBleIdleDisconnect()
                }
            }
            .show()
    }

    private fun showMoreMenu(anchor: View) {
        val menu = PopupMenu(this, anchor)
        val exportGpx = menu.menu.add(0, MENU_EXPORT_GPX, 0, "Export GPX")
        val exportCsv = menu.menu.add(0, MENU_EXPORT_CSV, 1, "Export CSV")
        val clear = menu.menu.add(0, MENU_CLEAR_HISTORY, 2, "Clear history")

        val hasTrack = store.records.isNotEmpty()
        exportGpx.isEnabled = hasTrack
        exportCsv.isEnabled = hasTrack
        clear.isEnabled = ble.isReady

        menu.setOnMenuItemClickListener { item ->
            when (item.itemId) {
                MENU_EXPORT_GPX -> {
                    pendingExport = ExportFormat.GPX
                    createFileLauncher.launch("xiao-track.gpx")
                    true
                }

                MENU_EXPORT_CSV -> {
                    pendingExport = ExportFormat.CSV
                    createFileLauncher.launch("xiao-track.csv")
                    true
                }

                MENU_CLEAR_HISTORY -> {
                    showClearHistoryDialog()
                    true
                }

                else -> false
            }
        }

        menu.show()
    }

    private fun setupButtons() {
        themeButton.setOnClickListener {
            toggleTheme()
        }

        connectButton.setOnClickListener {
            cancelBleIdleDisconnect()
            requestPermissionsAndScan()
        }

        downloadButton.setOnClickListener {
            if (!ble.isReady) {
                toast("Connect first")
                return@setOnClickListener
            }

            cancelBleIdleDisconnect()
            pendingDownload.clear()
            downloadStartLocalCount = store.records.size
            progressBar.progress = 0
            progressBar.visibility = View.VISIBLE

            val afterSeq = store.lastSeq
            if (!ble.send(Protocol.downloadRequest(afterSeq))) {
                progressBar.visibility = View.GONE
                toast("Could not start download")
                armBleIdleDisconnect()
            } else {
                setActivity("Syncing points after #$afterSeq…")
            }
        }

        intervalButton.setOnClickListener {
            showIntervalDialog()
        }

        moreButton.setOnClickListener {
            showMoreMenu(it)
        }
    }

    private fun requestPermissionsAndScan() {
        val permissions = if (Build.VERSION.SDK_INT >= 31) {
            arrayOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT
            )
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }

        if (ble.hasRequiredPermissions()) {
            ensureBluetoothAndScan()
        } else {
            permissionLauncher.launch(permissions)
        }
    }

    private fun ensureBluetoothAndScan() {
        val adapter = ble.adapter
        if (adapter == null) {
            toast("This phone has no Bluetooth adapter")
            return
        }

        if (!adapter.isEnabled) {
            enableBluetoothLauncher.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
        } else {
            ble.startScan()
        }
    }

    // ---------------- BLE callbacks ----------------

    override fun onState(message: String) {
        setActivity(message)

        when {
            message.contains("Scanning", ignoreCase = true) ||
                message.contains("Connecting", ignoreCase = true) ||
                message.contains("secur", ignoreCase = true) ->
                setConnectionState("Connecting", ConnectionTone.BUSY)

            message.contains("Disconnected", ignoreCase = true) ||
                message.contains("not found", ignoreCase = true) ->
                setConnectionState("Offline", ConnectionTone.NEUTRAL)

            message.contains("failed", ignoreCase = true) ||
                message.contains("missing", ignoreCase = true) ->
                setConnectionState("Error", ConnectionTone.ERROR)
        }
    }

    override fun onReady() {
        downloadButton.isEnabled = true
        intervalButton.isEnabled = true
        infoRetryCount = 0
        setConnectionState("Connected", ConnectionTone.CONNECTED)
        connectButton.text = "Connected"
        connectButton.isEnabled = false
        setActivity("Secure BLE link ready • reading tracker status")
        armBleIdleDisconnect()

        // Let the nRF52840 restore encryption from the saved bond before the
        // first application-level command.
        activityText.postDelayed({
            requestTrackerInfo()
        }, 350)
    }

    private fun requestTrackerInfo() {
        val started = ble.send(Protocol.infoRequest())
        if (!started) {
            setConnectionState("Error", ConnectionTone.ERROR)
            setActivity("Could not read tracker status. Reconnect to retry.")
        }
    }

    private fun retryTrackerInfoAfterSecurity() {
        if (infoRetryCount >= 5) {
            setConnectionState("Error", ConnectionTone.ERROR)
            setActivity(
                "Bond exists, but encryption was not restored. Reset the XIAO owner bond and pair again."
            )
            return
        }

        infoRetryCount++
        setConnectionState("Securing", ConnectionTone.BUSY)
        setActivity("Restoring encrypted link • attempt $infoRetryCount/5")

        activityText.postDelayed({
            requestTrackerInfo()
        }, 700L * infoRetryCount)
    }

    override fun onNotification(data: ByteArray) {
        parser.feed(data).forEach(::handlePacket)
    }

    override fun onDisconnected() {
        parser.reset()
        infoRetryCount = 0
        pendingIntervalSeconds = null
        cancelBleIdleDisconnect()
        downloadButton.isEnabled = false
        intervalButton.isEnabled = false
        connectButton.text = "Connect"
        connectButton.isEnabled = true
        setConnectionState("Offline", ConnectionTone.NEUTRAL)
        setActivity("Tracker disconnected")
        progressBar.visibility = View.GONE
    }

    private fun handlePacket(packet: Packet) {
        Log.d(
            "XiaoGpsProtocol",
            "RX type=0x${packet.type.toString(16)} payload=${packet.payload.size}"
        )

        when (packet.type) {
            Protocol.RSP_INFO -> {
                val info = Protocol.parseInfo(packet.payload) ?: return
                infoRetryCount = 0
                deviceInfo = info
                showDeviceInfo(info)
            }

            Protocol.RSP_DATA_BATCH -> {
                armBleIdleDisconnect()
                val batch = Protocol.parseDataBatch(packet.payload)
                pendingDownload += batch

                val info = deviceInfo
                if (info != null && info.storedCount > 0) {
                    val localLast = store.lastSeq
                    val expectedAvailable = (info.newestSeq - localLast)
                        .coerceAtLeast(1)
                        .coerceAtMost(info.storedCount)
                    progressBar.progress =
                        ((pendingDownload.size * 100L) / expectedAvailable)
                            .toInt()
                            .coerceIn(0, 99)
                }
            }

            Protocol.RSP_DOWNLOAD_DONE -> {
                val sent = Protocol.parseU32(packet.payload) ?: pendingDownload.size.toLong()
                store.merge(pendingDownload)
                pendingDownload.clear()
                progressBar.progress = 100
                progressBar.visibility = View.GONE
                renderTrack()
                updateLocalStatus("Downloaded $sent new device records")
                ble.send(Protocol.infoRequest())
                armBleIdleDisconnect(20_000L)
            }

            Protocol.RSP_ACK -> {
                val command = packet.payload.firstOrNull()?.toInt()?.and(0xFF) ?: return

                when (command) {
                    Protocol.CMD_CLEAR_LOG_REQ -> {
                        store.clear()
                        pendingDownload.clear()
                        renderTrack()
                        updateLocalStatus("History cleared")
                        armBleIdleDisconnect(15_000L)
                    }

                    Protocol.CMD_SET_INTERVAL -> {
                        val applied = pendingIntervalSeconds
                        if (applied != null) {
                            finishIntervalUpdate(applied)
                        }
                    }
                }
            }

            Protocol.RSP_ERROR -> {
                activityText.removeCallbacks(intervalCommandTimeout)

                val code = packet.payload.firstOrNull()?.toInt()?.and(0xFF) ?: -1
                val message = if (packet.payload.size > 1) {
                    packet.payload.copyOfRange(1, packet.payload.size)
                        .toString(StandardCharsets.UTF_8)
                } else {
                    "Unknown"
                }

                progressBar.visibility = View.GONE

                if (code == 1) {
                    retryTrackerInfoAfterSecurity()
                } else {
                    if (pendingIntervalSeconds != null) {
                        pendingIntervalSeconds = null
                        deviceInfo?.logIntervalSeconds?.let { actual ->
                            intervalButton.text = formatInterval(actual)
                            intervalSummaryText.text = intervalSummary(actual)
                        }
                    }

                    val friendly = when (code) {
                        2 -> "Firmware does not support this command"
                        3 -> "Invalid command payload"
                        4 -> "Could not save the setting to tracker flash"
                        6 -> "Unsupported GPS interval"
                        else -> message
                    }

                    setConnectionState("Error", ConnectionTone.ERROR)
                    setActivity("XIAO error $code: $friendly")
                    toast(friendly)
                }
            }
        }
    }

    private fun showDeviceInfo(info: DeviceInfo) {
        val pct = if (info.capacity == 0L) 0 else
            ((info.storedCount * 100) / info.capacity).toInt()

        val lastFix = if (info.lastEpoch > 0) {
            timelineTimeFormatter.format(Instant.ofEpochSecond(info.lastEpoch))
        } else {
            "No saved fix yet"
        }

        val gpsStatus: String
        val gpsDetail: String

        if (info.powerOptimizationEnabled && !info.gpsPowered) {
            gpsText.setTextColor(getColor(R.color.text_primary))
            gpsStatus = "GPS sleeping"
            gpsDetail = buildString {
                append("Power save")
                if (info.gpsNextWakeSeconds > 0) {
                    append(" • next wake in ${info.gpsNextWakeSeconds}s")
                }
                append(" • ${formatInterval(info.logIntervalSeconds)} interval")
            }
        } else {
            gpsStatus = when {
                info.gpsCharsProcessed == 0L -> {
                    gpsText.setTextColor(getColor(R.color.danger))
                    "No GPS data"
                }

                !info.gpsLocationValid -> {
                    gpsText.setTextColor(getColor(R.color.warning))
                    "Searching for satellites"
                }

                !info.gpsDateValid || !info.gpsTimeValid -> {
                    gpsText.setTextColor(getColor(R.color.warning))
                    "Position found • waiting for UTC"
                }

                else -> {
                    gpsText.setTextColor(getColor(R.color.success))
                    String.format(
                        Locale.US,
                        "%.5f, %.5f",
                        info.gpsCurrentLatitude,
                        info.gpsCurrentLongitude
                    )
                }
            }

            gpsDetail = when {
                info.gpsCharsProcessed == 0L ->
                    "Check GPS power, GND, TX → D7/RX and 9600 baud"

                !info.gpsLocationValid ->
                    "NMEA ${info.gpsCharsProcessed} chars • ${info.gpsCurrentSatellites} satellites"

                !info.gpsDateValid || !info.gpsTimeValid ->
                    "${info.gpsCurrentSatellites} satellites • UTC not valid yet"

                else ->
                    "GPS ready • ${info.gpsCurrentSatellites} satellites • age ${info.gpsLocationAgeMs} ms"
            }
        }

        gpsText.text = gpsStatus
        storageText.text =
            "Device ${info.storedCount}/${info.capacity} ($pct%)  •  Phone ${store.records.size} points"

        val fwLabel = if (info.firmwareMajor > 0) {
            "FW ${info.firmwareVersion}"
        } else {
            "FW legacy"
        }
        versionText.text = "App v$APP_VERSION • $fwLabel"

        intervalButton.text = formatInterval(info.logIntervalSeconds)
        intervalSummaryText.text = intervalSummary(info.logIntervalSeconds)
        intervalButton.isEnabled = ble.isReady && supportsIntervalConfiguration(info)

        setActivity("$gpsDetail  •  Last saved: $lastFix")
        setConnectionState("Connected", ConnectionTone.CONNECTED)
        connectButton.text = "Connected"
        connectButton.isEnabled = false

        if (info.storedCount == 0L && ble.isReady) {
            activityText.postDelayed({
                if (ble.isReady && deviceInfo?.storedCount == 0L) {
                    requestTrackerInfo()
                }
            }, 5000)
        }
    }

    // ---------------- Map ----------------

    private fun setupMap(savedInstanceState: Bundle?) {
        mapView.onCreate(savedInstanceState)
        mapView.getMapAsync { mapLibreMap ->
            map = mapLibreMap

            // Do not use the OSM Foundation standard raster tile service directly from this native app.
            // OpenFreeMap provides an OSM-derived MapLibre vector style designed
            // for applications and currently requires no API key.
            val styleUrl =
                if (isNightMode()) OPENFREEMAP_DARK_STYLE_URL else OPENFREEMAP_STYLE_URL

            mapLibreMap.setStyle(styleUrl) {
                mapStyleReady = true
                renderTrack()
            }
        }
    }

    private fun renderTrack() {
        val m = map
        if (m == null || !mapStyleReady) {
            configureTimeline(selectLatest = selectedTimelineIndex < 0)
            return
        }

        trackPolyline?.let { m.removePolyline(it) }
        futureTrackPolyline?.let { m.removePolyline(it) }
        timelineMarker?.let { m.removeMarker(it) }
        trackPolyline = null
        futureTrackPolyline = null
        timelineMarker = null

        val sampled = store.sampleForMap()
        if (sampled.isEmpty()) {
            configureTimeline(selectLatest = true)
            return
        }

        val points = sampled.map { LatLng(it.latitude, it.longitude) }

        // Timeline selection draws the route as two time-aware segments:
        // travelled portion in accent, future portion muted.
        configureTimeline(selectLatest = selectedTimelineIndex < 0)

        mapView.post {
            if (points.size == 1) {
                m.animateCamera(
                    CameraUpdateFactory.newLatLngZoom(points.first(), 15.0)
                )
            } else {
                val builder = LatLngBounds.Builder()
                points.forEach(builder::include)
                runCatching {
                    m.animateCamera(
                        CameraUpdateFactory.newLatLngBounds(builder.build(), 64)
                    )
                }
            }
        }
    }


    private fun setupTimeline() {
        timelineSeekBar.setOnSeekBarChangeListener(
            object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(
                    seekBar: SeekBar?,
                    progress: Int,
                    fromUser: Boolean
                ) {
                    if (fromUser) {
                        selectTimelineProgress(progress, moveCamera = true)
                    }
                }

                override fun onStartTrackingTouch(seekBar: SeekBar?) = Unit
                override fun onStopTrackingTouch(seekBar: SeekBar?) = Unit
            }
        )
    }

    private fun configureTimeline(selectLatest: Boolean) {
        val records = store.records

        if (records.isEmpty()) {
            selectedTimelineIndex = -1
            timelineSeekBar.isEnabled = false
            timelineSeekBar.max = TIMELINE_SCALE
            timelineSeekBar.progress = 0
            timelineTimeText.text = "No track yet"
            timelineStartText.text = "Start"
            timelineEndText.text = "End"
            timelineDetailText.text = "Sync a track to navigate it in time"
            pointsBadge.text = "0 points"

            map?.let { m ->
                timelineMarker?.let { m.removeMarker(it) }
            }
            timelineMarker = null
            return
        }

        val firstEpoch = records.first().epoch
        val lastEpoch = records.last().epoch
        val duration = (lastEpoch - firstEpoch).coerceAtLeast(0L)

        timelineSeekBar.max = TIMELINE_SCALE
        timelineSeekBar.isEnabled = records.size > 1 && duration > 0

        selectedTimelineIndex = when {
            selectLatest -> records.lastIndex
            selectedTimelineIndex !in records.indices -> records.lastIndex
            else -> selectedTimelineIndex
        }

        timelineSeekBar.progress = progressForEpoch(
            records[selectedTimelineIndex].epoch,
            firstEpoch,
            lastEpoch
        )
        timelineStartText.text =
            timelineEndpointFormatter.format(Instant.ofEpochSecond(firstEpoch))
        timelineEndText.text =
            timelineEndpointFormatter.format(Instant.ofEpochSecond(lastEpoch))

        selectTimelinePoint(selectedTimelineIndex, moveCamera = false)
    }

    private fun selectTimelineProgress(progress: Int, moveCamera: Boolean) {
        val records = store.records
        if (records.isEmpty()) return

        val firstEpoch = records.first().epoch
        val lastEpoch = records.last().epoch
        val targetEpoch = if (lastEpoch <= firstEpoch) {
            firstEpoch
        } else {
            firstEpoch + ((lastEpoch - firstEpoch) * progress.coerceIn(0, TIMELINE_SCALE)) /
                TIMELINE_SCALE
        }

        selectTimelinePoint(nearestRecordIndex(targetEpoch, records), moveCamera)
    }

    private fun nearestRecordIndex(targetEpoch: Long, records: List<GpsRecord>): Int {
        var low = 0
        var high = records.lastIndex

        while (low <= high) {
            val mid = (low + high) ushr 1
            val value = records[mid].epoch
            when {
                value < targetEpoch -> low = mid + 1
                value > targetEpoch -> high = mid - 1
                else -> return mid
            }
        }

        val after = low.coerceIn(0, records.lastIndex)
        val before = (low - 1).coerceIn(0, records.lastIndex)
        return if (
            kotlin.math.abs(records[after].epoch - targetEpoch) <
            kotlin.math.abs(records[before].epoch - targetEpoch)
        ) after else before
    }

    private fun progressForEpoch(epoch: Long, firstEpoch: Long, lastEpoch: Long): Int {
        if (lastEpoch <= firstEpoch) return 0
        return (((epoch - firstEpoch).coerceIn(0L, lastEpoch - firstEpoch) * TIMELINE_SCALE) /
            (lastEpoch - firstEpoch)).toInt()
    }

    private fun selectTimelinePoint(index: Int, moveCamera: Boolean) {
        val records = store.records
        if (records.isEmpty()) return

        val safeIndex = index.coerceIn(0, records.lastIndex)
        selectedTimelineIndex = safeIndex
        val record = records[safeIndex]

        timelineTimeText.text =
            timelineTimeFormatter.format(Instant.ofEpochSecond(record.epoch))

        val elapsedSeconds = record.epoch - records.first().epoch
        val elapsedLabel = when {
            elapsedSeconds >= 3600 -> String.format(
                Locale.US,
                "+%dh %02dm",
                elapsedSeconds / 3600,
                (elapsedSeconds % 3600) / 60
            )
            elapsedSeconds >= 60 -> "+${elapsedSeconds / 60}m"
            else -> "+${elapsedSeconds}s"
        }

        timelineDetailText.text = String.format(
            Locale.US,
            "%s • %.6f, %.6f • %d sat • HDOP %.2f",
            elapsedLabel,
            record.latitude,
            record.longitude,
            record.satellites,
            record.hdop
        )

        pointsBadge.text = "${safeIndex + 1}/${records.size}"
        updateTimelineMarker(record, moveCamera)
    }

    private fun updateTimelineMarker(record: GpsRecord, moveCamera: Boolean) {
        val m = map ?: return
        if (!mapStyleReady) return

        timelineMarker?.let { m.removeMarker(it) }

        val position = LatLng(record.latitude, record.longitude)
        timelineMarker = m.addMarker(
            MarkerOptions()
                .position(position)
                .title(timelineTimeFormatter.format(Instant.ofEpochSecond(record.epoch)))
        )

        updateTimelineTrackSegments(record.epoch)

        if (moveCamera) {
            m.animateCamera(
                CameraUpdateFactory.newLatLngZoom(position, 16.0)
            )
        }
    }

    private fun updateTimelineTrackSegments(selectedEpoch: Long) {
        val m = map ?: return
        if (!mapStyleReady) return

        trackPolyline?.let { m.removePolyline(it) }
        futureTrackPolyline?.let { m.removePolyline(it) }
        trackPolyline = null
        futureTrackPolyline = null

        // Keep scrubbing responsive even with months of data.
        val sampled = store.sampleForMap(maxPoints = 1500)
        if (sampled.size < 2) return

        var split = sampled.indexOfLast { it.epoch <= selectedEpoch }
        if (split < 0) split = 0
        if (split >= sampled.lastIndex) split = sampled.lastIndex

        val travelled = sampled.subList(0, split + 1)
            .map { LatLng(it.latitude, it.longitude) }

        if (travelled.size >= 2) {
            trackPolyline = m.addPolyline(
                PolylineOptions()
                    .addAll(travelled)
                    .color(getColor(R.color.accent))
                    .width(5f)
            )
        }

        if (split < sampled.lastIndex) {
            val futureStart = if (split > 0) split else 0
            val future = sampled.subList(futureStart, sampled.size)
                .map { LatLng(it.latitude, it.longitude) }

            if (future.size >= 2) {
                futureTrackPolyline = m.addPolyline(
                    PolylineOptions()
                        .addAll(future)
                        .color(getColor(R.color.track_future))
                        .width(3f)
                )
            }
        }
    }

    private fun updateLocalStatus(prefix: String? = null) {
        val records = store.records

        if (records.isEmpty()) {
            storageText.text = "Phone archive • no downloaded points"
        } else {
            val first = records.first()
            val last = records.last()
            storageText.text = "Phone archive • ${records.size} points"
            setActivity(
                listOfNotNull(
                    prefix,
                    "${timelineEndpointFormatter.format(Instant.ofEpochSecond(first.epoch))} → " +
                        timelineEndpointFormatter.format(Instant.ofEpochSecond(last.epoch))
                ).joinToString(" • ")
            )
        }

        if (records.isEmpty() && prefix != null) {
            setActivity(prefix)
        }

        configureTimeline(selectLatest = prefix != null || selectedTimelineIndex < 0)
    }

    private fun toast(message: String) {
        Toast.makeText(this, message, Toast.LENGTH_SHORT).show()
    }

    // ---------------- Map lifecycle ----------------

    override fun onStart() {
        super.onStart()
        mapView.onStart()
    }

    override fun onResume() {
        super.onResume()
        mapView.onResume()
    }

    override fun onPause() {
        mapView.onPause()
        super.onPause()
    }

    override fun onStop() {
        mapView.onStop()
        super.onStop()
    }

    override fun onLowMemory() {
        super.onLowMemory()
        mapView.onLowMemory()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        mapView.onSaveInstanceState(outState)
    }

    override fun onDestroy() {
        cancelBleIdleDisconnect()
        ble.close()
        mapView.onDestroy()
        super.onDestroy()
    }

    companion object {
        private const val APP_VERSION = "1.5.1"
        // OSM-derived vector basemap from OpenFreeMap.
        // No API key or registration is required by the public instance.
        private const val PREFS_NAME = "xiao_tracker_prefs"
        private const val PREF_THEME_MODE = "theme_mode"
        private const val TIMELINE_SCALE = 10000

        private const val MENU_EXPORT_GPX = 1001
        private const val MENU_EXPORT_CSV = 1002
        private const val MENU_CLEAR_HISTORY = 1003

        private val TRACK_INTERVAL_SECONDS =
            intArrayOf(60, 900, 1800, 3600, 7200, 10800)

        private val TRACK_INTERVAL_LABELS =
            arrayOf(
                "1 minute · high power",
                "15 minutes",
                "30 minutes · recommended",
                "1 hour",
                "2 hours",
                "3 hours"
            )

        private const val OPENFREEMAP_STYLE_URL =
            "https://tiles.openfreemap.org/styles/liberty"

        private const val OPENFREEMAP_DARK_STYLE_URL =
            "https://tiles.openfreemap.org/styles/dark"
    }
}

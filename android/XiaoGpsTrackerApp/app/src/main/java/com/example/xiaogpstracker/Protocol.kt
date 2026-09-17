package com.example.xiaogpstracker

import java.nio.ByteBuffer
import java.nio.ByteOrder

data class Packet(val type: Int, val payload: ByteArray)

data class DeviceInfo(
    val storedCount: Long,
    val capacity: Long,
    val newestSeq: Long,
    val lastEpoch: Long,
    val lastLatE7: Int,
    val lastLonE7: Int,
    val lastHdopX100: Int,
    val lastSatellites: Int,

    val gpsCharsProcessed: Long,
    val gpsSentencesWithFix: Long,
    val gpsFailedChecksum: Long,
    val gpsCurrentLatE7: Int,
    val gpsCurrentLonE7: Int,
    val gpsCurrentHdopX100: Int,
    val gpsCurrentSatellites: Int,
    val gpsFlags: Int,
    val gpsLocationAgeMs: Long,

    // Release 1.0 power-state extension.
    val powerFlags: Int,
    val gpsNextWakeSeconds: Int,
    val logIntervalSeconds: Int,
    val batteryMillivolts: Int,
    val firmwareMajor: Int,
    val firmwareMinor: Int,
    val firmwarePatch: Int
) {
    val gpsLocationValid: Boolean get() = (gpsFlags and 0x01) != 0
    val gpsDateValid: Boolean get() = (gpsFlags and 0x02) != 0
    val gpsTimeValid: Boolean get() = (gpsFlags and 0x04) != 0
    val gpsCurrentLatitude: Double get() = gpsCurrentLatE7 / 10_000_000.0
    val gpsCurrentLongitude: Double get() = gpsCurrentLonE7 / 10_000_000.0

    val gpsPowered: Boolean get() = (powerFlags and 0x01) != 0
    val powerOptimizationEnabled: Boolean get() = (powerFlags and 0x02) != 0
    val gpsPowerControlEnabled: Boolean get() = (powerFlags and 0x04) != 0
    val flashSleeping: Boolean get() = (powerFlags and 0x08) != 0

    val firmwareVersion: String
        get() = "$firmwareMajor.$firmwareMinor.$firmwarePatch"
}

/**
 * The wake scheduler selected by the device.  The numeric values are part of
 * the BLE protocol and must not be changed without a protocol revision.
 */
enum class WakeMode(val wireValue: Int, val label: String) {
    INTERVAL(0, "Interval"),
    SMART(1, "Smart motion"),
    UNKNOWN(-1, "Unknown");

    companion object {
        fun fromWire(value: Int): WakeMode = values().firstOrNull { it.wireValue == value } ?: UNKNOWN
    }
}

/** Motion trigger sensitivity presets, ordered from most to least sensitive. */
enum class SmartSensitivity(
    val wireValue: Int,
    val label: String,
    val thresholdMilligrams: Int
) {
    HIGH(0, "High", 80),
    BALANCED(1, "Balanced", 160),
    LOW(2, "Low", 300),
    UNKNOWN(-1, "Unknown", 0);

    companion object {
        fun fromWire(value: Int): SmartSensitivity =
            values().firstOrNull { it.wireValue == value } ?: UNKNOWN
    }
}

/** The recording behavior used while Smart motion mode is enabled. */
enum class TrackingProfile(
    val wireValue: Int,
    val label: String,
    val description: String
) {
    CONTINUOUS(
        0,
        "Continuous",
        "Motion-triggered fixes every ~2 minutes while moving."
    ),
    POINT_TO_POINT(
        1,
        "Start & Stop",
        "One fix after confirmed movement starts and one after 10 minutes without " +
            "acceleration."
    ),
    UNKNOWN(-1, "Unknown", "The tracker reported an unsupported tracking profile.");

    companion object {
        fun fromWire(value: Int): TrackingProfile =
            values().firstOrNull { it.wireValue == value } ?: UNKNOWN
    }
}

/** State reported by the Smart Motion scheduler. */
enum class SmartMotionState(val wireValue: Int, val label: String) {
    DISABLED(0, "Disabled"),
    ARMED(1, "Armed"),
    VERIFYING(2, "Confirming motion"),
    ACQUIRING(3, "Waiting for GPS"),
    TRACKING(4, "GPS receiver active"),
    COOLDOWN(5, "Cooldown"),
    WAITING_FOR_STOP(6, "Waiting for stop"),
    ACQUIRING_STOP(7, "Acquiring stop GPS"),
    UNKNOWN(-1, "Unknown");

    companion object {
        fun fromWire(value: Int): SmartMotionState =
            values().firstOrNull { it.wireValue == value } ?: UNKNOWN
    }
}

/** Reason for the most recent Smart scheduler wake, as reported by firmware. */
enum class SmartWakeReason(val wireValue: Int, val label: String) {
    NONE(0, "None"),
    MOTION(1, "Motion"),
    RETRY(2, "Retry"),
    STOP(3, "Stop"),
    UNKNOWN(-1, "Unknown");

    companion object {
        fun fromWire(value: Int): SmartWakeReason =
            values().firstOrNull { it.wireValue == value } ?: UNKNOWN
    }
}

// Compatibility aliases keep the model discoverable to callers that use the
// terminology from the protocol/specification rather than the UI wording.
typealias SmartMode = WakeMode
typealias Sensitivity = SmartSensitivity
typealias MotionState = SmartMotionState
typealias WakeReason = SmartWakeReason

data class SmartInfo(
    val protocolVersion: Int,
    val mode: WakeMode,
    val sensitivity: SmartSensitivity,
    val motionState: SmartMotionState,
    val confirmationSeconds: Int,
    val standbySliceSeconds: Int,
    val fixCooldownSeconds: Int,
    val cooldownRemainingSeconds: Int,
    val flags: Int,
    val lastWakeReason: Int,
    val profile: TrackingProfile = TrackingProfile.CONTINUOUS
) {
    val modeValue: Int get() = mode.wireValue
    val sensitivityValue: Int get() = sensitivity.wireValue
    val motionStateValue: Int get() = motionState.wireValue
    val modeCode: Int get() = modeValue
    val sensitivityCode: Int get() = sensitivityValue
    val motionStateCode: Int get() = motionStateValue

    val wakeMode: WakeMode get() = mode
    val smartSensitivity: SmartSensitivity get() = sensitivity
    val trackingProfile: TrackingProfile get() = profile
    val profileValue: Int get() = profile.wireValue
    val trackingProfileValue: Int get() = profileValue
    val supportsProfiles: Boolean
        get() = protocolVersion == Protocol.SMART_INFO_PROTOCOL_V3
    val supportsTrackingProfiles: Boolean get() = supportsProfiles
    val wakeReason: SmartWakeReason get() = SmartWakeReason.fromWire(lastWakeReason)
    val smartWakeReason: SmartWakeReason get() = wakeReason

    val mpuPresent: Boolean get() = (flags and Protocol.SMART_FLAG_MPU_PRESENT) != 0
    val mpuInterruptArmed: Boolean get() = (flags and Protocol.SMART_FLAG_MPU_INTERRUPT_ARMED) != 0
    val cas12Verified: Boolean get() = (flags and Protocol.SMART_FLAG_CAS12_VERIFIED) != 0
    val cas12Active: Boolean get() = (flags and Protocol.SMART_FLAG_CAS12_ACTIVE) != 0
    val runtimeIntervalFallback: Boolean
        get() = (flags and Protocol.SMART_FLAG_RUNTIME_INTERVAL_FALLBACK) != 0
    val gpsReceiverActive: Boolean get() = (flags and Protocol.SMART_FLAG_GPS_RECEIVER_ACTIVE) != 0

    val mpuFault: Boolean get() = !mpuPresent || !mpuInterruptArmed
    val standbyUnavailable: Boolean get() = !cas12Verified
    val standbyActive: Boolean get() = cas12Active
    val runtimeFallback: Boolean get() = runtimeIntervalFallback
    val receiverActive: Boolean get() = gpsReceiverActive
}

object Protocol {
    const val CMD_INFO_REQ = 0x01
    const val CMD_DOWNLOAD_REQ = 0x02
    const val CMD_CLEAR_LOG_REQ = 0x03
    const val CMD_PING = 0x04
    const val CMD_SET_INTERVAL = 0x05
    const val CMD_GET_SMART_INFO = 0x06
    const val CMD_SET_SMART_CONFIG = 0x07

    const val RSP_INFO = 0x81
    const val RSP_DATA_BATCH = 0x82
    const val RSP_DOWNLOAD_DONE = 0x83
    const val RSP_ACK = 0x84
    const val RSP_SMART_INFO = 0x85
    const val RSP_ERROR = 0xFF

    const val SMART_INFO_PROTOCOL_V2 = 2
    const val SMART_INFO_PROTOCOL_V3 = 3
    const val SMART_INFO_V2_PAYLOAD_SIZE = 12
    const val SMART_INFO_V3_PAYLOAD_SIZE = 13
    const val SMART_WAKE_REASON_NONE = 0
    const val SMART_WAKE_REASON_MOTION = 1
    const val SMART_WAKE_REASON_RETRY = 2
    const val SMART_WAKE_REASON_STOP = 3

    // SmartInfo.flags bits.  There is intentionally no GPS power-off bit:
    // Smart Motion reports receiver/standby state, not a physical GPS gate.
    const val SMART_FLAG_MPU_PRESENT = 1 shl 0
    const val SMART_FLAG_MPU_INTERRUPT_ARMED = 1 shl 1
    const val SMART_FLAG_CAS12_VERIFIED = 1 shl 2
    const val SMART_FLAG_CAS12_ACTIVE = 1 shl 3
    const val SMART_FLAG_RUNTIME_INTERVAL_FALLBACK = 1 shl 4
    const val SMART_FLAG_GPS_RECEIVER_ACTIVE = 1 shl 5

    // Alternate long-form names are useful for protocol-focused callers.
    const val SMART_INFO_FLAG_MPU_PRESENT = SMART_FLAG_MPU_PRESENT
    const val SMART_INFO_FLAG_MPU_INTERRUPT_ARMED = SMART_FLAG_MPU_INTERRUPT_ARMED
    const val SMART_INFO_FLAG_CAS12_VERIFIED = SMART_FLAG_CAS12_VERIFIED
    const val SMART_INFO_FLAG_CAS12_ACTIVE = SMART_FLAG_CAS12_ACTIVE
    const val SMART_INFO_FLAG_RUNTIME_INTERVAL_FALLBACK = SMART_FLAG_RUNTIME_INTERVAL_FALLBACK
    const val SMART_INFO_FLAG_GPS_RECEIVER_ACTIVE = SMART_FLAG_GPS_RECEIVER_ACTIVE

    private const val MAGIC1 = 0xA5
    private const val MAGIC2 = 0x5A

    fun infoRequest() = encode(CMD_INFO_REQ)

    fun clearRequest() = encode(CMD_CLEAR_LOG_REQ)

    fun setIntervalRequest(seconds: Int): ByteArray {
        val payload = ByteBuffer.allocate(4)
            .order(ByteOrder.LITTLE_ENDIAN)
            .putInt(seconds)
            .array()
        return encode(CMD_SET_INTERVAL, payload)
    }

    fun smartInfoRequest(): ByteArray = encode(CMD_GET_SMART_INFO)

    // Descriptive alias for callers that use the command name directly.
    fun getSmartInfoRequest(): ByteArray = smartInfoRequest()

    fun setSmartConfigRequest(mode: WakeMode, sensitivity: SmartSensitivity): ByteArray =
        setSmartConfigRequest(mode.wireValue, sensitivity.wireValue)

    fun setSmartConfigRequest(
        mode: WakeMode,
        sensitivity: SmartSensitivity,
        profile: TrackingProfile
    ): ByteArray = setSmartConfigRequest(mode.wireValue, sensitivity.wireValue, profile.wireValue)

    fun setSmartConfigRequest(mode: Int, sensitivity: Int): ByteArray {
        require(mode == WakeMode.INTERVAL.wireValue || mode == WakeMode.SMART.wireValue) {
            "Unsupported wake mode: $mode"
        }
        require(sensitivity in 0..2) { "Unsupported Smart Motion sensitivity: $sensitivity" }
        return encode(
            CMD_SET_SMART_CONFIG,
            byteArrayOf(mode.toByte(), sensitivity.toByte())
        )
    }

    fun setSmartConfigRequest(mode: Int, sensitivity: Int, profile: Int): ByteArray {
        require(mode == WakeMode.INTERVAL.wireValue || mode == WakeMode.SMART.wireValue) {
            "Unsupported wake mode: $mode"
        }
        require(sensitivity in 0..2) { "Unsupported Smart Motion sensitivity: $sensitivity" }
        require(profile == TrackingProfile.CONTINUOUS.wireValue ||
            profile == TrackingProfile.POINT_TO_POINT.wireValue) {
            "Unsupported tracking profile: $profile"
        }
        return encode(
            CMD_SET_SMART_CONFIG,
            byteArrayOf(mode.toByte(), sensitivity.toByte(), profile.toByte())
        )
    }

    fun setSmartConfigRequest(
        mode: Int,
        sensitivity: Int,
        profile: TrackingProfile
    ): ByteArray = setSmartConfigRequest(mode, sensitivity, profile.wireValue)

    fun downloadRequest(afterSeq: Long): ByteArray {
        val payload = ByteBuffer.allocate(4)
            .order(ByteOrder.LITTLE_ENDIAN)
            .putInt(afterSeq.toInt())
            .array()
        return encode(CMD_DOWNLOAD_REQ, payload)
    }

    fun encode(type: Int, payload: ByteArray = byteArrayOf()): ByteArray {
        require(payload.size <= 0xFFFF)

        val len = payload.size
        val out = ByteArray(2 + 1 + 2 + len + 2)
        out[0] = MAGIC1.toByte()
        out[1] = MAGIC2.toByte()
        out[2] = type.toByte()
        out[3] = (len and 0xFF).toByte()
        out[4] = ((len ushr 8) and 0xFF).toByte()
        payload.copyInto(out, destinationOffset = 5)

        var crc = 0xFFFF
        crc = crc16Update(crc, type)
        crc = crc16Update(crc, len and 0xFF)
        crc = crc16Update(crc, (len ushr 8) and 0xFF)
        for (b in payload) crc = crc16Update(crc, b.toInt() and 0xFF)

        out[out.size - 2] = (crc and 0xFF).toByte()
        out[out.size - 1] = ((crc ushr 8) and 0xFF).toByte()
        return out
    }

    fun parseInfo(payload: ByteArray): DeviceInfo? {
        // Supported firmware INFO packet sizes:
        // 27 = original log fields
        // 55 = GPS diagnostics
        // 65 = release 1.0 GPS + power state + firmware version
        if (payload.size !in setOf(27, 55, 65)) return null

        val b = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)

        fun u32(): Long = b.int.toLong() and 0xFFFF_FFFFL

        val storedCount = u32()
        val capacity = u32()
        val newestSeq = u32()
        val lastEpoch = u32()
        val lastLatE7 = b.int
        val lastLonE7 = b.int
        val lastHdopX100 = b.short.toInt() and 0xFFFF
        val lastSatellites = b.get().toInt() and 0xFF

        var gpsCharsProcessed = 0L
        var gpsSentencesWithFix = 0L
        var gpsFailedChecksum = 0L
        var gpsCurrentLatE7 = 0
        var gpsCurrentLonE7 = 0
        var gpsCurrentHdopX100 = 0
        var gpsCurrentSatellites = 0
        var gpsFlags = 0
        var gpsLocationAgeMs = 0xFFFF_FFFFL

        if (payload.size >= 55) {
            gpsCharsProcessed = u32()
            gpsSentencesWithFix = u32()
            gpsFailedChecksum = u32()
            gpsCurrentLatE7 = b.int
            gpsCurrentLonE7 = b.int
            gpsCurrentHdopX100 = b.short.toInt() and 0xFFFF
            gpsCurrentSatellites = b.get().toInt() and 0xFF
            gpsFlags = b.get().toInt() and 0xFF
            gpsLocationAgeMs = u32()
        }

        var powerFlags = 0
        var gpsNextWakeSeconds = 0
        var logIntervalSeconds = 60
        var batteryMillivolts = 0
        var firmwareMajor = 0
        var firmwareMinor = 0
        var firmwarePatch = 0

        if (payload.size == 65) {
            powerFlags = b.get().toInt() and 0xFF
            gpsNextWakeSeconds = b.short.toInt() and 0xFFFF
            logIntervalSeconds = b.short.toInt() and 0xFFFF
            batteryMillivolts = b.short.toInt() and 0xFFFF
            firmwareMajor = b.get().toInt() and 0xFF
            firmwareMinor = b.get().toInt() and 0xFF
            firmwarePatch = b.get().toInt() and 0xFF
        }

        return DeviceInfo(
            storedCount = storedCount,
            capacity = capacity,
            newestSeq = newestSeq,
            lastEpoch = lastEpoch,
            lastLatE7 = lastLatE7,
            lastLonE7 = lastLonE7,
            lastHdopX100 = lastHdopX100,
            lastSatellites = lastSatellites,
            gpsCharsProcessed = gpsCharsProcessed,
            gpsSentencesWithFix = gpsSentencesWithFix,
            gpsFailedChecksum = gpsFailedChecksum,
            gpsCurrentLatE7 = gpsCurrentLatE7,
            gpsCurrentLonE7 = gpsCurrentLonE7,
            gpsCurrentHdopX100 = gpsCurrentHdopX100,
            gpsCurrentSatellites = gpsCurrentSatellites,
            gpsFlags = gpsFlags,
            gpsLocationAgeMs = gpsLocationAgeMs,
            powerFlags = powerFlags,
            gpsNextWakeSeconds = gpsNextWakeSeconds,
            logIntervalSeconds = logIntervalSeconds,
            batteryMillivolts = batteryMillivolts,
            firmwareMajor = firmwareMajor,
            firmwareMinor = firmwareMinor,
            firmwarePatch = firmwarePatch
        )
    }

    /**
     * Parse a versioned little-endian RSP_SMART_INFO payload.
     *
     * Version 2 is exactly 12 bytes and predates tracking profiles, so it is
     * represented as Continuous. Version 3 appends profile at byte 12.
     * Rejecting every other version/length pair prevents a future payload from
     * being silently interpreted with the wrong offsets.
     */
    fun parseSmartInfo(payload: ByteArray): SmartInfo? {
        if (payload.size != SMART_INFO_V2_PAYLOAD_SIZE &&
            payload.size != SMART_INFO_V3_PAYLOAD_SIZE
        ) return null

        val b = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
        val protocolVersion = b.get().toInt() and 0xFF
        when (protocolVersion) {
            SMART_INFO_PROTOCOL_V2 -> if (payload.size != SMART_INFO_V2_PAYLOAD_SIZE) return null
            SMART_INFO_PROTOCOL_V3 -> if (payload.size != SMART_INFO_V3_PAYLOAD_SIZE) return null
            else -> return null
        }
        val mode = WakeMode.fromWire(b.get().toInt() and 0xFF)
        val sensitivity = SmartSensitivity.fromWire(b.get().toInt() and 0xFF)
        val motionState = SmartMotionState.fromWire(b.get().toInt() and 0xFF)
        val confirmationSeconds = b.get().toInt() and 0xFF
        val standbySliceSeconds = b.get().toInt() and 0xFF
        val fixCooldownSeconds = b.short.toInt() and 0xFFFF
        val cooldownRemainingSeconds = b.short.toInt() and 0xFFFF
        val flags = b.get().toInt() and 0xFF
        val lastWakeReason = b.get().toInt() and 0xFF
        val profile = if (protocolVersion == SMART_INFO_PROTOCOL_V3) {
            TrackingProfile.fromWire(b.get().toInt() and 0xFF)
        } else {
            TrackingProfile.CONTINUOUS
        }

        return SmartInfo(
            protocolVersion = protocolVersion,
            mode = mode,
            sensitivity = sensitivity,
            motionState = motionState,
            confirmationSeconds = confirmationSeconds,
            standbySliceSeconds = standbySliceSeconds,
            fixCooldownSeconds = fixCooldownSeconds,
            cooldownRemainingSeconds = cooldownRemainingSeconds,
            flags = flags,
            lastWakeReason = lastWakeReason,
            profile = profile
        )
    }

    fun parseDataBatch(payload: ByteArray): List<GpsRecord> {
        if (payload.isEmpty()) return emptyList()
        val count = payload[0].toInt() and 0xFF
        if (payload.size != 1 + count * GpsRecord.SIZE) return emptyList()

        return buildList {
            repeat(count) { i ->
                GpsRecord.fromDeviceBytes(payload, 1 + i * GpsRecord.SIZE)?.let(::add)
            }
        }
    }

    fun parseU32(payload: ByteArray): Long? {
        if (payload.size != 4) return null
        return ByteBuffer.wrap(payload)
            .order(ByteOrder.LITTLE_ENDIAN)
            .int.toLong() and 0xFFFF_FFFFL
    }

    fun crc16Update(start: Int, value: Int): Int {
        var crc = start xor ((value and 0xFF) shl 8)
        repeat(8) {
            crc = if ((crc and 0x8000) != 0) {
                ((crc shl 1) xor 0x1021) and 0xFFFF
            } else {
                (crc shl 1) and 0xFFFF
            }
        }
        return crc
    }
}

class PacketParser {
    private var buffer = ByteArray(0)

    fun feed(data: ByteArray): List<Packet> {
        buffer += data
        val packets = mutableListOf<Packet>()

        while (true) {
            // Synchronize to magic A5 5A.
            var start = -1
            for (i in 0 until buffer.size - 1) {
                if ((buffer[i].toInt() and 0xFF) == 0xA5 &&
                    (buffer[i + 1].toInt() and 0xFF) == 0x5A
                ) {
                    start = i
                    break
                }
            }

            if (start < 0) {
                buffer = if (buffer.lastOrNull()?.toInt()?.and(0xFF) == 0xA5) {
                    byteArrayOf(0xA5.toByte())
                } else {
                    byteArrayOf()
                }
                break
            }

            if (start > 0) buffer = buffer.copyOfRange(start, buffer.size)
            if (buffer.size < 7) break

            val type = buffer[2].toInt() and 0xFF
            val len = (buffer[3].toInt() and 0xFF) or
                ((buffer[4].toInt() and 0xFF) shl 8)
            val total = 2 + 1 + 2 + len + 2

            if (len > 4096) {
                buffer = buffer.copyOfRange(1, buffer.size)
                continue
            }

            if (buffer.size < total) break

            val payload = buffer.copyOfRange(5, 5 + len)
            val received = (buffer[total - 2].toInt() and 0xFF) or
                ((buffer[total - 1].toInt() and 0xFF) shl 8)

            var crc = 0xFFFF
            crc = Protocol.crc16Update(crc, type)
            crc = Protocol.crc16Update(crc, len and 0xFF)
            crc = Protocol.crc16Update(crc, (len ushr 8) and 0xFF)
            for (b in payload) crc = Protocol.crc16Update(crc, b.toInt() and 0xFF)

            if (crc == received) {
                packets += Packet(type, payload)
                buffer = buffer.copyOfRange(total, buffer.size)
            } else {
                // Bad CRC: drop one byte and look for the next frame.
                buffer = buffer.copyOfRange(1, buffer.size)
            }
        }

        return packets
    }

    fun reset() {
        buffer = byteArrayOf()
    }
}

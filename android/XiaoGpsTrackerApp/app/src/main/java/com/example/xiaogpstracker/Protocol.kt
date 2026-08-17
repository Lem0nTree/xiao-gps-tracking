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

object Protocol {
    const val CMD_INFO_REQ = 0x01
    const val CMD_DOWNLOAD_REQ = 0x02
    const val CMD_CLEAR_LOG_REQ = 0x03
    const val CMD_PING = 0x04
    const val CMD_SET_INTERVAL = 0x05

    const val RSP_INFO = 0x81
    const val RSP_DATA_BATCH = 0x82
    const val RSP_DOWNLOAD_DONE = 0x83
    const val RSP_ACK = 0x84
    const val RSP_ERROR = 0xFF

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
